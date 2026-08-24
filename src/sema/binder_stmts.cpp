#include "../ast/class_decl.hpp"
#include "../ast/const/const_char.hpp"
#include "../ast/defer_node.hpp"
#include "../ast/record_decl.hpp"
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

void Binder::emitDefersUpTo(std::vector<std::unique_ptr<BoundStatement>> &target, bool stopAtLoop) {
  for (auto it = deferScopes_.rbegin(); it != deferScopes_.rend(); ++it) {
    for (auto defIt = it->defers.rbegin(); defIt != it->defers.rend(); ++defIt) {
      const auto *defer = *defIt;
      if (!defer || !defer->statement_) {
        continue;
      }
      if (auto *body = dynamic_cast<BodyNode *>(defer->statement_.get())) {
        target.push_back(bindBody(body, true));
      } else {
        defer->statement_->accept(*this);
        if (!statementStack_.empty()) {
          target.push_back(std::move(statementStack_.top()));
          statementStack_.pop();
        } else if (!expressionStack_.empty()) {
          auto boundExpr = std::move(expressionStack_.top());
          expressionStack_.pop();
          target.push_back(std::make_unique<BoundExpressionStatement>(std::move(boundExpr)));
        }
      }
    }
    if (stopAtLoop && it->isLoopBoundary) {
      break;
    }
  }
}

std::unique_ptr<BoundBlock> Binder::bindBody(BodyNode *body, bool createScope) {
  auto savedBlock = std::move(currentBlock_);
  if (createScope) {
    pushScope();
  }

  deferScopes_.push_back(DeferScope{});

  if (body) {
    body->accept(*this);
  }

  auto boundBody = std::make_unique<BoundBlock>();
  if (currentBlock_) {
    boundBody = std::move(currentBlock_);
  }

  auto currentScopeDefers = std::move(deferScopes_.back());
  deferScopes_.pop_back();

  if (!blockAlwaysReturns(boundBody.get())) {
    for (auto defIt = currentScopeDefers.defers.rbegin(); defIt != currentScopeDefers.defers.rend(); ++defIt) {
      const auto *deferNode = *defIt;
      if (!deferNode || !deferNode->statement_) {
        continue;
      }
      if (auto *bodyNode = dynamic_cast<BodyNode *>(deferNode->statement_.get())) {
        boundBody->statements.push_back(bindBody(bodyNode, true));
      } else {
        deferNode->statement_->accept(*this);
        if (!statementStack_.empty()) {
          boundBody->statements.push_back(std::move(statementStack_.top()));
          statementStack_.pop();
        } else if (!expressionStack_.empty()) {
          auto bound = std::move(expressionStack_.top());
          expressionStack_.pop();
          boundBody->statements.push_back(std::make_unique<BoundExpressionStatement>(std::move(bound)));
        }
      }
    }
  }

  if (createScope) {
    popScope();
  }

  currentBlock_ = std::move(savedBlock);
  return boundBody;
}

void Binder::visit(DeferNode &node) {
  if (deferScopes_.empty()) {
    error(node.span, "'defer' can only be used inside a function or block.");
    return;
  }
  deferScopes_.back().defers.push_back(&node);
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

    if (!requireMutablePlace(*bound, node.span, MutablePlaceUse::AsmOutput)) {
      return;
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
      statementStack_.push(std::make_unique<BoundReturnStatement>(
          nullptr, currentFunction_ && currentFunction_->returnsRef));
      return;
    }
  }

  if (currentFunction_ && currentFunction_->returnType) {
    auto expectedType = currentFunction_->returnType;
    auto actualType =
        expr ? expr->type
             : std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);

    if (isFailableType(expectedType) && !isFailableType(actualType)) {
      auto expectedValueType = failableValueType(expectedType);
      auto conversion =
          conversions_.classifyImplicit(actualType, expectedValueType);
      if (!conversion) {
        error(node.span,
              "Function '" + currentFunction_->name +
                  "' expects return type '" + renderTypeForUser(expectedType) +
                  "', but received '" + renderTypeForUser(actualType) + "'");
      } else if (expr) {
        expr = applyConversion(std::move(expr), *conversion);
        expr = makeFailableValueExpr(std::move(expr), expectedType);
      } else {
        expr = makeFailableValueExpr(makeDefaultValueExpr(expectedValueType),
                                     expectedType);
      }
    } else {
      auto conversion = conversions_.classifyImplicit(actualType, expectedType);
      if (!conversion) {
        if (expressionHadDiagnostic) {
          statementStack_.push(std::make_unique<BoundReturnStatement>(
              std::move(expr),
              currentFunction_ && currentFunction_->returnsRef));
          return;
        }
        error(node.span,
              "Function '" + currentFunction_->name +
                  "' expects return type '" + renderTypeForUser(expectedType) +
                  "', but received '" + renderTypeForUser(actualType) + "'");
      } else if (expr) {
        expr = applyConversion(std::move(expr), *conversion);
      }
    }
  }

  if (expr && currentFunction_ && currentFunction_->returnsRef &&
      !requireMutablePlace(*expr, node.span,
                           MutablePlaceUse::MutableReference)) {
    statementStack_.push(std::make_unique<BoundReturnStatement>(
        std::move(expr), currentFunction_->returnsRef));
    return;
  }

  std::vector<std::unique_ptr<BoundStatement>> deferStmts;
  emitDefersUpTo(deferStmts, false);

  if (!deferStmts.empty()) {
    auto returnBlock = std::make_unique<BoundBlock>();
    std::shared_ptr<VariableSymbol> tempReturnSymbol = nullptr;

    if (expr && expr->type && expr->type->getKind() != zir::TypeKind::Void) {
      auto tempName = makeSyntheticLoopName("ret_val");
      tempReturnSymbol = std::make_shared<VariableSymbol>(
          tempName, expr->type, BindingKind::Immutable, false, tempName,
          modules_[currentModuleId_].info->moduleName, Visibility::Private);
      currentScope_->declare(tempName, tempReturnSymbol);
      returnBlock->statements.push_back(std::make_unique<BoundVariableDeclaration>(tempReturnSymbol, std::move(expr)));
    }

    for (auto &defStmt : deferStmts) {
      returnBlock->statements.push_back(std::move(defStmt));
    }

    std::unique_ptr<BoundExpression> finalReturnExpr = nullptr;
    if (tempReturnSymbol) {
      finalReturnExpr = std::make_unique<BoundVariableExpression>(tempReturnSymbol);
    } else if (expr) {
      finalReturnExpr = std::move(expr);
    }

    returnBlock->statements.push_back(std::make_unique<BoundReturnStatement>(std::move(finalReturnExpr), currentFunction_ && currentFunction_->returnsRef));
    statementStack_.push(std::move(returnBlock));
    return;
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

  auto conversion =
      conversions_.classifyImplicit(errExpr->type, expectedErrorType);
  if (!conversion) {
    error(node.errorValue_->span,
          "Cannot fail with error type '" + renderTypeForUser(errExpr->type) +
              "', expected '" + renderTypeForUser(expectedErrorType) + "'");
    return;
  }
  errExpr = applyConversion(std::move(errExpr), *conversion);

  std::vector<std::unique_ptr<BoundStatement>> deferStmts;
  emitDefersUpTo(deferStmts, false);

  if (!deferStmts.empty()) {
    auto fail = std::make_unique<BoundBlock>();
    auto name = makeSyntheticLoopName("fail_err");
    auto err = std::make_shared<VariableSymbol>(
        name, errExpr->type, BindingKind::Immutable, false, name,
        modules_[currentModuleId_].info->moduleName, Visibility::Private);
    currentScope_->declare(name, err);
    fail->statements.push_back(std::make_unique<BoundVariableDeclaration>(err, std::move(errExpr)));

    for (auto &defStmt : deferStmts) {
      fail->statements.push_back(std::move(defStmt));
    }

    fail->statements.push_back(std::make_unique<BoundFailStatement>(std::make_unique<BoundVariableExpression>(err), propagatedType, expectedErrorType));
    statementStack_.push(std::move(fail));
    return;
  }

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

  std::shared_ptr<VariableSymbol> narrowedSource;
  std::shared_ptr<VariableSymbol> narrowedVariable;
  BoundClassTypeTest *typeTest = dynamic_cast<BoundClassTypeTest *>(cond.get());
  if (!typeTest) {
    if (auto *binary = dynamic_cast<BoundBinaryExpression *>(cond.get());
        binary && binary->op == "&&") {
      typeTest = dynamic_cast<BoundClassTypeTest *>(binary->left.get());
    }
  }
  if (typeTest) {
    if (auto *variable = dynamic_cast<BoundVariableExpression *>(
            typeTest->expression.get())) {
      narrowedSource = variable->symbol;
      narrowedVariable = std::make_shared<VariableSymbol>(*narrowedSource);
      narrowedVariable->type = typeTest->targetType;
    }
  }

  std::unique_ptr<BoundBlock> thenBody;
  if (narrowedVariable) {
    pushScope();
    currentScope_->declare(narrowedVariable->name, narrowedVariable);
    thenBody = bindBody(node.thenBody_.get(), false);
    popScope();
  } else {
    thenBody = bindBody(node.thenBody_.get(), true);
  }

  std::unique_ptr<BoundBlock> elseBody = nullptr;
  if (node.elseBody_) {
    elseBody = bindBody(node.elseBody_.get(), true);
  }

  statementStack_.push(std::make_unique<BoundIfStatement>(
      std::move(cond), std::move(thenBody), std::move(elseBody),
      std::move(narrowedSource), std::move(narrowedVariable)));
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
  deferScopes_.push_back(DeferScope{true, {}});
  auto body = bindBody(node.body_.get(), true);
  deferScopes_.pop_back();
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
  deferScopes_.push_back(DeferScope{true, {}});
  auto body = bindBody(node.body_.get(), true);
  deferScopes_.pop_back();
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

  if (auto rangeExpr = dynamic_cast<BoundRangeExpression *>(iterableValue.get())) {
    auto rangeType = rangeExpr->type;
    auto start = std::move(rangeExpr->start);
    auto end = std::move(rangeExpr->end);
    auto step = std::move(rangeExpr->step);

    std::optional<int64_t> constantStep;
    if (step) {
      constantStep = evaluateConstantInt(step.get());
    } else {
      constantStep = 1;
      step = std::make_unique<BoundLiteral>("1", rangeType);
    }

    auto initBlock = std::make_unique<BoundBlock>();

    auto counterName = makeSyntheticLoopName("val");
    auto valCounterSymbol = std::make_shared<VariableSymbol>(
        counterName, rangeType, BindingKind::Mutable, false, counterName,
        moduleName, Visibility::Private);
    currentScope_->declare(counterName, valCounterSymbol);
    initBlock->statements.push_back(std::make_unique<BoundVariableDeclaration>(
        valCounterSymbol, std::move(start)));

    auto endName = makeSyntheticLoopName("end");
    auto endCounterSymbol = std::make_shared<VariableSymbol>(endName, rangeType, BindingKind::Immutable, false, endName,moduleName, Visibility::Private);
    currentScope_->declare(endName, endCounterSymbol);
    initBlock->statements.push_back(std::make_unique<BoundVariableDeclaration>(endCounterSymbol, std::move(end)));

    auto stepCounterName = makeSyntheticLoopName("step");
    auto stepCounterSymbol = std::make_shared<VariableSymbol>(
        stepCounterName, rangeType, BindingKind::Immutable, false, stepCounterName,
        moduleName, Visibility::Private);
    currentScope_->declare(stepCounterName, stepCounterSymbol);
    initBlock->statements.push_back(std::make_unique<BoundVariableDeclaration>(stepCounterSymbol, std::move(step)));

    std::shared_ptr<VariableSymbol> idxCounterSymbol = nullptr;
    if (!node.indexName_.empty()) {
      auto idxCounterName = makeSyntheticLoopName("idx");
      idxCounterSymbol = std::make_shared<VariableSymbol>(
          idxCounterName, intType, BindingKind::Mutable, false, idxCounterName,
          moduleName, Visibility::Private);
      currentScope_->declare(idxCounterName, idxCounterSymbol);
      initBlock->statements.push_back(std::make_unique<BoundVariableDeclaration>(idxCounterSymbol, std::make_unique<BoundLiteral>("0", intType)));
    }

    auto boolType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Bool);
    std::unique_ptr<BoundExpression> condition;

    if (constantStep.has_value()) {
      std::string cmpOp = (*constantStep < 0) ? ">" : "<";
      condition = std::make_unique<BoundBinaryExpression>(std::make_unique<BoundVariableExpression>(valCounterSymbol), cmpOp,std::make_unique<BoundVariableExpression>(endCounterSymbol), boolType);
    } else {
      auto zeroLiteral = std::make_unique<BoundLiteral>("0", rangeType);
      auto stepIsPositive = std::make_unique<BoundBinaryExpression>(std::make_unique<BoundVariableExpression>(stepCounterSymbol), ">",std::move(zeroLiteral), boolType);
      auto posCondition = std::make_unique<BoundBinaryExpression>(std::make_unique<BoundVariableExpression>(valCounterSymbol), "<", std::make_unique<BoundVariableExpression>(endCounterSymbol), boolType);
      auto negCondition = std::make_unique<BoundBinaryExpression>(std::make_unique<BoundVariableExpression>(valCounterSymbol), ">",std::make_unique<BoundVariableExpression>(endCounterSymbol), boolType);

      condition = std::make_unique<BoundTernaryExpression>(std::move(stepIsPositive), std::move(posCondition),std::move(negCondition), boolType);
    }

    auto increment = std::make_unique<BoundAssignment>(
        std::make_unique<BoundVariableExpression>(valCounterSymbol),
        std::make_unique<BoundBinaryExpression>(
            std::make_unique<BoundVariableExpression>(valCounterSymbol), "+",
            std::make_unique<BoundVariableExpression>(stepCounterSymbol), rangeType));

    pushScope();
    auto itemSymbol = std::make_shared<VariableSymbol>(node.itemName_, rangeType, BindingKind::Immutable, false,node.itemName_, moduleName, Visibility::Private);
    if (!currentScope_->declare(node.itemName_, itemSymbol)) {
      error(node.span, "Variable '" + node.itemName_ + "' already declared.");
    }
    if (semanticInfo_) {
      semanticInfo_->recordSymbol(&node, itemSymbol);
      semanticInfo_->recordDeclaration(&node, itemSymbol);
      semanticInfo_->recordType(&node, itemSymbol->type);
    }

    std::shared_ptr<VariableSymbol> indexUserSymbol = nullptr;
    if (!node.indexName_.empty()) {
      indexUserSymbol = std::make_shared<VariableSymbol>(
          node.indexName_, intType, BindingKind::Immutable, false,
          node.indexName_, moduleName, Visibility::Private);
      if (!currentScope_->declare(node.indexName_, indexUserSymbol)) {
        error(node.span, "Variable '" + node.indexName_ + "' already declared.");
      }
    }

    ++loopDepth_;
    deferScopes_.push_back(DeferScope{true, {}});
    auto body = bindBody(node.body_.get(), false);
    deferScopes_.pop_back();
    --loopDepth_;

    body->statements.insert(
        body->statements.begin(),
        std::make_unique<BoundVariableDeclaration>(
            itemSymbol,
            std::make_unique<BoundVariableExpression>(valCounterSymbol)));

    if (indexUserSymbol) {
      body->statements.insert(
          body->statements.begin(),
          std::make_unique<BoundVariableDeclaration>(
              indexUserSymbol,
              std::make_unique<BoundVariableExpression>(idxCounterSymbol)));

      body->statements.push_back(std::make_unique<BoundAssignment>(
          std::make_unique<BoundVariableExpression>(idxCounterSymbol),
          std::make_unique<BoundBinaryExpression>(
              std::make_unique<BoundVariableExpression>(idxCounterSymbol), "+",
              std::make_unique<BoundLiteral>("1", intType), intType)));
    }
    popScope();

    popScope();
    statementStack_.push(std::make_unique<BoundForStatement>(
        std::move(initBlock), std::move(condition), std::move(increment),
        std::move(body)));
    return;
  }

  auto iterableName = makeSyntheticLoopName("iter");
  auto iterableSymbol = std::make_shared<VariableSymbol>(
      iterableName, iterableValue->type, BindingKind::Mutable, false,
      iterableName, moduleName, Visibility::Private);
  currentScope_->declare(iterableName, iterableSymbol);

  auto indexName = makeSyntheticLoopName("idx");
  auto indexSymbol = std::make_shared<VariableSymbol>(
      indexName, intType, BindingKind::Mutable, false, indexName, moduleName,
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
        sliceName, sliceType, BindingKind::Mutable, false, sliceName,
        moduleName, Visibility::Private);
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
      node.itemName_, elementValue->type, BindingKind::Immutable, false,
      node.itemName_, moduleName, Visibility::Private);
  if (!currentScope_->declare(node.itemName_, itemSymbol)) {
    error(node.span, "Variable '" + node.itemName_ + "' already declared.");
  }
  if (semanticInfo_) {
    semanticInfo_->recordSymbol(&node, itemSymbol);
    semanticInfo_->recordDeclaration(&node, itemSymbol);
    semanticInfo_->recordType(&node, itemSymbol->type);
  }

  std::shared_ptr<VariableSymbol> indexUserSymbol = nullptr;
  if (!node.indexName_.empty()) {
    indexUserSymbol = std::make_shared<VariableSymbol>(
        node.indexName_, intType, BindingKind::Immutable, false,
        node.indexName_, moduleName, Visibility::Private);
    if (!currentScope_->declare(node.indexName_, indexUserSymbol)) {
      error(node.span, "Variable '" + node.indexName_ + "' already declared.");
    }
  }

  ++loopDepth_;
  deferScopes_.push_back(DeferScope{true, {}});
  auto body = bindBody(node.body_.get(), false);
  deferScopes_.pop_back();
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
  std::vector<std::unique_ptr<BoundStatement>> deferStmts;
  emitDefersUpTo(deferStmts, true);

  if (!deferStmts.empty()) {
    auto breakBlock = std::make_unique<BoundBlock>();
    for (auto &defStmt : deferStmts) {
      breakBlock->statements.push_back(std::move(defStmt));
    }
    breakBlock->statements.push_back(std::make_unique<BoundBreakStatement>());
    statementStack_.push(std::move(breakBlock));
    return;
  }
  statementStack_.push(std::make_unique<BoundBreakStatement>());
}

void Binder::visit(ContinueNode &node) {
  if (loopDepth_ <= 0) {
    error(node.span, "'continue' can only be used inside loops.");
    return;
  }
  std::vector<std::unique_ptr<BoundStatement>> deferStmts;
  emitDefersUpTo(deferStmts, true);

  if (!deferStmts.empty()) {
    auto continueBlock = std::make_unique<BoundBlock>();
    for (auto &defStmt : deferStmts) {
      continueBlock->statements.push_back(std::move(defStmt));
    }
    continueBlock->statements.push_back(std::make_unique<BoundContinueStatement>());
    statementStack_.push(std::move(continueBlock));
    return;
  }
  statementStack_.push(std::make_unique<BoundContinueStatement>());
}

} // namespace sema