#include "../ast/class_decl.hpp"
#include "../ast/const/const_char.hpp"
#include "../ast/record_decl.hpp"
#include "../ir/string_type.hpp"
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
  if (node.op_ == "is") {
    auto left = bindExpressionWithExpected(node.left_.get(), nullptr);
    auto right = bindExpressionWithExpected(node.right_.get(), nullptr);
    if (!left || !right || !dynamic_cast<BoundLiteral *>(right.get())) {
      error(node.span, "'is' expects a class type on its right-hand side.");
      return;
    }
    if (right->type->getKind() != zir::TypeKind::Class) {
      error(node.right_->span, "'is' expects a class type.");
      return;
    }
    if (left->type->getKind() != zir::TypeKind::Class) {
      error(node.left_->span,
            "'is' expects a class reference on its left-hand side.");
      return;
    }
    if (std::static_pointer_cast<zir::ClassType>(left->type)->isWeak()) {
      error(node.left_->span,
            "'is' does not accept weak class references; lock it first.");
      return;
    }
    expressionStack_.push(std::make_unique<BoundClassTypeTest>(
        std::move(left), std::static_pointer_cast<zir::ClassType>(right->type)));
    return;
  }

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
  auto applyJoin = [&](const TypeJoin &join) {
    resultType = join.type;
    left = applyConversion(std::move(left), join.leftConversion);
    right = applyConversion(std::move(right), join.rightConversion);
  };

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
    resultType = zir::makeStringType();
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
      if (!typeInterner_.same(leftType, rightType)) {
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
      auto join = conversions_.joinTypes(leftType, rightType);
      if (join) {
        applyJoin(*join);
      }
    }
  } else if (op == "&" || op == "|" || op == "^") {
    if (!leftType->isInteger() || !rightType->isInteger()) {
      error(SourceSpan::merge(leftSpan, rightSpan),
            "Bitwise operator '" + op + "' requires integer operands, got '" +
                renderTypeForUser(leftType) + "' and '" +
                renderTypeForUser(rightType) + "'");
    } else {
      auto join = conversions_.joinTypes(leftType, rightType);
      if (join) {
        applyJoin(*join);
      }
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
      auto shiftAmount = evaluateConstantInt(right.get());
      right = applyConversion(std::move(right), *conversions_.classifyImplicit(
                                                    rightType, resultType));

      if (shiftAmount) {
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

    if (stringComparison) {
      auto stringViewType = zir::makeStringViewType();
      left = applyConversion(std::move(left), *conversions_.classifyImplicit(
                                                  leftType, stringViewType));
      right = applyConversion(std::move(right), *conversions_.classifyImplicit(
                                                    rightType, stringViewType));
    } else if (auto join = conversions_.joinTypes(leftType, rightType)) {
      applyJoin(*join);
    } else {
      error(SourceSpan::merge(leftSpan, rightSpan),
            "Cannot compare '" + renderTypeForUser(leftType) + "' and '" +
                renderTypeForUser(rightType) + "'");
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

  auto join = conversions_.joinTypes(thenExpr->type, elseExpr->type);
  if (!join) {
    error(SourceSpan::merge(node.thenExpr_->span, node.elseExpr_->span),
          "Ternary branches must be compatible, got '" +
              renderTypeForUser(thenExpr->type) + "' and '" +
              renderTypeForUser(elseExpr->type) + "'");
    return;
  }

  auto resultType = join->type;
  thenExpr = applyConversion(std::move(thenExpr), join->leftConversion);
  elseExpr = applyConversion(std::move(elseExpr), join->rightConversion);

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
  expressionStack_.push(
      std::make_unique<BoundLiteral>(node.value_, zir::makeStringViewType()));
}

void Binder::visit(ConstChar &node) {
  expressionStack_.push(std::make_unique<BoundLiteral>(
      node.value_, std::make_shared<zir::PrimitiveType>(zir::TypeKind::Char)));
}

void Binder::visit(ConstNull &) {
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

  if (!conversions_.classifyExplicit(expr->type, targetType)) {
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
      !typeInterner_.same(currentErrorType, errorType)) {
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

  auto conversion = conversions_.classifyImplicit(fallback->type, valueType);
  if (!conversion) {
    error(node.fallback_->span, "Fallback expression type '" +
                                    renderTypeForUser(fallback->type) +
                                    "' is not compatible with '" +
                                    renderTypeForUser(valueType) + "'");
    return;
  }
  fallback = applyConversion(std::move(fallback), *conversion);

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
      node.errorName_, errorType, BindingKind::Mutable, false, node.errorName_,
      modules_[currentModuleId_].info->moduleName, Visibility::Private);
  if (!currentScope_->declare(node.errorName_, errorSymbol)) {
    error(node.span, "Handler variable '" + node.errorName_ +
                         "' is already declared in this scope.");
  }
  if (semanticInfo_) {
    semanticInfo_->recordSymbol(&node, errorSymbol);
    semanticInfo_->recordDeclaration(&node, errorSymbol);
    semanticInfo_->recordType(&node, errorType);
  }

  auto handler = bindBody(node.handler_.get(), false);
  popScope();

  if (handler && !handler->result) {
    handler->result = deriveValueExpressionFromBlock(*handler);
  }

  std::shared_ptr<zir::Type> handlerResultType = valueType;
  if (handler && handler->result) {
    auto conversion =
        conversions_.classifyImplicit(handler->result->type, valueType);
    if (!conversion) {
      error(node.span, "Handler result type '" +
                           renderTypeForUser(handler->result->type) +
                           "' is not compatible with '" +
                           renderTypeForUser(valueType) + "'");
      return;
    }
    handler->result = applyConversion(std::move(handler->result), *conversion);
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
          if (overload->returnsRef != fpType.returnsRef()) {
            continue;
          }
          if (overload->resultBorrow != fpType.getResultBorrow()) {
            continue;
          }
          bool ok = true;
          for (size_t i = 0; i < fpType.getParams().size(); ++i) {
            if (!conversions_.classifyImplicit(fpType.getParams()[i],
                                               overload->parameters[i]->type)) {
              ok = false;
              break;
            }
            const auto expectedEscape = fpType.getParameterEscapes()[i];
            const auto actualEscape = overload->parameters[i]->is_noescape
                                          ? zir::ParameterEscape::NoEscape
                                          : zir::ParameterEscape::Unspecified;
            if (expectedEscape != actualEscape) {
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
      std::vector<zir::ParameterOwnership> ownership;
      std::vector<zir::ParameterEscape> escape;
      for (size_t i = 0; i < match->parameters.size(); ++i) {
        const auto &p = match->parameters[i];
        params.push_back(p->type);
        const bool borrowedSelf =
            i == 0 && !match->ownerTypeCodegenName.empty() && p->name == "self";
        const bool transfers = !match->isExternal && !p->is_ref &&
                               !p->is_variadic_pack && !borrowedSelf &&
                               zir::containsManagedValues(p->type);
        ownership.push_back(
            transfers ? (p->is_sink ? zir::ParameterOwnership::Sink
                                    : zir::ParameterOwnership::Transfer)
                      : zir::ParameterOwnership::Borrow);
        escape.push_back(p->is_noescape ? zir::ParameterEscape::NoEscape
                                        : zir::ParameterEscape::Unspecified);
      }
      auto fpType = std::make_shared<zir::FunctionPointerType>(
          std::move(params), match->returnType, std::move(ownership),
          std::move(escape), match->resultBorrow, match->returnsRef);
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

  if (!requireMutablePlace(*target, node.span, MutablePlaceUse::Assignment)) {
    return;
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

  auto conversion = conversions_.classifyImplicit(expr->type, target->type);
  if (!conversion) {
    error(node.span, "Cannot assign expression of type '" +
                         renderTypeForUser(expr->type) + "' to type '" +
                         renderTypeForUser(target->type) + "'");
  } else if (conversion->kind == ConversionKind::StringToView &&
             !dynamic_cast<BoundVariableExpression *>(expr.get()) &&
             !dynamic_cast<BoundMemberAccess *>(expr.get())) {
    error(node.expr_->span,
          "Cannot assign a temporary String to a StringView; store it in a "
          "String variable or use a String variable as the view owner.");
  } else {
    expr = applyConversion(std::move(expr), *conversion);
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
    if (!stringIndexFunction_) {
      error(node.span,
            "String indexing requires the core.at(StringView, Int) helper.");
      return;
    }
    auto leftConversion = conversions_.classifyImplicit(
        left->type, stringIndexFunction_->parameters[0]->type);
    auto indexConversion = conversions_.classifyImplicit(
        index->type, stringIndexFunction_->parameters[1]->type);
    if (!leftConversion || !indexConversion) {
      error(node.span, "String index helper has an incompatible signature.");
      return;
    }
    left = applyConversion(std::move(left), *leftConversion);
    index = applyConversion(std::move(index), *indexConversion);

    std::vector<std::unique_ptr<BoundExpression>> arguments;
    arguments.push_back(std::move(left));
    arguments.push_back(std::move(index));
    expressionStack_.push(std::make_unique<BoundFunctionCall>(
        stringIndexFunction_, std::move(arguments)));
    return;
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
      auto infoIt = classInfos_.find(classType->getCodegenName());
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
    auto infoIt = classInfos_.find(classType->getCodegenName());
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
  if (concreteType->isInterface()) {
    error(node.span, "Cannot instantiate interface '" +
                         concreteType->getName() + "' with 'new'.");
    return;
  }
  auto infoIt = classInfos_.find(concreteType->getCodegenName());
  if (infoIt == classInfos_.end()) {
    error(node.span, "Unknown class type: " + concreteType->getName());
    return;
  }

  std::vector<std::unique_ptr<BoundExpression>> args;
  std::vector<bool> argRefs;
  std::vector<std::unique_ptr<BoundExpression>> rawArgs;
  rawArgs.reserve(node.args_.size());
  for (size_t i = 0; i < node.args_.size(); ++i) {
    auto arg = bindExpressionWithExpected(node.args_[i]->value.get(), nullptr);
    if (!arg) {
      return;
    }
    rawArgs.push_back(std::move(arg));
  }

  auto ctor = infoIt->second.constructor;
  auto ctorIt = infoIt->second.methods.find("init");
  auto ctorCandidates = ctorIt == infoIt->second.methods.end()
                            ? std::vector<std::shared_ptr<FunctionSymbol>>{}
                            : collectOverloads(ctorIt->second);
  if (ctorCandidates.empty() && ctor) {
    ctorCandidates.push_back(ctor);
  }

  struct ConstructorCandidate {
    std::shared_ptr<FunctionSymbol> symbol;
    std::vector<int> cost;
  };

  std::vector<ConstructorCandidate> matches;
  if (ctorCandidates.empty()) {
    if (!node.args_.empty()) {
      error(node.span, "Constructor for class '" + concreteType->getName() +
                           "' expects 0 arguments, got " +
                           std::to_string(node.args_.size()) + ".");
      return;
    }
  } else {
    for (const auto &candidate : ctorCandidates) {
      if (!candidate) {
        continue;
      }
      size_t ctorParamOffset = candidate->isMethod ? 1 : 0;
      if (node.args_.size() + ctorParamOffset != candidate->parameters.size()) {
        continue;
      }

      ConstructorCandidate match;
      match.symbol = candidate;
      bool failed = false;
      for (size_t i = 0; i < rawArgs.size(); ++i) {
        auto expected = candidate->parameters[i + ctorParamOffset]->type;
        auto conversion =
            conversions_.classifyImplicit(rawArgs[i]->type, expected);
        if (!conversion) {
          failed = true;
          break;
        }
        match.cost.push_back(conversion->cost());
      }
      if (!failed) {
        matches.push_back(std::move(match));
      }
    }

    if (matches.empty()) {
      error(node.span, "No matching constructor for class '" +
                           concreteType->getName() + "'.");
      return;
    }

    std::sort(
        matches.begin(), matches.end(),
        [](const ConstructorCandidate &lhs, const ConstructorCandidate &rhs) {
          return lhs.cost < rhs.cost;
        });
    if (matches.size() > 1 && matches[0].cost == matches[1].cost) {
      error(node.span, "Ambiguous constructor for class '" +
                           concreteType->getName() + "'.");
      return;
    }

    ctor = matches.front().symbol;
  }

  size_t ctorParamOffset = ctor && ctor->isMethod ? 1 : 0;
  for (size_t i = 0; i < rawArgs.size(); ++i) {
    auto arg = rawArgs[i]->clone();
    if (node.args_[i]->isRef &&
        !requireMutablePlace(*arg, node.args_[i]->value->span,
                             MutablePlaceUse::MutableReference)) {
      return;
    }
    auto expected =
        ctor ? ctor->parameters[i + ctorParamOffset]->type : nullptr;
    if (expected) {
      if (auto conversion =
              conversions_.classifyImplicit(arg->type, expected)) {
        arg = applyConversion(std::move(arg), *conversion);
      }
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
    requireMutablePlace(*expr, node.span, MutablePlaceUse::Address);

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
  bool hasExpectedElementType =
      expectedType && expectedType->getKind() == zir::TypeKind::Array;
  if (hasExpectedElementType) {
    elementType =
        std::static_pointer_cast<zir::ArrayType>(expectedType)->getBaseType();
  }

  for (const auto &el : node.elements_) {
    auto boundEl = bindExpressionWithExpected(el.get(), elementType);
    if (boundEl) {
      if (!elementType) {
        elementType = boundEl->type;
      } else if (hasExpectedElementType) {
        auto conversion =
            conversions_.classifyImplicit(boundEl->type, elementType);
        if (conversion) {
          boundEl = applyConversion(std::move(boundEl), *conversion);
        } else {
          error(el->span, "Array elements must have the same type. Expected '" +
                              renderTypeForUser(elementType) + "', but got '" +
                              renderTypeForUser(boundEl->type) + "'");
          continue;
        }
      } else {
        auto join = conversions_.joinTypes(elementType, boundEl->type);
        if (!join) {
          error(el->span, "Array elements must have a common type, got '" +
                              renderTypeForUser(elementType) + "' and '" +
                              renderTypeForUser(boundEl->type) + "'");
          continue;
        } else {
          for (auto &element : elements) {
            element = applyConversion(std::move(element), join->leftConversion);
          }
          boundEl = applyConversion(std::move(boundEl), join->rightConversion);
          elementType = join->type;
        }
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
        auto conversion = conversions_.classifyImplicit(boundVal->type, f.type);
        if (!conversion) {
          error(node.span, "Cannot assign type '" +
                               renderTypeForUser(boundVal->type) +
                               "' to field '" + f.name + "' of type '" +
                               renderTypeForUser(f.type) + "'");
        } else {
          boundVal = applyConversion(std::move(boundVal), *conversion);
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
        } else if (auto conversion = conversions_.classifyImplicit(
                       defaultValue->type, f.type)) {
          defaultValue = applyConversion(std::move(defaultValue), *conversion);
        } else {
          error(declField->defaultValue->span,
                "Cannot assign default value of type '" +
                    renderTypeForUser(defaultValue->type) + "' to field '" +
                    f.name + "' of type '" + renderTypeForUser(f.type) + "'");
          defaultValue = nullptr;
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

void Binder::visit(RangeExpr &node) {
  auto start = bindExpressionWithExpected(node.start_.get(), nullptr);
  auto end = bindExpressionWithExpected(node.end_.get(), start ? start->type : nullptr);
  if (!start || !end) {
    return;
  }

  if (!start->type->isInteger() || !end->type->isInteger()) {
    error(node.span, "Range bounds must be integer types, got '" + renderTypeForUser(start->type) + "' and '" + renderTypeForUser(end->type) + "'");
    return;
  }

  auto join = conversions_.joinTypes(start->type, end->type);
  if (!join) {
    error(node.span, "Range bounds must have compatible types, got '" + renderTypeForUser(start->type) + "' and '" + renderTypeForUser(end->type) + "'");
    return;
  }

  auto type = join->type;
  start = applyConversion(std::move(start), join->leftConversion);
  end = applyConversion(std::move(end), join->rightConversion);

  std::unique_ptr<BoundExpression> step = nullptr;
  if (node.step_) {
    step = bindExpressionWithExpected(node.step_.get(), type);
    if (step) {
      if (!step->type->isInteger()) {
        error(node.step_->span, "Range step must be an integer, got '" + renderTypeForUser(step->type) + "'");
        return;
      }
      auto conv = conversions_.classifyImplicit(step->type, type);
      if (!conv) {
        error(node.step_->span, "Range step type '" + renderTypeForUser(step->type) + "' is incompatible with range type '" + renderTypeForUser(type) + "'");
        return;
      }
      step = applyConversion(std::move(step), *conv);
    }
  }

  expressionStack_.push(std::make_unique<BoundRangeExpression>(std::move(start), std::move(end), std::move(step), type));
}

} // namespace sema
