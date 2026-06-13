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

std::unique_ptr<BoundBlock> Binder::bindBody(BodyNode *body, bool createScope) {
  auto savedBlock = std::move(currentBlock_);
  if (createScope) {
    pushScope();
  }

  if (body) {
    body->accept(*this);
  }

  auto boundBody = std::make_unique<BoundBlock>();
  if (currentBlock_) {
    boundBody = std::move(currentBlock_);
  }

  if (createScope) {
    popScope();
  }

  currentBlock_ = std::move(savedBlock);
  return boundBody;
}

void Binder::visit(BodyNode &node) {
  currentBlock_ = std::make_unique<BoundBlock>();

  for (const auto &stmt : node.statements) {
    stmt->accept(*this);
    if (!statementStack_.empty()) {
      currentBlock_->statements.push_back(std::move(statementStack_.top()));
      statementStack_.pop();
    } else if (!expressionStack_.empty()) {
      auto expr = std::move(expressionStack_.top());
      expressionStack_.pop();
      currentBlock_->statements.push_back(
          std::make_unique<BoundExpressionStatement>(std::move(expr)));
    }
  }

  if (node.result) {
    node.result->accept(*this);
    if (!expressionStack_.empty()) {
      currentBlock_->result = std::move(expressionStack_.top());
      expressionStack_.pop();
    }
  }
}

void Binder::visit(UnsafeBlockNode &node) {
  requireUnsafeEnabled(node.span, "'unsafe' block");
  int oldUnsafeDepth = unsafeDepth_;
  ++unsafeDepth_;

  auto savedBlock = std::move(currentBlock_);
  pushScope();
  visit(static_cast<BodyNode &>(node));

  auto boundBody = std::make_unique<BoundBlock>();
  if (currentBlock_) {
    boundBody = std::move(currentBlock_);
  }

  popScope();
  currentBlock_ = std::move(savedBlock);
  unsafeDepth_ = oldUnsafeDepth;
  statementStack_.push(std::move(boundBody));
}

void Binder::visit(AsmStmtNode &node) {
  requireUnsafeContext(node.span, "inline 'asm'");

  std::vector<BoundAsmOperand> outputs;
  for (auto &operand : node.outputs) {
    operand.expr->accept(*this);
    if (expressionStack_.empty())
      return;
    auto bound = std::move(expressionStack_.top());
    expressionStack_.pop();

    bool isLValue =
        dynamic_cast<BoundVariableExpression *>(bound.get()) ||
        dynamic_cast<BoundIndexAccess *>(bound.get()) ||
        dynamic_cast<BoundMemberAccess *>(bound.get()) ||
        (dynamic_cast<BoundUnaryExpression *>(bound.get()) &&
         static_cast<BoundUnaryExpression *>(bound.get())->op == "*");
    if (!isLValue) {
      error(node.span, "Inline 'asm' output operand must be an l-value.");
      return;
    }
    if (auto varExpr = dynamic_cast<BoundVariableExpression *>(bound.get())) {
      if (varExpr->symbol->is_const) {
        error(node.span, "Cannot assign to constant '" + varExpr->symbol->name +
                             "' through inline 'asm'.");
        return;
      }
    }
    outputs.push_back({operand.constraint, std::move(bound)});
  }

  std::vector<BoundAsmOperand> inputs;
  for (auto &operand : node.inputs) {
    operand.expr->accept(*this);
    if (expressionStack_.empty())
      return;
    auto bound = std::move(expressionStack_.top());
    expressionStack_.pop();
    inputs.push_back({operand.constraint, std::move(bound)});
  }

  statementStack_.push(std::make_unique<BoundAsmStatement>(
      node.assembly, std::move(outputs), std::move(inputs), node.clobbers));
}

void Binder::visit(ReturnNode &node) {
  std::unique_ptr<BoundExpression> expr = nullptr;
  bool expressionHadDiagnostic = false;
  if (node.returnValue) {
    bool hadErrorBefore = hadError_;
    expr = bindExpressionWithExpected(
        node.returnValue.get(),
        currentFunction_ ? currentFunction_->returnType : nullptr);
    expressionHadDiagnostic = (hadError_ != hadErrorBefore);
    if (!expr) {
      // The return expression already produced a diagnostic.
      // Avoid cascading with a secondary "received Void" return-type error.
      statementStack_.push(std::make_unique<BoundReturnStatement>(
          nullptr, currentFunction_ && currentFunction_->returnsRef));
      return;
    }
  }

  if (currentFunction_) {
    auto expectedType = currentFunction_->returnType;
    auto actualType =
        expr ? expr->type
             : std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);

    if (isFailableType(expectedType) && !isFailableType(actualType)) {
      auto expectedValueType = failableValueType(expectedType);
      if (!canConvert(actualType, expectedValueType)) {
        error(node.span,
              "Function '" + currentFunction_->name +
                  "' expects return type '" + renderTypeForUser(expectedType) +
                  "', but received '" + renderTypeForUser(actualType) + "'");
      } else if (expr) {
        expr = wrapInCast(std::move(expr), expectedValueType);
        expr = makeFailableValueExpr(std::move(expr), expectedType);
      } else {
        expr = makeFailableValueExpr(makeDefaultValueExpr(expectedValueType),
                                     expectedType);
      }
    } else if (!canConvert(actualType, expectedType)) {
      if (expressionHadDiagnostic) {
        statementStack_.push(std::make_unique<BoundReturnStatement>(
            std::move(expr), currentFunction_ && currentFunction_->returnsRef));
        return;
      }
      error(node.span,
            "Function '" + currentFunction_->name + "' expects return type '" +
                renderTypeForUser(expectedType) + "', but received '" +
                renderTypeForUser(actualType) + "'");
    } else if (expr) {
      expr = wrapInCast(std::move(expr), expectedType);
    }
  }

  statementStack_.push(std::make_unique<BoundReturnStatement>(
      std::move(expr), currentFunction_ && currentFunction_->returnsRef));
}

void Binder::visit(FailNode &node) {
  if (!currentFunction_ || !isFailableType(currentFunction_->returnType)) {
    error(node.span, "'fail' can only be used inside failable functions.");
    return;
  }

  auto propagatedType = currentFunction_->returnType;
  auto expectedErrorType = failableErrorType(propagatedType);

  auto errExpr =
      bindExpressionWithExpected(node.errorValue_.get(), expectedErrorType);
  if (!errExpr) {
    return;
  }

  if (!canConvert(errExpr->type, expectedErrorType)) {
    error(node.errorValue_->span,
          "Cannot fail with error type '" + renderTypeForUser(errExpr->type) +
              "', expected '" + renderTypeForUser(expectedErrorType) + "'");
    return;
  }
  errExpr = wrapInCast(std::move(errExpr), expectedErrorType);

  statementStack_.push(std::make_unique<BoundFailStatement>(
      std::move(errExpr), propagatedType, expectedErrorType));
}

void Binder::visit(IfNode &node) {
  node.condition_->accept(*this);
  if (expressionStack_.empty())
    return;

  auto cond = std::move(expressionStack_.top());
  expressionStack_.pop();

  if (cond->type->getKind() != zir::TypeKind::Bool) {
    error(node.condition_->span, "If condition must be Bool, got '" +
                                     renderTypeForUser(cond->type) + "'");
  }

  auto thenBody = bindBody(node.thenBody_.get(), true);

  std::unique_ptr<BoundBlock> elseBody = nullptr;
  if (node.elseBody_) {
    elseBody = bindBody(node.elseBody_.get(), true);
  }

  statementStack_.push(std::make_unique<BoundIfStatement>(
      std::move(cond), std::move(thenBody), std::move(elseBody)));
}

void Binder::visit(IfTypeNode &node) {
  if (activeGenericBindingsStack_.empty()) {
    error(node.span,
          "'iftype' can only be used inside a generic instantiation.");
    return;
  }

  std::shared_ptr<zir::Type> actualType = nullptr;
  for (auto it = activeGenericBindingsStack_.rbegin();
       it != activeGenericBindingsStack_.rend(); ++it) {
    auto bindingIt = it->find(node.parameterName_);
    if (bindingIt != it->end()) {
      actualType = bindingIt->second;
      break;
    }
  }
  if (!actualType) {
    error(node.span,
          "'iftype' expects an active generic type parameter, got '" +
              node.parameterName_ + "'.");
    return;
  }

  auto matchType = mapType(*node.matchType_);
  if (!matchType) {
    error(node.matchType_->span,
          "Unknown type: " + node.matchType_->qualifiedName());
    return;
  }

  bool matched = renderTypeForUser(actualType) == renderTypeForUser(matchType);
  std::unique_ptr<BoundBlock> selectedBody = nullptr;
  if (matched) {
    selectedBody = bindBody(node.thenBody_.get(), true);
  } else if (node.elseBody_) {
    selectedBody = bindBody(node.elseBody_.get(), true);
  } else {
    selectedBody = std::make_unique<BoundBlock>();
  }

  statementStack_.push(std::move(selectedBody));
}

void Binder::visit(WhileNode &node) {
  node.condition_->accept(*this);
  if (expressionStack_.empty())
    return;

  auto cond = std::move(expressionStack_.top());
  expressionStack_.pop();

  if (cond->type->getKind() != zir::TypeKind::Bool) {
    error(node.condition_->span, "While condition must be Bool, got '" +
                                     renderTypeForUser(cond->type) + "'");
  }

  ++loopDepth_;
  auto body = bindBody(node.body_.get(), true);
  --loopDepth_;

  statementStack_.push(
      std::make_unique<BoundWhileStatement>(std::move(cond), std::move(body)));
}

void Binder::visit(ForNode &node) {
  pushScope();

  node.initializer_->accept(*this);
  std::unique_ptr<BoundStatement> initializer = nullptr;
  if (!statementStack_.empty()) {
    initializer = std::move(statementStack_.top());
    statementStack_.pop();
  }

  node.condition_->accept(*this);
  if (expressionStack_.empty()) {
    popScope();
    return;
  }

  auto condition = std::move(expressionStack_.top());
  expressionStack_.pop();
  if (condition->type->getKind() != zir::TypeKind::Bool) {
    error(node.condition_->span, "For condition must be Bool, got '" +
                                     renderTypeForUser(condition->type) + "'");
  }

  auto incrementTargetId =
      dynamic_cast<ConstId *>(node.increment_->target_.get());
  if (!incrementTargetId ||
      incrementTargetId->value_ != node.initializer_->name_) {
    error(node.increment_->target_->span,
          "For increment must assign to loop variable '" +
              node.initializer_->name_ + "'.");
  }

  node.increment_->accept(*this);
  std::unique_ptr<BoundStatement> increment = nullptr;
  if (!statementStack_.empty()) {
    increment = std::move(statementStack_.top());
    statementStack_.pop();
  }

  ++loopDepth_;
  auto body = bindBody(node.body_.get(), true);
  --loopDepth_;

  popScope();
  statementStack_.push(std::make_unique<BoundForStatement>(
      std::move(initializer), std::move(condition), std::move(increment),
      std::move(body)));
}

void Binder::visit(ForInNode &node) {
  const std::string moduleName =
      (modules_.count(currentModuleId_) && modules_[currentModuleId_].info)
          ? modules_[currentModuleId_].info->moduleName
          : "";

  pushScope();

  node.iterable_->accept(*this);
  if (expressionStack_.empty()) {
    popScope();
    return;
  }

  auto iterableValue = std::move(expressionStack_.top());
  expressionStack_.pop();

  auto intType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int);

  auto iterableName = makeSyntheticLoopName("iter");
  auto iterableSymbol = std::make_shared<VariableSymbol>(
      iterableName, iterableValue->type, false, false, iterableName, moduleName,
      Visibility::Private);
  currentScope_->declare(iterableName, iterableSymbol);

  auto indexName = makeSyntheticLoopName("idx");
  auto indexSymbol = std::make_shared<VariableSymbol>(
      indexName, intType, false, false, indexName, moduleName,
      Visibility::Private);
  currentScope_->declare(indexName, indexSymbol);

  auto initBlock = std::make_unique<BoundBlock>();
  initBlock->statements.push_back(std::make_unique<BoundVariableDeclaration>(
      iterableSymbol, std::move(iterableValue)));
  initBlock->statements.push_back(std::make_unique<BoundVariableDeclaration>(
      indexSymbol, std::make_unique<BoundLiteral>("0", intType)));

  auto makeIdExpr = [](const std::string &name) {
    return std::make_unique<ConstId>(name);
  };

  std::unique_ptr<ExpressionNode> conditionAst = nullptr;
  std::unique_ptr<ExpressionNode> elementAst = nullptr;
  auto iterableType = iterableSymbol->type;
  std::string accessName = iterableName;

  if (iterableType->getKind() == zir::TypeKind::Array) {
    auto arr = std::static_pointer_cast<zir::ArrayType>(iterableType);
    auto sliceName = makeSyntheticLoopName("slice");
    auto sliceType = makeVariadicViewType(arr->getBaseType());
    auto sliceSymbol = std::make_shared<VariableSymbol>(
        sliceName, sliceType, false, false, sliceName, moduleName,
        Visibility::Private);
    currentScope_->declare(sliceName, sliceSymbol);
    initBlock->statements.push_back(std::make_unique<BoundVariableDeclaration>(
        sliceSymbol,
        std::make_unique<BoundCast>(
            std::make_unique<BoundVariableExpression>(iterableSymbol),
            sliceType)));
    accessName = sliceName;
    conditionAst = std::make_unique<BinExpr>(
        makeIdExpr(indexName), "<",
        std::make_unique<MemberAccessNode>(makeIdExpr(accessName), "len"));
    elementAst = std::make_unique<IndexAccessNode>(makeIdExpr(accessName),
                                                   makeIdExpr(indexName));
  } else if (isVariadicViewType(iterableType)) {
    conditionAst = std::make_unique<BinExpr>(
        makeIdExpr(indexName), "<",
        std::make_unique<MemberAccessNode>(makeIdExpr(accessName), "len"));
    elementAst = std::make_unique<IndexAccessNode>(makeIdExpr(accessName),
                                                   makeIdExpr(indexName));
  } else if (iterableType->getKind() == zir::TypeKind::Class) {
    auto lenCall = std::make_unique<FunCall>();
    lenCall->callee_ =
        std::make_unique<MemberAccessNode>(makeIdExpr(accessName), "len");

    conditionAst = std::make_unique<BinExpr>(makeIdExpr(indexName), "<",
                                             std::move(lenCall));

    auto atCall = std::make_unique<FunCall>();
    atCall->callee_ =
        std::make_unique<MemberAccessNode>(makeIdExpr(accessName), "at");
    atCall->params_.push_back(
        std::make_unique<Argument>("", makeIdExpr(indexName), false, false));
    elementAst = std::move(atCall);
  } else {
    error(node.iterable_->span,
          "Type '" + renderTypeForUser(iterableType) +
              "' is not iterable in for-in. Expected array, slice, or class "
              "with 'len()' and 'at(Int)'.");
    popScope();
    return;
  }

  conditionAst->accept(*this);
  if (expressionStack_.empty()) {
    popScope();
    return;
  }
  auto condition = std::move(expressionStack_.top());
  expressionStack_.pop();
  if (condition->type->getKind() != zir::TypeKind::Bool) {
    error(node.span, "For-in condition must be Bool, got '" +
                         renderTypeForUser(condition->type) + "'");
  }

  elementAst->accept(*this);
  if (expressionStack_.empty()) {
    popScope();
    return;
  }
  auto elementValue = std::move(expressionStack_.top());
  expressionStack_.pop();

  auto increment = std::make_unique<BoundAssignment>(
      std::make_unique<BoundVariableExpression>(indexSymbol),
      std::make_unique<BoundBinaryExpression>(
          std::make_unique<BoundVariableExpression>(indexSymbol), "+",
          std::make_unique<BoundLiteral>("1", intType), intType));

  pushScope();
  auto itemSymbol = std::make_shared<VariableSymbol>(
      node.itemName_, elementValue->type, false, false, node.itemName_,
      moduleName, Visibility::Private);
  if (!currentScope_->declare(node.itemName_, itemSymbol)) {
    error(node.span, "Variable '" + node.itemName_ + "' already declared.");
  }

  std::shared_ptr<VariableSymbol> indexUserSymbol = nullptr;
  if (!node.indexName_.empty()) {
    indexUserSymbol = std::make_shared<VariableSymbol>(
        node.indexName_, intType, false, false, node.indexName_, moduleName,
        Visibility::Private);
    if (!currentScope_->declare(node.indexName_, indexUserSymbol)) {
      error(node.span, "Variable '" + node.indexName_ + "' already declared.");
    }
  }

  ++loopDepth_;
  auto body = bindBody(node.body_.get(), false);
  --loopDepth_;

  body->statements.insert(body->statements.begin(),
                          std::make_unique<BoundVariableDeclaration>(
                              itemSymbol, std::move(elementValue)));
  if (indexUserSymbol) {
    body->statements.insert(
        body->statements.begin(),
        std::make_unique<BoundVariableDeclaration>(
            indexUserSymbol,
            std::make_unique<BoundVariableExpression>(indexSymbol)));
  }
  popScope();

  popScope();
  statementStack_.push(std::make_unique<BoundForStatement>(
      std::move(initBlock), std::move(condition), std::move(increment),
      std::move(body)));
}

void Binder::visit(BreakNode &node) {
  if (loopDepth_ <= 0) {
    error(node.span, "'break' can only be used inside loops.");
    return;
  }
  statementStack_.push(std::make_unique<BoundBreakStatement>());
}

void Binder::visit(ContinueNode &node) {
  if (loopDepth_ <= 0) {
    error(node.span, "'continue' can only be used inside loops.");
    return;
  }
  statementStack_.push(std::make_unique<BoundContinueStatement>());
}

} // namespace sema
