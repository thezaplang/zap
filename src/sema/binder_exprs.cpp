#include "../ast/class_decl.hpp"
#include "../ast/const/const_char.hpp"
#include "../ast/record_decl.hpp"
#include "../utils/string_type_utils.hpp"
#include "binder.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <functional>
#include <limits>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace sema {
namespace {

struct TypeLayout {
  uint64_t size = 0;
  uint64_t align = 1;
};

uint64_t alignTo(uint64_t value, uint64_t alignment) {
  if (alignment <= 1) {
    return value;
  }
  return ((value + alignment - 1) / alignment) * alignment;
}

TypeLayout layoutOfType(const std::shared_ptr<zir::Type> &type);

TypeLayout layoutOfAggregateField(const std::shared_ptr<zir::Type> &type) {
  if (type && type->getKind() == zir::TypeKind::Void) {
    return {1, 1};
  }
  return layoutOfType(type);
}

TypeLayout layoutOfRecord(const zir::RecordType &record) {
  if (zap::text::isStringRecordName(record.getName())) {
    return {16, 8};
  }

  uint64_t offset = 0;
  uint64_t maxAlign = 1;
  for (const auto &field : record.getFields()) {
    auto fieldLayout = layoutOfAggregateField(field.type);
    maxAlign = std::max(maxAlign, fieldLayout.align);
    if (!record.isPacked) {
      offset = alignTo(offset, fieldLayout.align);
    }
    offset += fieldLayout.size;
  }

  if (!record.isPacked) {
    offset = alignTo(offset, maxAlign);
  }
  return {offset, record.isPacked ? 1 : maxAlign};
}

TypeLayout layoutOfTaggedUnion(const zir::TaggedUnionType &taggedUnion) {
  TypeLayout payloadLayout{1, 1};
  for (const auto &variant : taggedUnion.getVariants()) {
    if (!variant.payloadType) {
      continue;
    }
    auto candidate = layoutOfAggregateField(variant.payloadType);
    if (candidate.size > payloadLayout.size ||
        (candidate.size == payloadLayout.size &&
         candidate.align > payloadLayout.align)) {
      payloadLayout = candidate;
    }
  }

  uint64_t offset = 4;
  const uint64_t structAlign = std::max<uint64_t>(4, payloadLayout.align);
  offset = alignTo(offset, payloadLayout.align);
  offset += payloadLayout.size;
  return {alignTo(offset, structAlign), structAlign};
}

TypeLayout layoutOfType(const std::shared_ptr<zir::Type> &type) {
  if (!type) {
    return {0, 1};
  }

  switch (type->getKind()) {
  case zir::TypeKind::Void:
    return {0, 1};
  case zir::TypeKind::Bool:
  case zir::TypeKind::Char:
  case zir::TypeKind::Int8:
  case zir::TypeKind::UInt8:
    return {1, 1};
  case zir::TypeKind::Int16:
  case zir::TypeKind::UInt16:
    return {2, 2};
  case zir::TypeKind::Int32:
  case zir::TypeKind::UInt32:
  case zir::TypeKind::Float:
  case zir::TypeKind::Float32:
    return {4, 4};
  case zir::TypeKind::Int:
  case zir::TypeKind::UInt:
  case zir::TypeKind::Int64:
  case zir::TypeKind::UInt64:
  case zir::TypeKind::Float64:
  case zir::TypeKind::Pointer:
  case zir::TypeKind::NullPtr:
  case zir::TypeKind::FunctionPointer:
  case zir::TypeKind::Class:
    return {8, 8};
  case zir::TypeKind::Enum:
    return std::static_pointer_cast<zir::EnumType>(type)->hasReprC
               ? TypeLayout{4, 4}
               : TypeLayout{8, 8};
  case zir::TypeKind::Record:
    return layoutOfRecord(*std::static_pointer_cast<zir::RecordType>(type));
  case zir::TypeKind::TaggedUnion:
    return layoutOfTaggedUnion(
        *std::static_pointer_cast<zir::TaggedUnionType>(type));
  case zir::TypeKind::Array: {
    auto array = std::static_pointer_cast<zir::ArrayType>(type);
    auto elementLayout = layoutOfAggregateField(array->getBaseType());
    return {elementLayout.size * array->getSize(), elementLayout.align};
  }
  }

  return {0, 1};
}

} // namespace

std::unique_ptr<BoundExpression>
Binder::bindExpressionWithExpected(ExpressionNode *expr,
                                   std::shared_ptr<zir::Type> expectedType) {
  if (!expr) {
    return nullptr;
  }

  expectedExpressionTypes_.push_back(std::move(expectedType));
  expr->accept(*this);
  expectedExpressionTypes_.pop_back();

  if (expressionStack_.empty()) {
    return nullptr;
  }

  auto boundExpr = std::move(expressionStack_.top());
  expressionStack_.pop();
  return boundExpr;
}

std::shared_ptr<zir::Type> Binder::currentExpectedExpressionType() const {
  if (expectedExpressionTypes_.empty()) {
    return nullptr;
  }
  return expectedExpressionTypes_.back();
}

void Binder::visit(BinExpr &node) {
  std::unique_ptr<BoundExpression> left;
  std::unique_ptr<BoundExpression> right;

  bool leftIsNullLiteral =
      dynamic_cast<ConstNull *>(node.left_.get()) != nullptr;
  bool rightIsNullLiteral =
      dynamic_cast<ConstNull *>(node.right_.get()) != nullptr;

  if (leftIsNullLiteral && !rightIsNullLiteral) {
    right = bindExpressionWithExpected(node.right_.get(), nullptr);
    if (!right)
      return;
    left = bindExpressionWithExpected(node.left_.get(), right->type);
    if (!left)
      return;
  } else if (rightIsNullLiteral && !leftIsNullLiteral) {
    left = bindExpressionWithExpected(node.left_.get(), nullptr);
    if (!left)
      return;
    right = bindExpressionWithExpected(node.right_.get(), left->type);
    if (!right)
      return;
  } else {
    node.left_->accept(*this);
    if (expressionStack_.empty())
      return;
    left = std::move(expressionStack_.top());
    expressionStack_.pop();

    node.right_->accept(*this);
    if (expressionStack_.empty())
      return;
    right = std::move(expressionStack_.top());
    expressionStack_.pop();
  }

  expressionStack_.push(
      buildBinaryExpression(std::move(left), node.op_, std::move(right),
                            node.left_->span, node.right_->span));
}

std::unique_ptr<BoundExpression>
Binder::buildBinaryExpression(std::unique_ptr<BoundExpression> left,
                              const std::string &op,
                              std::unique_ptr<BoundExpression> right,
                              SourceSpan leftSpan, SourceSpan rightSpan) {
  auto leftType = left->type;
  auto rightType = right->type;
  std::shared_ptr<zir::Type> resultType = leftType;

  if (op == "+" &&
      ((isStringType(leftType) || leftType->getKind() == zir::TypeKind::Char) ||
       (isStringType(rightType) ||
        rightType->getKind() == zir::TypeKind::Char))) {
    bool leftOk =
        isStringType(leftType) || leftType->getKind() == zir::TypeKind::Char;
    bool rightOk =
        isStringType(rightType) || rightType->getKind() == zir::TypeKind::Char;
    bool hasString = isStringType(leftType) || isStringType(rightType);

    if (!leftOk || !rightOk || !hasString) {
      error(SourceSpan::merge(leftSpan, rightSpan),
            "Concatenation requires String and/or Char operands with at least "
            "one String, got '" +
                renderTypeForUser(leftType) + "' and '" +
                renderTypeForUser(rightType) + "'");
    }
    resultType = std::make_shared<zir::RecordType>("String", "String");
  } else if ((op == "+" || op == "-") &&
             (isPointerType(leftType) || isPointerType(rightType))) {
    if (op == "+" && isPointerType(leftType) && rightType->isInteger()) {
      resultType = leftType;
    } else if (op == "+" && leftType->isInteger() && isPointerType(rightType)) {
      std::swap(left, right);
      std::swap(leftType, rightType);
      resultType = leftType;
    } else if (op == "-" && isPointerType(leftType) && rightType->isInteger()) {
      resultType = leftType;
    } else if (op == "-" && isPointerType(leftType) &&
               isPointerType(rightType)) {
      if (leftType->toString() != rightType->toString()) {
        error(SourceSpan::merge(leftSpan, rightSpan),
              "Pointer subtraction requires operands of the same type.");
      }
      resultType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int);
    } else {
      error(SourceSpan::merge(leftSpan, rightSpan),
            "Invalid pointer arithmetic between '" +
                renderTypeForUser(leftType) + "' and '" +
                renderTypeForUser(rightType) + "'");
    }
  } else if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%") {
    if (!isNumeric(leftType) || !isNumeric(rightType)) {
      error(SourceSpan::merge(leftSpan, rightSpan),
            "Operator '" + op + "' cannot be applied to '" +
                renderTypeForUser(leftType) + "' and '" +
                renderTypeForUser(rightType) + "'");
    } else {
      resultType = getPromotedType(leftType, rightType);
      left = wrapInCast(std::move(left), resultType);
      right = wrapInCast(std::move(right), resultType);
    }
  } else if (op == "&" || op == "|" || op == "^") {
    if (!leftType->isInteger() || !rightType->isInteger()) {
      error(SourceSpan::merge(leftSpan, rightSpan),
            "Bitwise operator '" + op + "' requires integer operands, got '" +
                renderTypeForUser(leftType) + "' and '" +
                renderTypeForUser(rightType) + "'");
    } else {
      resultType = getPromotedType(leftType, rightType);
      left = wrapInCast(std::move(left), resultType);
      right = wrapInCast(std::move(right), resultType);
    }
  } else if (op == "<<" || op == ">>") {
    if (!leftType->isInteger() || !rightType->isInteger()) {
      error(SourceSpan::merge(leftSpan, rightSpan),
            "Shift operator '" + op + "' requires integer operands, got '" +
                renderTypeForUser(leftType) + "' and '" +
                renderTypeForUser(rightType) + "'");
    } else {
      // Keep the left-hand integer type for shift results.
      resultType = leftType;
      right = wrapInCast(std::move(right), resultType);

      if (auto shiftAmount = evaluateConstantInt(right.get())) {
        if (*shiftAmount < 0) {
          error(SourceSpan::merge(leftSpan, rightSpan),
                "Shift amount must be non-negative, got '" +
                    std::to_string(*shiftAmount) + "'.");
        } else {
          unsigned width = static_cast<unsigned>(typeBitWidth(resultType));
          if (width == 0 || static_cast<uint64_t>(*shiftAmount) >= width) {
            error(SourceSpan::merge(leftSpan, rightSpan),
                  "Shift amount '" + std::to_string(*shiftAmount) +
                      "' is out of range for type '" +
                      renderTypeForUser(resultType) + "' (" +
                      std::to_string(width) + "-bit width).");
          }
        }
      }
    }
  } else if (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" ||
             op == ">=") {
    bool classOrNullComparison =
        (leftType->getKind() == zir::TypeKind::Class &&
         isNullType(rightType)) ||
        (rightType->getKind() == zir::TypeKind::Class && isNullType(leftType));
    if (!classOrNullComparison &&
        (isPointerType(leftType) || isPointerType(rightType) ||
         isNullType(leftType) || isNullType(rightType))) {
    }

    bool stringComparison = isStringType(leftType) && isStringType(rightType) &&
                            (op == "==" || op == "!=");

    // Reject comparisons of struct types except String/StringView equality.
    if (!stringComparison && (leftType->getKind() == zir::TypeKind::Record ||
                              rightType->getKind() == zir::TypeKind::Record)) {
      error(SourceSpan::merge(leftSpan, rightSpan),
            "Cannot compare struct types '" + renderTypeForUser(leftType) +
                "' and '" + renderTypeForUser(rightType) + "'");
    }

    if (!canConvert(leftType, rightType) && !canConvert(rightType, leftType)) {
      error(SourceSpan::merge(leftSpan, rightSpan),
            "Cannot compare '" + renderTypeForUser(leftType) + "' and '" +
                renderTypeForUser(rightType) + "'");
    } else if (isNullType(leftType) && isPointerType(rightType)) {
      left = wrapInCast(std::move(left), rightType);
    } else if (isNullType(rightType) && isPointerType(leftType)) {
      right = wrapInCast(std::move(right), leftType);
    } else if (isNullType(leftType) &&
               rightType->getKind() == zir::TypeKind::Class) {
      left = wrapInCast(std::move(left), rightType);
    } else if (isNullType(rightType) &&
               leftType->getKind() == zir::TypeKind::Class) {
      right = wrapInCast(std::move(right), leftType);
    } else if ((leftType->isInteger() && rightType->isInteger()) ||
               (leftType->isFloatingPoint() && rightType->isFloatingPoint()) ||
               (leftType->isInteger() && rightType->isFloatingPoint()) ||
               (leftType->isFloatingPoint() && rightType->isInteger())) {
      auto promotedType = getPromotedType(leftType, rightType);
      left = wrapInCast(std::move(left), promotedType);
      right = wrapInCast(std::move(right), promotedType);
    } else if (stringComparison) {
      auto stringViewType =
          std::make_shared<zir::RecordType>("StringView", "StringView");
      left = wrapInCast(std::move(left), stringViewType);
      right = wrapInCast(std::move(right), stringViewType);
    }
    resultType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Bool);
  } else if (op == "&&" || op == "||") {
    if (leftType->getKind() != zir::TypeKind::Bool ||
        rightType->getKind() != zir::TypeKind::Bool) {
      error(SourceSpan::merge(leftSpan, rightSpan),
            "Logical operator '" + op + "' requires Bool operands.");
    }
    resultType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Bool);
  }

  auto result = std::make_unique<BoundBinaryExpression>(
      std::move(left), op, std::move(right), resultType);

  if (auto folded = foldConstantBinary(result.get()))
    return folded;

  return result;
}

void Binder::visit(TernaryExpr &node) {
  node.condition_->accept(*this);
  if (expressionStack_.empty())
    return;
  auto condition = std::move(expressionStack_.top());
  expressionStack_.pop();

  if (condition->type->getKind() != zir::TypeKind::Bool) {
    error(node.condition_->span, "Ternary condition must be Bool, got '" +
                                     renderTypeForUser(condition->type) + "'");
  }

  node.thenExpr_->accept(*this);
  if (expressionStack_.empty())
    return;
  auto thenExpr = std::move(expressionStack_.top());
  expressionStack_.pop();

  node.elseExpr_->accept(*this);
  if (expressionStack_.empty())
    return;
  auto elseExpr = std::move(expressionStack_.top());
  expressionStack_.pop();

  if (!canConvert(thenExpr->type, elseExpr->type) &&
      !canConvert(elseExpr->type, thenExpr->type)) {
    error(SourceSpan::merge(node.thenExpr_->span, node.elseExpr_->span),
          "Ternary branches must be compatible, got '" +
              renderTypeForUser(thenExpr->type) + "' and '" +
              renderTypeForUser(elseExpr->type) + "'");
  }

  auto resultType = canConvert(thenExpr->type, elseExpr->type) ? elseExpr->type
                                                               : thenExpr->type;
  thenExpr = wrapInCast(std::move(thenExpr), resultType);
  elseExpr = wrapInCast(std::move(elseExpr), resultType);

  expressionStack_.push(std::make_unique<BoundTernaryExpression>(
      std::move(condition), std::move(thenExpr), std::move(elseExpr),
      resultType));
}

void Binder::visit(ConstInt &node) {
  expressionStack_.push(std::make_unique<BoundLiteral>(
      node.value_, std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int)));
}

void Binder::visit(ConstFloat &node) {
  expressionStack_.push(std::make_unique<BoundLiteral>(
      std::to_string(node.value_),
      std::make_shared<zir::PrimitiveType>(zir::TypeKind::Float)));
}

void Binder::visit(ConstString &node) {
  expressionStack_.push(std::make_unique<BoundLiteral>(
      node.value_,
      std::make_shared<zir::RecordType>("StringView", "StringView")));
}

void Binder::visit(ConstChar &node) {
  expressionStack_.push(std::make_unique<BoundLiteral>(
      node.value_, std::make_shared<zir::PrimitiveType>(zir::TypeKind::Char)));
}

void Binder::visit(ConstNull &node) {
  auto expectedType = currentExpectedExpressionType();
  bool nullIsSafeHere =
      expectedType && expectedType->getKind() == zir::TypeKind::Class;

  if (!nullIsSafeHere) {
  }
  expressionStack_.push(std::make_unique<BoundLiteral>(
      "0", std::make_shared<zir::PrimitiveType>(zir::TypeKind::NullPtr)));
}

void Binder::visit(CastExpr &node) {
  node.expr_->accept(*this);
  if (expressionStack_.empty())
    return;

  auto expr = std::move(expressionStack_.top());
  expressionStack_.pop();

  auto targetType = mapType(*node.type_);
  if (!targetType) {
    error(node.type_->span, "Unknown type: " + node.type_->qualifiedName());
    return;
  }

  bool castAllowed = false;
  auto sourceKind = expr->type->getKind();
  auto targetKind = targetType->getKind();
  bool sourceIsChar = sourceKind == zir::TypeKind::Char;
  bool targetIsChar = targetKind == zir::TypeKind::Char;

  if (isNumeric(expr->type) && isNumeric(targetType))
    castAllowed = true;
  else if ((sourceIsChar && targetType->isInteger()) ||
           (expr->type->isInteger() && targetIsChar))
    castAllowed = true;
  else if (sourceKind == zir::TypeKind::Enum && targetType->isInteger())
    castAllowed = true;
  else if ((isPointerType(expr->type) || isNullType(expr->type)) &&
           isPointerType(targetType))
    castAllowed = true;
  else if (isStringType(expr->type) && isPointerType(targetType))
    castAllowed = true;
  else if (isStringType(expr->type) && isStringType(targetType))
    castAllowed = true;
  else if (isPointerType(expr->type) && isStringType(targetType)) {
    auto ptrType = std::static_pointer_cast<zir::PointerType>(expr->type);
    auto baseKind = ptrType->getBaseType()->getKind();
    castAllowed =
        baseKind == zir::TypeKind::Void || baseKind == zir::TypeKind::Char;
  } else if (isPointerType(expr->type) && targetType->isInteger())
    castAllowed = true;
  else if (expr->type->isInteger() && isPointerType(targetType))
    castAllowed = true;

  if (!castAllowed) {
    error(node.span, "Cannot cast from '" + renderTypeForUser(expr->type) +
                         "' to '" + renderTypeForUser(targetType) + "'");
    return;
  }

  expressionStack_.push(
      std::make_unique<BoundCast>(std::move(expr), targetType));
}

void Binder::visit(TryExpr &node) {
  node.expression_->accept(*this);
  if (expressionStack_.empty()) {
    return;
  }

  auto expression = std::move(expressionStack_.top());
  expressionStack_.pop();

  if (!isFailableType(expression->type)) {
    error(node.span, "Operator '?' requires failable expression type, got '" +
                         renderTypeForUser(expression->type) + "'");
    return;
  }

  auto valueType = failableValueType(expression->type);
  auto errorType = failableErrorType(expression->type);

  if (!currentFunction_ || !isFailableType(currentFunction_->returnType)) {
    error(node.span, "Operator '?' can only be used inside functions returning "
                     "failable type.");
    return;
  }

  auto currentErrorType = failableErrorType(currentFunction_->returnType);
  if (!currentErrorType || !errorType ||
      currentErrorType->toString() != errorType->toString()) {
    error(node.span, "Cannot propagate error type '" +
                         renderTypeForUser(errorType) +
                         "' into function error type '" +
                         renderTypeForUser(currentErrorType) +
                         "': exact error type match is required for '?'.");
    return;
  }

  expressionStack_.push(std::make_unique<BoundTryExpression>(
      std::move(expression), valueType, currentFunction_->returnType,
      errorType));
}

void Binder::visit(FallbackExpr &node) {
  node.expression_->accept(*this);
  if (expressionStack_.empty()) {
    return;
  }

  auto expression = std::move(expressionStack_.top());
  expressionStack_.pop();

  if (!isFailableType(expression->type)) {
    error(node.span, "Operator 'or' requires failable expression type, got '" +
                         renderTypeForUser(expression->type) + "'");
    return;
  }

  auto valueType = failableValueType(expression->type);
  auto errorType = failableErrorType(expression->type);

  auto fallback = bindExpressionWithExpected(node.fallback_.get(), valueType);
  if (!fallback) {
    return;
  }

  if (!canConvert(fallback->type, valueType)) {
    error(node.fallback_->span, "Fallback expression type '" +
                                    renderTypeForUser(fallback->type) +
                                    "' is not compatible with '" +
                                    renderTypeForUser(valueType) + "'");
    return;
  }
  fallback = wrapInCast(std::move(fallback), valueType);

  expressionStack_.push(std::make_unique<BoundFallbackExpression>(
      std::move(expression), std::move(fallback), valueType, errorType));
}

void Binder::visit(FailableHandleExpr &node) {
  node.expression_->accept(*this);
  if (expressionStack_.empty()) {
    return;
  }

  auto expression = std::move(expressionStack_.top());
  expressionStack_.pop();

  if (!isFailableType(expression->type)) {
    error(node.span, "Operator 'or <name> { ... }' requires failable "
                     "expression type, got '" +
                         renderTypeForUser(expression->type) + "'");
    return;
  }

  auto valueType = failableValueType(expression->type);
  auto errorType = failableErrorType(expression->type);

  pushScope();
  auto errorSymbol = std::make_shared<VariableSymbol>(
      node.errorName_, errorType, false, false, node.errorName_,
      modules_[currentModuleId_].info->moduleName, Visibility::Private);
  if (!currentScope_->declare(node.errorName_, errorSymbol)) {
    error(node.span, "Handler variable '" + node.errorName_ +
                         "' is already declared in this scope.");
  }
  if (semanticInfo_) {
    semanticInfo_->recordSymbol(&node, errorSymbol);
    semanticInfo_->recordType(&node, errorType);
  }

  auto handler = bindBody(node.handler_.get(), false);
  popScope();

  if (handler && !handler->result) {
    handler->result = deriveValueExpressionFromBlock(*handler);
  }

  std::shared_ptr<zir::Type> handlerResultType = valueType;
  if (handler && handler->result) {
    if (!canConvert(handler->result->type, valueType)) {
      error(node.span, "Handler result type '" +
                           renderTypeForUser(handler->result->type) +
                           "' is not compatible with '" +
                           renderTypeForUser(valueType) + "'");
      return;
    }
    handler->result = wrapInCast(std::move(handler->result), valueType);
    handlerResultType = valueType;
  } else if (valueType && valueType->getKind() != zir::TypeKind::Void &&
             (!handler || !blockAlwaysReturns(handler.get()))) {
    error(node.span, "Handler block for 'or " + node.errorName_ +
                         " { ... }' must produce a fallback value of type '" +
                         renderTypeForUser(valueType) + "'.");
    return;
  }

  expressionStack_.push(std::make_unique<BoundFailableHandleExpression>(
      std::move(expression), errorSymbol, std::move(handler), handlerResultType,
      errorType));
}

void Binder::visit(ConstId &node) {
  auto symbol = currentScope_->lookup(node.value_);
  if (!symbol) {
    error(node.span, "Undefined identifier: " + node.value_);
    return;
  }

  if (auto varSymbol = std::dynamic_pointer_cast<VariableSymbol>(symbol)) {
    if (semanticInfo_) {
      semanticInfo_->recordSymbol(&node, varSymbol);
      semanticInfo_->recordType(&node, varSymbol->type);
    }
    expressionStack_.push(std::make_unique<BoundVariableExpression>(varSymbol));
  } else if (auto typeSymbol = std::dynamic_pointer_cast<TypeSymbol>(symbol)) {
    if (semanticInfo_) {
      semanticInfo_->recordSymbol(&node, typeSymbol);
      semanticInfo_->recordType(&node, typeSymbol->type);
    }
    expressionStack_.push(std::make_unique<BoundLiteral>("", typeSymbol->type));
  } else if (auto moduleSymbol =
                 std::dynamic_pointer_cast<ModuleSymbol>(symbol)) {
    expressionStack_.push(std::make_unique<BoundModuleReference>(moduleSymbol));
  } else if (auto overloadSet =
                 std::dynamic_pointer_cast<OverloadSetSymbol>(symbol)) {
    // Function reference — resolve to a function pointer type
    auto expected = currentExpectedExpressionType();
    std::shared_ptr<FunctionSymbol> match;

    if (expected && expected->getKind() == zir::TypeKind::FunctionPointer) {
      const auto &fpType =
          static_cast<const zir::FunctionPointerType &>(*expected);
      for (const auto &overload : overloadSet->overloads) {
        if (overload->parameters.size() == fpType.getParams().size()) {
          bool ok = true;
          for (size_t i = 0; i < fpType.getParams().size(); ++i) {
            if (!canConvert(fpType.getParams()[i],
                            overload->parameters[i]->type)) {
              ok = false;
              break;
            }
          }
          if (ok) {
            match = overload;
            break;
          }
        }
      }
    }
    if (!match && !overloadSet->overloads.empty())
      match = overloadSet->overloads.front();

    if (match) {
      // Build FunctionPointerType from the matched overload's signature
      std::vector<std::shared_ptr<zir::Type>> params;
      for (const auto &p : match->parameters)
        params.push_back(p->type);
      auto fpType = std::make_shared<zir::FunctionPointerType>(
          std::move(params), match->returnType);
      expressionStack_.push(
          std::make_unique<BoundFunctionReference>(match, fpType));
      return;
    }
    error(node.span, "'" + node.value_ + "' is not a variable or type.");
  } else {
    error(node.span, "'" + node.value_ + "' is not a variable or type.");
  }
}

void Binder::visit(AssignNode &node) {
  node.target_->accept(*this);
  if (expressionStack_.empty())
    return;
  auto target = std::move(expressionStack_.top());
  expressionStack_.pop();

  auto *targetAsVar = dynamic_cast<BoundVariableExpression *>(target.get());
  auto *targetAsIndex = dynamic_cast<BoundIndexAccess *>(target.get());
  auto *targetAsMember = dynamic_cast<BoundMemberAccess *>(target.get());
  auto *targetAsUnary = dynamic_cast<BoundUnaryExpression *>(target.get());
  auto *targetAsCall = dynamic_cast<BoundFunctionCall *>(target.get());

  bool isLValue = targetAsVar || targetAsIndex || targetAsMember ||
                  (targetAsUnary && targetAsUnary->op == "*") ||
                  (targetAsCall && targetAsCall->symbol->returnsRef);

  if (!isLValue) {
    error(node.span, "Target of assignment must be an l-value.");
    return;
  }

  if (auto varExpr = targetAsVar) {
    if (varExpr->symbol->is_const) {
      error(node.span,
            "Cannot assign to constant '" + varExpr->symbol->name + "'.");
      return;
    }
  } else if (auto indexExpr = targetAsIndex) {
    if (isStringType(indexExpr->left->type)) {
      error(node.span, "Cannot assign through String index access.");
      return;
    }
  }

  auto expr = bindExpressionWithExpected(node.expr_.get(), target->type);
  if (!expr)
    return;

  bool isCompound = !node.op_.empty();
  if (isCompound) {
    auto targetLoad = std::make_unique<BoundCompoundTargetLoad>(target->type);
    expr =
        buildBinaryExpression(std::move(targetLoad), node.op_, std::move(expr),
                              node.target_->span, node.expr_->span);
    if (!expr)
      return;
  }

  if (!canConvert(expr->type, target->type)) {
    error(node.span, "Cannot assign expression of type '" +
                         renderTypeForUser(expr->type) + "' to type '" +
                         renderTypeForUser(target->type) + "'");
  } else {
    expr = wrapInCast(std::move(expr), target->type);
  }

  statementStack_.push(std::make_unique<BoundAssignment>(
      std::move(target), std::move(expr), isCompound));
}

void Binder::visit(IndexAccessNode &node) {
  node.left_->accept(*this);
  if (expressionStack_.empty())
    return;
  auto left = std::move(expressionStack_.top());
  expressionStack_.pop();

  if (left->type->getKind() != zir::TypeKind::Array &&
      !isVariadicViewType(left->type) && !isStringType(left->type)) {
    error(node.span, "Type '" + renderTypeForUser(left->type) +
                         "' does not support indexing.");
    return;
  }

  node.index_->accept(*this);
  if (expressionStack_.empty())
    return;
  auto index = std::move(expressionStack_.top());
  expressionStack_.pop();

  if (!index->type->isInteger()) {
    error(node.span, "Array index must be an integer, but got '" +
                         renderTypeForUser(index->type) + "'");
  }

  std::shared_ptr<zir::Type> elementType;
  if (left->type->getKind() == zir::TypeKind::Array) {
    auto arrayType = std::static_pointer_cast<zir::ArrayType>(left->type);
    elementType = arrayType->getBaseType();
  } else if (isVariadicViewType(left->type)) {
    auto recordType = std::static_pointer_cast<zir::RecordType>(left->type);
    const auto &fields = recordType->getFields();
    if (fields.empty() || fields[0].type->getKind() != zir::TypeKind::Pointer) {
      error(node.span, "Internal error: invalid variadic view layout.");
      return;
    }
    elementType = std::static_pointer_cast<zir::PointerType>(fields[0].type)
                      ->getBaseType();
  } else {
    elementType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Char);
  }

  expressionStack_.push(std::make_unique<BoundIndexAccess>(
      std::move(left), std::move(index), elementType));
}

void Binder::visit(MemberAccessNode &node) {
  node.left_->accept(*this);
  if (expressionStack_.empty())
    return;

  auto left = std::move(expressionStack_.top());
  expressionStack_.pop();

  if (auto moduleRef = dynamic_cast<BoundModuleReference *>(left.get())) {
    auto memberIt = moduleRef->symbol->exports.find(node.member_);
    if (memberIt == moduleRef->symbol->exports.end()) {
      auto privateIt = moduleRef->symbol->members.find(node.member_);
      if (privateIt != moduleRef->symbol->members.end()) {
        error(node.span, "Member '" + node.member_ + "' of module '" +
                             moduleRef->symbol->name + "' is private.");
      } else {
        error(node.span, "Module '" + moduleRef->symbol->name +
                             "' has no member '" + node.member_ + "'");
      }
      return;
    }

    if (auto varSymbol =
            std::dynamic_pointer_cast<VariableSymbol>(memberIt->second)) {
      expressionStack_.push(
          std::make_unique<BoundVariableExpression>(varSymbol));
      return;
    }
    if (auto typeSymbol =
            std::dynamic_pointer_cast<TypeSymbol>(memberIt->second)) {
      expressionStack_.push(
          std::make_unique<BoundLiteral>("", typeSymbol->type));
      return;
    }
    if (auto nestedModule =
            std::dynamic_pointer_cast<ModuleSymbol>(memberIt->second)) {
      expressionStack_.push(
          std::make_unique<BoundModuleReference>(nestedModule));
      return;
    }

    error(node.span, "'" + node.member_ + "' is not a value or type.");
    return;
  }

  if (left->type->getKind() == zir::TypeKind::Enum) {
    auto enumType = std::static_pointer_cast<zir::EnumType>(left->type);
    int64_t value = enumType->getVariantDiscriminant(node.member_);
    if (value != -1) {
      expressionStack_.push(
          std::make_unique<BoundLiteral>(std::to_string(value), enumType));
      return;
    }
  } else if (left->type->getKind() == zir::TypeKind::TaggedUnion) {
    auto taggedUnionType =
        std::static_pointer_cast<zir::TaggedUnionType>(left->type);
    if (node.member_ == "tag") {
      expressionStack_.push(std::make_unique<BoundMemberAccess>(
          std::move(left), node.member_,
          std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int32)));
      return;
    }
    if (taggedUnionType->findVariant(node.member_) &&
        dynamic_cast<BoundLiteral *>(left.get())) {
      expressionStack_.push(std::make_unique<BoundMemberAccess>(
          std::move(left), node.member_, taggedUnionType));
      return;
    }
  } else if (left->type->getKind() == zir::TypeKind::Record) {
    auto recordType = std::static_pointer_cast<zir::RecordType>(left->type);
    for (const auto &field : recordType->getFields()) {
      if (field.name == node.member_) {
        expressionStack_.push(std::make_unique<BoundMemberAccess>(
            std::move(left), node.member_, field.type));
        return;
      }
    }
  } else if (left->type->getKind() == zir::TypeKind::Pointer) {
    auto ptrType = std::static_pointer_cast<zir::PointerType>(left->type);
    auto baseType = ptrType->getBaseType();

    if (baseType->getKind() == zir::TypeKind::Record) {
      auto recordType = std::static_pointer_cast<zir::RecordType>(baseType);
      for (const auto &field : recordType->getFields()) {
        if (field.name == node.member_) {
          expressionStack_.push(std::make_unique<BoundMemberAccess>(
              std::move(left), node.member_, field.type));
          return;
        }
      }
    } else if (baseType->getKind() == zir::TypeKind::Class) {
      auto classType = std::static_pointer_cast<zir::ClassType>(baseType);
      if (classType->isWeak()) {
        error(node.span, "Weak references cannot be accessed directly.");
        return;
      }
      auto infoIt = classInfos_.find(classType->getName());
      if (infoIt != classInfos_.end()) {
        auto fieldIt = infoIt->second.fields.find(node.member_);
        if (fieldIt != infoIt->second.fields.end()) {
          auto fieldVis = fieldIt->second->visibility;
          bool allowed = fieldVis == Visibility::Public ||
                         (!currentClassStack_.empty() &&
                          currentClassStack_.back() == classType->getName()) ||
                         (fieldVis == Visibility::Protected &&
                          !currentClassStack_.empty());
          if (!allowed) {
            error(node.span, "Field '" + node.member_ + "' is not accessible.");
            return;
          }
          expressionStack_.push(std::make_unique<BoundMemberAccess>(
              std::move(left), node.member_, fieldIt->second->type));
          return;
        }
      }
    }
  } else if (left->type->getKind() == zir::TypeKind::Class) {
    auto classType = std::static_pointer_cast<zir::ClassType>(left->type);
    if (classType->isWeak()) {
      error(node.span, "Weak references cannot be accessed directly.");
      return;
    }
    auto infoIt = classInfos_.find(classType->getName());
    if (infoIt != classInfos_.end()) {
      auto fieldIt = infoIt->second.fields.find(node.member_);
      if (fieldIt != infoIt->second.fields.end()) {
        auto fieldVis = fieldIt->second->visibility;
        bool allowed =
            fieldVis == Visibility::Public ||
            (!currentClassStack_.empty() &&
             currentClassStack_.back() == classType->getName()) ||
            (fieldVis == Visibility::Protected && !currentClassStack_.empty());
        if (!allowed) {
          error(node.span, "Field '" + node.member_ + "' is not accessible.");
          return;
        }
        expressionStack_.push(std::make_unique<BoundMemberAccess>(
            std::move(left), node.member_, fieldIt->second->type));
        return;
      }
    }
  }

  error(node.span, "Member '" + node.member_ + "' not found in type '" +
                       renderTypeForUser(left->type) + "'");
}

void Binder::visit(FunCall &node) {
  if (bindSizeOfBuiltinCall(node)) {
    return;
  }

  if (bindWeakBuiltinCall(node)) {
    return;
  }

  if (auto member = dynamic_cast<MemberAccessNode *>(node.callee_.get())) {
    member->left_->accept(*this);
    if (expressionStack_.empty()) {
      return;
    }
    auto selfExpr = std::move(expressionStack_.top());
    expressionStack_.pop();
    if (selfExpr->type->getKind() == zir::TypeKind::TaggedUnion &&
        dynamic_cast<BoundLiteral *>(selfExpr.get())) {
      auto taggedUnionType =
          std::static_pointer_cast<zir::TaggedUnionType>(selfExpr->type);
      auto variant = taggedUnionType->findVariant(member->member_);
      if (!variant) {
        error(node.span, "Enum '" + taggedUnionType->getName() +
                             "' has no variant '" + member->member_ + "'.");
        return;
      }

      if (variant->payloadType) {
        if (node.params_.size() != 1) {
          error(node.span, "Enum variant '" + member->member_ +
                               "' expects one payload argument.");
          return;
        }
        if (!node.params_[0]->name.empty() || node.params_[0]->isRef ||
            node.params_[0]->isSpread) {
          error(node.params_[0]->value->span,
                "Enum payload arguments must be positional values.");
          return;
        }
        auto payload = bindExpressionWithExpected(node.params_[0]->value.get(),
                                                  variant->payloadType);
        if (!payload) {
          return;
        }
        if (!canConvert(payload->type, variant->payloadType)) {
          error(node.params_[0]->value->span,
                "Cannot convert enum payload from '" +
                    renderTypeForUser(payload->type) + "' to '" +
                    renderTypeForUser(variant->payloadType) + "'");
          return;
        }
        payload = wrapInCast(std::move(payload), variant->payloadType);
        expressionStack_.push(std::make_unique<BoundTaggedUnionLiteral>(
            taggedUnionType, variant->name, variant->tag, std::move(payload)));
        return;
      }

      if (!node.params_.empty()) {
        error(node.span, "Enum variant '" + member->member_ +
                             "' does not take a payload argument.");
        return;
      }
      expressionStack_.push(std::make_unique<BoundTaggedUnionLiteral>(
          taggedUnionType, variant->name, variant->tag, nullptr));
      return;
    }

    if (selfExpr->type->getKind() != zir::TypeKind::Class) {
      // Not a class method call. Fall through to the normal qualified
      // function/module call resolution path below.
    } else {
      auto classType = std::static_pointer_cast<zir::ClassType>(selfExpr->type);
      if (classType->isWeak()) {
        error(node.span,
              "Weak references cannot be used to call methods directly.");
        return;
      }
      auto infoIt = classInfos_.find(classType->getName());
      if (infoIt == classInfos_.end()) {
        error(node.span, "Unknown class type: " + classType->getName());
        return;
      }
      auto methodIt = infoIt->second.methods.find(member->member_);
      if (methodIt == infoIt->second.methods.end()) {
        error(node.span, "Class '" + classType->getName() +
                             "' has no method '" + member->member_ + "'.");
        return;
      }
      auto funcSymbol =
          std::dynamic_pointer_cast<FunctionSymbol>(methodIt->second);
      if (!funcSymbol) {
        error(node.span, "'" + member->member_ + "' is not a method.");
        return;
      }
      bool methodAllowed =
          funcSymbol->visibility == Visibility::Public ||
          (!currentClassStack_.empty() &&
           currentClassStack_.back() == classType->getName()) ||
          (funcSymbol->visibility == Visibility::Protected &&
           !currentClassStack_.empty());
      if (!methodAllowed) {
        error(node.span, "Method '" + member->member_ + "' is not accessible.");
        return;
      }
      if (funcSymbol->isUnsafe) {
        requireUnsafeContext(node.span, "unsafe function calls");
      }

      std::vector<std::unique_ptr<BoundExpression>> rawArgs;
      rawArgs.reserve(node.params_.size());
      for (size_t i = 0; i < node.params_.size(); ++i) {
        auto arg =
            bindExpressionWithExpected(node.params_[i]->value.get(), nullptr);
        if (!arg) {
          return;
        }
        rawArgs.push_back(std::move(arg));
      }

      if (!funcSymbol->genericParameterNames.empty()) {
        std::vector<std::unique_ptr<BoundExpression>> inferenceArgs;
        if (funcSymbol->isMethod) {
          inferenceArgs.push_back(selfExpr->clone());
        }
        for (const auto &rawArg : rawArgs) {
          inferenceArgs.push_back(rawArg->clone());
        }
        std::string genericBindingFailure;
        auto genericBindings =
            buildGenericBindings(*funcSymbol, inferenceArgs, node.genericArgs_,
                                 node.span, &genericBindingFailure);
        if (genericBindings.empty()) {
          error(node.span,
                "No matching overload for method '" + member->member_ + "'. " +
                    (genericBindingFailure.empty()
                         ? std::string("Generic type binding failed.")
                         : genericBindingFailure));
          return;
        }
        funcSymbol = ensureGenericFunctionInstantiation(
            funcSymbol, orderedGenericBindings(genericBindings), node.span);
        if (!funcSymbol) {
          error(node.span, "Failed to instantiate generic method '" +
                               member->member_ + "'.");
          return;
        }
      } else if (!node.genericArgs_.empty()) {
        error(node.span, "Method '" + member->member_ +
                             "' does not accept generic arguments.");
        return;
      }

      std::vector<std::unique_ptr<BoundExpression>> args;
      std::vector<bool> argIsRef;
      if (funcSymbol->isMethod) {
        args.push_back(std::move(selfExpr));
        argIsRef.push_back(false);
      }

      size_t paramOffset = funcSymbol->isMethod ? 1 : 0;
      if (node.params_.size() + paramOffset != funcSymbol->parameters.size()) {
        error(node.span,
              "No matching overload for method '" + member->member_ + "'.");
        return;
      }

      for (size_t i = 0; i < node.params_.size(); ++i) {
        auto arg = rawArgs[i]->clone();
        auto expectedType = funcSymbol->parameters[i + paramOffset]->type;
        if (!canConvert(arg->type, expectedType)) {
          error(node.params_[i]->value->span,
                "Cannot convert method argument from '" +
                    renderTypeForUser(arg->type) + "' to '" +
                    renderTypeForUser(expectedType) + "'");
          return;
        }
        arg = wrapInCast(std::move(arg), expectedType);
        args.push_back(std::move(arg));
        argIsRef.push_back(node.params_[i]->isRef);
      }

      expressionStack_.push(std::make_unique<BoundFunctionCall>(
          funcSymbol, std::move(args), std::move(argIsRef)));
      return;
    }
  }

  std::vector<std::string> calleeParts;
  if (!node.callee_ || !extractQualifiedPath(node.callee_.get(), calleeParts)) {
    // Try indirect call through function pointer
    if (node.callee_) {
      node.callee_->accept(*this);
      if (!expressionStack_.empty()) {
        auto calleeExpr = std::move(expressionStack_.top());
        expressionStack_.pop();
        if (calleeExpr->type &&
            calleeExpr->type->getKind() == zir::TypeKind::FunctionPointer) {
          const auto &fpType =
              static_cast<const zir::FunctionPointerType &>(*calleeExpr->type);
          if (node.params_.size() != fpType.getParams().size()) {
            error(node.span, "Function pointer call argument count mismatch.");
            return;
          }
          std::vector<std::unique_ptr<BoundExpression>> args;
          for (size_t i = 0; i < node.params_.size(); ++i) {
            auto arg = bindExpressionWithExpected(node.params_[i]->value.get(),
                                                  fpType.getParams()[i]);
            if (!arg)
              return;
            args.push_back(std::move(arg));
          }
          expressionStack_.push(std::make_unique<BoundIndirectCall>(
              std::move(calleeExpr), std::move(args), fpType.getReturnType()));
          return;
        }
      }
    }
    error(node.span, "Only direct function calls are supported.");
    return;
  }

  auto symbol =
      resolveQualifiedSymbol(calleeParts, node.span, SymbolKind::Function);
  if (!symbol) {
    // Check if it's a variable holding a function pointer
    auto varSym =
        resolveQualifiedSymbol(calleeParts, node.span, SymbolKind::Variable);
    if (varSym && varSym->getKind() == SymbolKind::Variable) {
      auto varSymbol = std::static_pointer_cast<VariableSymbol>(varSym);
      if (varSymbol->type &&
          varSymbol->type->getKind() == zir::TypeKind::FunctionPointer) {
        const auto &fpType =
            static_cast<const zir::FunctionPointerType &>(*varSymbol->type);
        if (node.params_.size() != fpType.getParams().size()) {
          error(node.span, "Function pointer call argument count mismatch.");
          return;
        }
        std::vector<std::unique_ptr<BoundExpression>> args;
        for (size_t i = 0; i < node.params_.size(); ++i) {
          auto arg = bindExpressionWithExpected(node.params_[i]->value.get(),
                                                fpType.getParams()[i]);
          if (!arg)
            return;
          args.push_back(std::move(arg));
        }
        auto calleeExpr = std::make_unique<BoundVariableExpression>(varSymbol);
        expressionStack_.push(std::make_unique<BoundIndirectCall>(
            std::move(calleeExpr), std::move(args), fpType.getReturnType()));
        return;
      }
    }
    return;
  }

  auto candidates = collectOverloads(symbol);
  if (candidates.empty()) {
    error(node.span, "'" + calleeParts.back() + "' is not a function.");
    return;
  }

  bool seenSpreadArg = false;
  std::vector<std::unique_ptr<BoundExpression>> rawArgs;
  std::vector<bool> rawArgIsRef;
  std::vector<bool> rawArgIsSpread;
  std::vector<std::string> rawArgNames;
  for (size_t i = 0; i < node.params_.size(); ++i) {
    if (seenSpreadArg) {
      error(node.params_[i]->value->span,
            "Spread argument must be the last argument in a function call.");
      return;
    }

    auto arg =
        bindExpressionWithExpected(node.params_[i]->value.get(), nullptr);
    if (!arg)
      return;
    rawArgNames.push_back(node.params_[i]->name);
    rawArgIsRef.push_back(node.params_[i]->isRef);
    rawArgIsSpread.push_back(node.params_[i]->isSpread);
    if (node.params_[i]->isSpread) {
      seenSpreadArg = true;
    }
    rawArgs.push_back(std::move(arg));
  }

  struct CandidateMatch {
    std::shared_ptr<FunctionSymbol> symbol;
    std::vector<std::unique_ptr<BoundExpression>> arguments;
    std::vector<bool> argumentIsRef;
    std::unique_ptr<BoundExpression> variadicPack;
    std::vector<int> cost;
    bool usedExtraArguments = false;
    int returnCost = 0;
    std::vector<std::string> notes;
  };

  auto compareCost = [](const CandidateMatch &lhs, const CandidateMatch &rhs) {
    if (lhs.cost != rhs.cost) {
      return lhs.cost < rhs.cost;
    }
    if (lhs.returnCost != rhs.returnCost) {
      return lhs.returnCost < rhs.returnCost;
    }
    if (lhs.usedExtraArguments != rhs.usedExtraArguments) {
      return !lhs.usedExtraArguments && rhs.usedExtraArguments;
    }
    if (lhs.symbol->acceptsExtraArguments() !=
        rhs.symbol->acceptsExtraArguments()) {
      return !lhs.symbol->acceptsExtraArguments() &&
             rhs.symbol->acceptsExtraArguments();
    }
    return false;
  };

  std::vector<CandidateMatch> matches;
  std::shared_ptr<FunctionSymbol> blockedUnsafeMatch = nullptr;
  std::vector<std::string> rejectionNotes;
  auto expectedReturnType = currentExpectedExpressionType();

  for (const auto &funcSymbol : candidates) {
    if (!funcSymbol) {
      continue;
    }

    size_t fixedParamCount = funcSymbol->fixedParameterCount();
    bool hasExplicitTypeArgs = !node.genericArgs_.empty();
    if (hasExplicitTypeArgs &&
        node.genericArgs_.size() > funcSymbol->genericParameterNames.size()) {
      rejectionNotes.push_back("'" + renderFunctionSignature(*funcSymbol) +
                               "': explicit generic argument count mismatch");
      continue;
    }

    if (!hasExplicitTypeArgs && !funcSymbol->genericParameterNames.empty()) {
      // inference is allowed; no early rejection
    }
    if (!funcSymbol->acceptsExtraArguments() &&
        node.params_.size() != funcSymbol->parameters.size()) {
      rejectionNotes.push_back("'" + renderFunctionSignature(*funcSymbol) +
                               "': wrong argument count");
      continue;
    }
    if (funcSymbol->acceptsExtraArguments() &&
        node.params_.size() < fixedParamCount) {
      rejectionNotes.push_back("'" + renderFunctionSignature(*funcSymbol) +
                               "': too few arguments");
      continue;
    }

    CandidateMatch match;
    match.symbol = funcSymbol;
    match.arguments.resize(fixedParamCount);
    match.argumentIsRef.resize(fixedParamCount, false);
    std::string genericBindingFailure;
    auto genericBindings =
        buildGenericBindings(*funcSymbol, rawArgs, node.genericArgs_, node.span,
                             &genericBindingFailure);
    if (!funcSymbol->genericParameterNames.empty() && genericBindings.empty()) {
      rejectionNotes.push_back(
          "'" + renderFunctionSignature(*funcSymbol) + "': " +
          (genericBindingFailure.empty() ? "generic type binding failed"
                                         : genericBindingFailure));
      continue;
    }
    bool failed = false;
    std::string failureReason;
    auto variadicParam = funcSymbol->variadicParameter();
    std::vector<int> positionalToParameter(rawArgs.size(), -1);
    std::vector<bool> parameterAssigned(funcSymbol->parameters.size(), false);
    bool seenNamedArgument = false;

    for (size_t i = 0, positionalIndex = 0; i < rawArgs.size(); ++i) {
      const bool isSpread = rawArgIsSpread[i];
      const bool isNamed = !rawArgNames[i].empty();

      if (isSpread) {
        if (isNamed) {
          failed = true;
          failureReason = "named spread arguments are not supported";
          break;
        }
        positionalToParameter[i] = static_cast<int>(fixedParamCount);
        continue;
      }

      if (isNamed) {
        seenNamedArgument = true;
        bool found = false;
        for (size_t paramIndex = 0; paramIndex < fixedParamCount;
             ++paramIndex) {
          if (funcSymbol->parameters[paramIndex]->name != rawArgNames[i]) {
            continue;
          }
          if (parameterAssigned[paramIndex]) {
            failed = true;
            failureReason =
                "parameter '" + rawArgNames[i] + "' provided more than once";
            break;
          }
          positionalToParameter[i] = static_cast<int>(paramIndex);
          parameterAssigned[paramIndex] = true;
          found = true;
          break;
        }
        if (failed) {
          break;
        }
        if (!found) {
          failed = true;
          failureReason = "unknown named argument '" + rawArgNames[i] + "'";
          break;
        }
        continue;
      }

      if (seenNamedArgument) {
        failed = true;
        failureReason = "positional arguments cannot follow named arguments";
        break;
      }

      while (positionalIndex < fixedParamCount &&
             parameterAssigned[positionalIndex]) {
        ++positionalIndex;
      }

      if (positionalIndex < fixedParamCount) {
        positionalToParameter[i] = static_cast<int>(positionalIndex);
        parameterAssigned[positionalIndex] = true;
        ++positionalIndex;
      } else {
        positionalToParameter[i] = static_cast<int>(fixedParamCount);
      }
    }

    if (!failed) {
      for (size_t paramIndex = 0; paramIndex < fixedParamCount; ++paramIndex) {
        if (!parameterAssigned[paramIndex]) {
          failed = true;
          failureReason = "missing argument for parameter '" +
                          funcSymbol->parameters[paramIndex]->name + "'";
          break;
        }
      }
    }

    for (size_t i = 0; i < rawArgs.size(); ++i) {
      auto arg = rawArgs[i]->clone();
      bool argIsRef = rawArgIsRef[i];
      bool argIsSpread = rawArgIsSpread[i];
      int parameterIndex = positionalToParameter[i];

      if (failed) {
        break;
      }

      if (argIsSpread) {
        if (parameterIndex < static_cast<int>(fixedParamCount) || argIsRef ||
            !funcSymbol->hasVariadicParameter()) {
          failed = true;
          failureReason =
              "spread arguments can only target a Zap variadic parameter";
          break;
        }

        if (!variadicParam || !variadicParam->variadic_element_type) {
          failed = true;
          failureReason = "internal error: missing variadic parameter type";
          break;
        }

        auto expectedViewType =
            makeVariadicViewType(variadicParam->variadic_element_type);

        if (arg->type && arg->type->getKind() == zir::TypeKind::Array) {
          if (!canConvert(arg->type, expectedViewType)) {
            failed = true;
            failureReason =
                "spread argument type does not match variadic parameter";
            break;
          }
          arg = wrapInCast(std::move(arg), expectedViewType);
        } else if (!arg->type || !isVariadicViewType(arg->type) ||
                   !canConvert(arg->type, expectedViewType)) {
          failed = true;
          failureReason =
              "spread argument type does not match variadic parameter";
          break;
        }

        match.variadicPack = std::move(arg);
        match.usedExtraArguments = true;
        match.notes.push_back("spread -> variadic pack");
        continue;
      }

      if (parameterIndex >= 0 &&
          parameterIndex < static_cast<int>(fixedParamCount)) {
        auto expectedType = funcSymbol->parameters[parameterIndex]->type;
        if (!genericBindings.empty()) {
          expectedType = substituteGenericType(expectedType, genericBindings);
        }
        const auto &parameter = funcSymbol->parameters[parameterIndex];
        if (argIsRef != parameter->is_ref) {
          failed = true;
          failureReason = "argument for parameter '" + parameter->name +
                          "' has mismatched ref-ness";
          break;
        }

        if (argIsRef) {
          auto varExpr = dynamic_cast<BoundVariableExpression *>(arg.get());
          if (!varExpr || arg->type->toString() != expectedType->toString()) {
            failed = true;
            failureReason = "ref argument for parameter '" + parameter->name +
                            "' must exactly match type '" +
                            renderTypeForUser(expectedType) + "'";
            break;
          }
          match.cost.push_back(0);
          match.notes.push_back("param " + parameter->name +
                                ": exact ref match");
        } else if (!canConvert(arg->type, expectedType)) {
          failed = true;
          failureReason = "argument for parameter '" + parameter->name +
                          "' is not convertible from '" +
                          renderTypeForUser(arg->type) + "' to '" +
                          renderTypeForUser(expectedType) + "'";
          break;
        } else {
          int cost = conversionCost(arg->type, expectedType);
          if (cost >= 1000) {
            failed = true;
            failureReason = "argument for parameter '" + parameter->name +
                            "' is not convertible from '" +
                            renderTypeForUser(arg->type) + "' to '" +
                            renderTypeForUser(expectedType) + "'";
            break;
          }
          match.cost.push_back(cost);
          match.notes.push_back("param " + parameter->name + ": " +
                                describeConversion(arg->type, expectedType));
          arg = wrapInCast(std::move(arg), expectedType);
        }
        match.argumentIsRef[parameterIndex] = argIsRef;
        match.arguments[parameterIndex] = std::move(arg);
      } else if (funcSymbol->hasVariadicParameter()) {
        if (argIsRef) {
          failed = true;
          failureReason = "variadic arguments cannot be passed by ref";
          break;
        }
        auto expectedType = variadicParam->variadic_element_type;
        if (!genericBindings.empty()) {
          expectedType = substituteGenericType(expectedType, genericBindings);
        }
        if (!canConvert(arg->type, expectedType)) {
          failed = true;
          failureReason = "variadic argument is not convertible from '" +
                          renderTypeForUser(arg->type) + "' to '" +
                          renderTypeForUser(expectedType) + "'";
          break;
        }
        int cost = conversionCost(arg->type, expectedType);
        match.cost.push_back(cost);
        match.usedExtraArguments = true;
        match.notes.push_back("variadic: " +
                              describeConversion(arg->type, expectedType));
        arg = wrapInCast(std::move(arg), expectedType);
        match.argumentIsRef.push_back(false);
        match.arguments.push_back(std::move(arg));
      } else if (funcSymbol->isCVariadic) {
        if (argIsRef) {
          failed = true;
          failureReason = "C variadic arguments cannot be passed by ref";
          break;
        }
        auto promotedType = getCVariadicArgumentType(arg->type);
        if (!promotedType) {
          failed = true;
          failureReason = "type '" + renderTypeForUser(arg->type) +
                          "' is not supported in C variadic arguments";
          break;
        }
        int cost = conversionCost(arg->type, promotedType);
        match.cost.push_back(cost);
        match.usedExtraArguments = true;
        match.notes.push_back("c variadic: " +
                              describeConversion(arg->type, promotedType));
        arg = wrapInCast(std::move(arg), promotedType);
        match.argumentIsRef.push_back(false);
        match.arguments.push_back(std::move(arg));
      } else {
        failed = true;
        failureReason = "too many arguments";
        break;
      }
    }

    if (failed) {
      rejectionNotes.push_back("'" + renderFunctionSignature(*funcSymbol) +
                               "': " + failureReason);
      continue;
    }

    if (expectedReturnType) {
      if (funcSymbol->returnType->toString() ==
          expectedReturnType->toString()) {
        match.returnCost = 0;
        match.notes.push_back("return: exact match");
      } else if (canConvert(funcSymbol->returnType, expectedReturnType)) {
        match.returnCost =
            conversionCost(funcSymbol->returnType, expectedReturnType);
        match.notes.push_back(
            "return: " +
            describeConversion(funcSymbol->returnType, expectedReturnType));
      } else {
        match.returnCost = 50;
        match.notes.push_back("return: incompatible with expected " +
                              renderTypeForUser(expectedReturnType));
      }
    }

    std::shared_ptr<FunctionSymbol> resolvedSymbol = funcSymbol;
    if (!funcSymbol->genericParameterNames.empty()) {
      resolvedSymbol = ensureGenericFunctionInstantiation(
          funcSymbol, orderedGenericBindings(genericBindings), node.span);
      if (!resolvedSymbol) {
        rejectionNotes.push_back("'" + renderFunctionSignature(*funcSymbol) +
                                 "': failed to instantiate generic function");
        continue;
      }

      std::vector<std::unique_ptr<BoundExpression>> remappedArgs;
      std::vector<bool> remappedRef;
      remappedArgs.reserve(match.arguments.size());
      remappedRef.reserve(match.argumentIsRef.size());

      for (size_t i = 0; i < match.arguments.size(); ++i) {
        auto argClone =
            match.arguments[i] ? match.arguments[i]->clone() : nullptr;
        if (i < resolvedSymbol->parameters.size() && argClone) {
          auto expected = resolvedSymbol->parameters[i]->type;
          if (!resolvedSymbol->parameters[i]->is_ref) {
            argClone = wrapInCast(std::move(argClone), expected);
          }
        }
        remappedArgs.push_back(std::move(argClone));
        remappedRef.push_back(
            i < match.argumentIsRef.size() ? match.argumentIsRef[i] : false);
      }

      match.arguments = std::move(remappedArgs);
      match.argumentIsRef = std::move(remappedRef);
      match.symbol = resolvedSymbol;
    }

    if (resolvedSymbol->isUnsafe && !isUnsafeActive()) {
      blockedUnsafeMatch = resolvedSymbol;
      rejectionNotes.push_back("'" + renderFunctionSignature(*resolvedSymbol) +
                               "': requires unsafe context");
      continue;
    }
    matches.push_back(std::move(match));
  }

  if (matches.empty()) {
    if (blockedUnsafeMatch) {
      requireUnsafeContext(node.span, "unsafe function calls");
      return;
    }

    error(
        node.callee_->span,
        "No matching overload for function '" + calleeParts.back() + "'. " +
            (rejectionNotes.empty() ? std::string() : ("Candidates: " + [&]() {
              std::string details;
              for (size_t i = 0; i < rejectionNotes.size(); ++i) {
                if (i != 0) {
                  details += "; ";
                }
                details += rejectionNotes[i];
              }
              return details;
            }())));
    return;
  }

  size_t bestIndex = 0;
  for (size_t i = 1; i < matches.size(); ++i) {
    if (compareCost(matches[i], matches[bestIndex])) {
      bestIndex = i;
    }
  }

  std::vector<size_t> ambiguous = {bestIndex};
  for (size_t i = 0; i < matches.size(); ++i) {
    if (i == bestIndex) {
      continue;
    }
    if (!compareCost(matches[i], matches[bestIndex]) &&
        !compareCost(matches[bestIndex], matches[i])) {
      ambiguous.push_back(i);
    }
  }

  if (ambiguous.size() > 1) {
    std::string message =
        "Call to function '" + calleeParts.back() + "' is ambiguous between ";
    for (size_t i = 0; i < ambiguous.size(); ++i) {
      if (i != 0) {
        message += i + 1 == ambiguous.size() ? " and " : ", ";
      }
      message +=
          "'" + renderFunctionSignature(*matches[ambiguous[i]].symbol) + "'";
    }
    message += ".";
    if (expectedReturnType) {
      message +=
          " Expected result type: '" + expectedReturnType->toString() + "'.";
    }
    message += " Candidate details: ";
    for (size_t i = 0; i < ambiguous.size(); ++i) {
      if (i != 0) {
        message += "; ";
      }
      message +=
          "'" + renderFunctionSignature(*matches[ambiguous[i]].symbol) + "' [";
      for (size_t j = 0; j < matches[ambiguous[i]].notes.size(); ++j) {
        if (j != 0) {
          message += ", ";
        }
        message += matches[ambiguous[i]].notes[j];
      }
      message += "]";
    }
    error(node.callee_->span, message);
    return;
  }

  auto &best = matches[bestIndex];
  expressionStack_.push(std::make_unique<BoundFunctionCall>(
      best.symbol, std::move(best.arguments), std::move(best.argumentIsRef),
      std::move(best.variadicPack)));
}

bool Binder::bindSizeOfBuiltinCall(FunCall &node) {
  auto *calleeId = dynamic_cast<ConstId *>(node.callee_.get());
  if (!calleeId || calleeId->value_ != "sizeof") {
    return false;
  }

  if (node.params_.size() != 1 || node.params_[0]->isRef ||
      node.params_[0]->isSpread || !node.params_[0]->name.empty()) {
    error(node.span, "'sizeof' expects exactly one positional argument.");
    return true;
  }

  auto argument =
      bindExpressionWithExpected(node.params_[0]->value.get(), nullptr);
  if (!argument) {
    return true;
  }

  auto layout = layoutOfType(argument->type);
  expressionStack_.push(std::make_unique<BoundLiteral>(
      std::to_string(layout.size),
      std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int)));
  return true;
}

bool Binder::bindWeakBuiltinCall(FunCall &node) {
  auto *calleeId = dynamic_cast<ConstId *>(node.callee_.get());
  if (!calleeId) {
    return false;
  }

  const bool isLock = calleeId->value_ == "lock";
  const bool isAlive = calleeId->value_ == "alive";
  if (!isLock && !isAlive) {
    return false;
  }

  if (node.params_.size() != 1 || node.params_[0]->isRef ||
      node.params_[0]->isSpread || !node.params_[0]->name.empty()) {
    error(node.span, "'" + calleeId->value_ +
                         "' expects exactly one positional argument.");
    return true;
  }

  auto weakExpr =
      bindExpressionWithExpected(node.params_[0]->value.get(), nullptr);
  if (!weakExpr) {
    return true;
  }

  if (weakExpr->type->getKind() != zir::TypeKind::Class) {
    error(node.params_[0]->value->span,
          "'" + calleeId->value_ + "' expects a weak class reference.");
    return true;
  }

  auto weakClassType = std::static_pointer_cast<zir::ClassType>(weakExpr->type);
  if (!weakClassType->isWeak()) {
    error(node.params_[0]->value->span,
          "'" + calleeId->value_ + "' expects a weak class reference.");
    return true;
  }

  if (isAlive) {
    expressionStack_.push(
        std::make_unique<BoundWeakAliveExpression>(std::move(weakExpr)));
    return true;
  }

  auto strongType = std::make_shared<zir::ClassType>(*weakClassType);
  strongType->setWeak(false);
  expressionStack_.push(std::make_unique<BoundWeakLockExpression>(
      std::move(weakExpr), strongType));
  return true;
}

void Binder::visit(NewExpr &node) {
  auto classType = mapType(*node.type_);
  if (!classType || classType->getKind() != zir::TypeKind::Class) {
    error(node.span, "'new' expects a class type.");
    return;
  }
  auto concreteType = std::static_pointer_cast<zir::ClassType>(classType);
  if (concreteType->isWeak()) {
    error(node.span, "'new' expects a strong class type, not 'weak'.");
    return;
  }
  auto infoIt = classInfos_.find(concreteType->getName());
  if (infoIt == classInfos_.end()) {
    error(node.span, "Unknown class type: " + concreteType->getName());
    return;
  }

  std::vector<std::unique_ptr<BoundExpression>> args;
  std::vector<bool> argRefs;
  auto ctor = infoIt->second.constructor;
  size_t ctorParamOffset = ctor && ctor->isMethod ? 1 : 0;
  size_t expectedArgCount =
      ctor ? (ctor->parameters.size() - ctorParamOffset) : 0;
  if (node.args_.size() != expectedArgCount) {
    error(node.span, "Constructor for class '" + concreteType->getName() +
                         "' expects " + std::to_string(expectedArgCount) +
                         " arguments, got " +
                         std::to_string(node.args_.size()) + ".");
    return;
  }

  for (size_t i = 0; i < node.args_.size(); ++i) {
    auto expected =
        ctor ? ctor->parameters[i + ctorParamOffset]->type : nullptr;
    auto arg = bindExpressionWithExpected(node.args_[i]->value.get(), expected);
    if (!arg) {
      return;
    }
    if (expected && !canConvert(arg->type, expected)) {
      error(node.args_[i]->value->span,
            "Cannot convert constructor argument from '" +
                renderTypeForUser(arg->type) + "' to '" +
                renderTypeForUser(expected) + "'");
      return;
    }
    if (expected) {
      arg = wrapInCast(std::move(arg), expected);
    }
    args.push_back(std::move(arg));
    argRefs.push_back(node.args_[i]->isRef);
  }

  expressionStack_.push(std::make_unique<BoundNewExpression>(
      concreteType, ctor, std::move(args), std::move(argRefs)));
  if (semanticInfo_) {
    semanticInfo_->recordType(&node, concreteType);
  }
}

void Binder::visit(ConstBool &node) {
  expressionStack_.push(std::make_unique<BoundLiteral>(
      node.value_ ? "true" : "false",
      std::make_shared<zir::PrimitiveType>(zir::TypeKind::Bool)));
}

void Binder::visit(UnaryExpr &node) {
  node.expr_->accept(*this);
  if (expressionStack_.empty())
    return;
  auto expr = std::move(expressionStack_.top());
  expressionStack_.pop();

  auto type = expr->type;
  if (node.op_ == "&") {
    auto *exprAsVar = dynamic_cast<BoundVariableExpression *>(expr.get());
    auto *exprAsIndex = dynamic_cast<BoundIndexAccess *>(expr.get());
    auto *exprAsMember = dynamic_cast<BoundMemberAccess *>(expr.get());
    auto *exprAsUnary = dynamic_cast<BoundUnaryExpression *>(expr.get());
    auto *exprAsCall = dynamic_cast<BoundFunctionCall *>(expr.get());
    bool isLValue = exprAsVar || exprAsIndex || exprAsMember ||
                    (exprAsUnary && exprAsUnary->op == "*") ||
                    (exprAsCall && exprAsCall->symbol->returnsRef);
    if (!isLValue) {
      error(node.span, "Cannot take the address of a non-lvalue expression.");
    }

    type = std::make_shared<zir::PointerType>(expr->type);
  } else if (node.op_ == "*") {
    requireUnsafeContext(node.span, "pointer dereference");
    if (!isPointerType(type)) {
      error(node.span, "Cannot dereference non-pointer type '" +
                           renderTypeForUser(type) + "'");
    } else {
      type = std::static_pointer_cast<zir::PointerType>(type)->getBaseType();
      if (type->getKind() == zir::TypeKind::Void) {
        error(node.span, "Cannot dereference '*Void' directly. Cast it to a "
                         "concrete pointer type first.");
      }
    }
  } else if (node.op_ == "-" || node.op_ == "+") {
    if (!isNumeric(type)) {
      error(node.span, "Operator '" + node.op_ +
                           "' cannot be applied to type '" +
                           renderTypeForUser(type) + "'");
    }
  } else if (node.op_ == "!") {
    if (type->getKind() != zir::TypeKind::Bool) {
      error(node.span, "Operator '!' cannot be applied to type '" +
                           renderTypeForUser(type) + "'");
    }
  } else if (node.op_ == "~") {
    if (!type->isInteger()) {
      error(node.span, "Operator '~' cannot be applied to type '" +
                           renderTypeForUser(type) + "'");
    }
  }

  expressionStack_.push(
      std::make_unique<BoundUnaryExpression>(node.op_, std::move(expr), type));
}

void Binder::visit(ArrayLiteralNode &node) {
  std::vector<std::unique_ptr<BoundExpression>> elements;
  std::shared_ptr<zir::Type> elementType = nullptr;

  auto expectedType = currentExpectedExpressionType();
  if (expectedType && expectedType->getKind() == zir::TypeKind::Array) {
    elementType =
        std::static_pointer_cast<zir::ArrayType>(expectedType)->getBaseType();
  }

  for (const auto &el : node.elements_) {
    auto boundEl = bindExpressionWithExpected(el.get(), elementType);
    if (boundEl) {
      if (!elementType) {
        elementType = boundEl->type;
      } else if (!canConvert(boundEl->type, elementType)) {
        error(el->span, "Array elements must have the same type. Expected '" +
                            renderTypeForUser(elementType) + "', but got '" +
                            renderTypeForUser(boundEl->type) + "'");
      }
      elements.push_back(std::move(boundEl));
    }
  }

  auto arrayType = std::make_shared<zir::ArrayType>(
      elementType ? elementType
                  : std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void),
      elements.size());
  expressionStack_.push(
      std::make_unique<BoundArrayLiteral>(std::move(elements), arrayType));
}

void Binder::visit(StructLiteralNode &node) {
  if (!node.type_) {
    error(node.span, "Missing struct literal type.");
    return;
  }

  auto parts = splitQualified(node.type_->qualifiedName());
  auto symbol = resolveQualifiedSymbol(parts, node.span, SymbolKind::Type);
  if (!symbol || symbol->getKind() != SymbolKind::Type) {
    error(node.span, "Unknown type: " + node.type_->qualifiedName());
    return;
  }

  auto typeSymbol = std::static_pointer_cast<TypeSymbol>(symbol);

  bool pushedInferredBindings = false;
  if (!typeSymbol->genericParameterNames.empty() &&
      node.type_->genericArgs.empty()) {
    const std::vector<std::unique_ptr<ParameterNode>> *declFields = nullptr;
    if (auto rdIt = recordTypeDeclarationNodes_.find(typeSymbol.get());
        rdIt != recordTypeDeclarationNodes_.end()) {
      declFields = &rdIt->second->fields_;
    } else if (auto sdIt = structTypeDeclarationNodes_.find(typeSymbol.get());
               sdIt != structTypeDeclarationNodes_.end()) {
      declFields = &sdIt->second->fields_;
    }

    if (declFields) {
      std::unordered_map<std::string, std::shared_ptr<zir::Type>>
          inferredBindings;

      for (const auto &fieldInit : node.fields_) {
        for (const auto &declField : *declFields) {
          if (declField->name != fieldInit.name)
            continue;
          const TypeNode &fieldTypeNode = *declField->type;
          if (!fieldTypeNode.qualifiers.empty() ||
              !fieldTypeNode.genericArgs.empty())
            break;
          const auto &paramName = fieldTypeNode.typeName;
          bool isGenericParam = false;
          for (const auto &gp : typeSymbol->genericParameterNames) {
            if (gp == paramName) {
              isGenericParam = true;
              break;
            }
          }
          if (!isGenericParam || inferredBindings.count(paramName))
            break;
          auto stackSizeBefore = expressionStack_.size();
          fieldInit.value->accept(*this);
          if (expressionStack_.size() > stackSizeBefore) {
            auto preBound = std::move(expressionStack_.top());
            expressionStack_.pop();
            if (preBound && preBound->type) {
              inferredBindings[paramName] = preBound->type;
            }
          }
          break;
        }
      }

      if (!inferredBindings.empty()) {
        activeGenericBindingsStack_.push_back(std::move(inferredBindings));
        pushedInferredBindings = true;
      }
    }
  }

  auto mappedType = mapType(*node.type_);

  if (pushedInferredBindings) {
    activeGenericBindingsStack_.pop_back();
  }

  if (!mappedType || mappedType->getKind() != zir::TypeKind::Record) {
    error(node.span, "'" + node.type_->qualifiedName() + "' is not a struct.");
    return;
  }

  if (typeSymbol->isUnsafe) {
    requireUnsafeContext(node.span, "unsafe struct literals");
  }

  auto recordType = std::static_pointer_cast<zir::RecordType>(mappedType);
  const StructDeclarationNode *structDecl = nullptr;
  if (auto sdIt = structTypeDeclarationNodes_.find(typeSymbol.get());
      sdIt != structTypeDeclarationNodes_.end()) {
    structDecl = sdIt->second;
  }
  std::vector<std::pair<std::string, std::unique_ptr<BoundExpression>>>
      boundFields;
  std::vector<std::string> missingFields;

  for (auto &fieldInit : node.fields_) {
    std::shared_ptr<zir::Type> fieldExpectedType = nullptr;
    for (const auto &f : recordType->getFields()) {
      if (f.name == fieldInit.name) {
        fieldExpectedType = f.type;
        break;
      }
    }

    std::unique_ptr<BoundExpression> boundVal;
    if (fieldExpectedType) {
      boundVal =
          bindExpressionWithExpected(fieldInit.value.get(), fieldExpectedType);
    } else {
      fieldInit.value->accept(*this);
      if (expressionStack_.empty())
        continue;
      boundVal = std::move(expressionStack_.top());
      expressionStack_.pop();
    }
    if (!boundVal)
      continue;

    bool found = false;
    for (const auto &f : recordType->getFields()) {
      if (f.name == fieldInit.name) {
        if (!canConvert(boundVal->type, f.type)) {
          error(node.span, "Cannot assign type '" +
                               renderTypeForUser(boundVal->type) +
                               "' to field '" + f.name + "' of type '" +
                               renderTypeForUser(f.type) + "'");
        } else {
          boundVal = wrapInCast(std::move(boundVal), f.type);
        }
        found = true;
        break;
      }
    }

    if (!found) {
      error(fieldInit.value ? fieldInit.value->span : node.span,
            "Field '" + fieldInit.name + "' not found in struct '" +
                node.type_->qualifiedName() + "'");
    }

    boundFields.push_back({fieldInit.name, std::move(boundVal)});
  }

  for (const auto &f : recordType->getFields()) {
    bool initialized = false;
    for (const auto &bf : boundFields) {
      if (bf.first == f.name) {
        initialized = true;
        break;
      }
    }
    if (!initialized) {
      const ParameterNode *declField = nullptr;
      if (structDecl) {
        for (const auto &candidate : structDecl->fields_) {
          if (candidate->name == f.name) {
            declField = candidate.get();
            break;
          }
        }
      }

      std::unique_ptr<BoundExpression> defaultValue;
      bool hasDeclaredDefault = declField && declField->defaultValue;
      if (hasDeclaredDefault) {
        defaultValue =
            bindExpressionWithExpected(declField->defaultValue.get(), f.type);
        if (!defaultValue) {
          defaultValue = nullptr;
        } else if (!canConvert(defaultValue->type, f.type)) {
          error(declField->defaultValue->span,
                "Cannot assign default value of type '" +
                    renderTypeForUser(defaultValue->type) + "' to field '" +
                    f.name + "' of type '" + renderTypeForUser(f.type) + "'");
          defaultValue = nullptr;
        } else {
          defaultValue = wrapInCast(std::move(defaultValue), f.type);
        }
      }

      if (!defaultValue) {
        if (!hasDeclaredDefault) {
          missingFields.push_back(f.name);
        }
        defaultValue = makeDefaultValueExpr(f.type);
      }

      boundFields.push_back({f.name, std::move(defaultValue)});
    }
  }

  if (!missingFields.empty()) {
    std::string missingList;
    for (size_t i = 0; i < missingFields.size(); ++i) {
      if (i != 0) {
        missingList += ", ";
      }
      missingList += "'" + missingFields[i] + "'";
    }

    SourceSpan warningSpan = node.span;
    if (!node.fields_.empty() && node.fields_.front().value) {
      warningSpan = node.fields_.front().value->span;
    } else if (node.type_) {
      warningSpan = node.type_->span;
    }

    _diag.report(warningSpan, zap::DiagnosticLevel::Warning,
                 "Struct literal for '" + node.type_->qualifiedName() +
                     "' is missing fields: " + missingList +
                     ". Using default values.");
  }

  expressionStack_.push(
      std::make_unique<BoundStructLiteral>(std::move(boundFields), recordType));
}

} // namespace sema
