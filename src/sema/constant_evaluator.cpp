#include "constant_evaluator.hpp"

#include "bound_nodes.hpp"
#include "symbol.hpp"
#include <set>

namespace sema {
namespace {

bool isConstantExpression(const BoundExpression &expression,
                          std::set<const VariableSymbol *> &resolving,
                          std::string *failureReason) {
  auto reject = [&](const std::string &reason) {
    if (failureReason) {
      *failureReason = reason;
    }
    return false;
  };

  auto isGlobalAddress = [](const BoundExpression &place) {
    if (const auto *variable =
            dynamic_cast<const BoundVariableExpression *>(&place)) {
      return variable->symbol && variable->symbol->is_global;
    }
    if (const auto *index = dynamic_cast<const BoundIndexAccess *>(&place)) {
      const auto *variable =
          dynamic_cast<const BoundVariableExpression *>(index->left.get());
      return variable && variable->symbol && variable->symbol->is_global;
    }
    return false;
  };

  if (dynamic_cast<const BoundLiteral *>(&expression)) {
    return true;
  }

  if (const auto *variable =
          dynamic_cast<const BoundVariableExpression *>(&expression)) {
    const auto &symbol = variable->symbol;
    if (!symbol || !symbol->isCompileTimeConstant()) {
      return reject("it reads a runtime binding");
    }
    if (!symbol->constant_value) {
      return reject("it depends on a constant that has not been evaluated yet");
    }
    if (!resolving.insert(symbol.get()).second) {
      return reject("it contains a cycle between constants");
    }
    const bool valid =
        isConstantExpression(*symbol->constant_value, resolving, failureReason);
    resolving.erase(symbol.get());
    return valid;
  }

  if (const auto *unary = dynamic_cast<const BoundUnaryExpression *>(&expression)) {
    if (unary->op == "&") {
      if (isGlobalAddress(*unary->expr)) {
        return true;
      }
      return reject("it takes the address of runtime storage");
    }
    if (unary->op == "++" || unary->op == "--") {
      return reject("it performs a runtime address or mutation operation");
    }
    return isConstantExpression(*unary->expr, resolving, failureReason);
  }

  if (const auto *binary = dynamic_cast<const BoundBinaryExpression *>(&expression)) {
    return isConstantExpression(*binary->left, resolving, failureReason) &&
           isConstantExpression(*binary->right, resolving, failureReason);
  }

  if (const auto *ternary = dynamic_cast<const BoundTernaryExpression *>(&expression)) {
    (void)ternary;
    return reject("conditional expressions are not supported in constants");
  }

  if (const auto *cast = dynamic_cast<const BoundCast *>(&expression)) {
    return isConstantExpression(*cast->expression, resolving, failureReason);
  }

  if (const auto *array = dynamic_cast<const BoundArrayLiteral *>(&expression)) {
    for (const auto &element : array->elements) {
      if (!isConstantExpression(*element, resolving, failureReason)) {
        return false;
      }
    }
    return true;
  }

  if (const auto *record = dynamic_cast<const BoundStructLiteral *>(&expression)) {
    for (const auto &field : record->fields) {
      if (!isConstantExpression(*field.second, resolving, failureReason)) {
        return false;
      }
    }
    return true;
  }

  return reject("it is not a compile-time expression");
}

} // namespace

bool ConstantEvaluator::isConstant(const BoundExpression &expression,
                                   std::string *failureReason) {
  std::set<const VariableSymbol *> resolving;
  return isConstantExpression(expression, resolving, failureReason);
}

} // namespace sema
