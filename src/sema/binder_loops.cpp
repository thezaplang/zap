#include "../ir/string_type.hpp"
#include "binder.hpp"
#include <algorithm>

namespace sema {

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

  if (auto rangeExpr =
          dynamic_cast<BoundRangeExpression *>(iterableValue.get())) {
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
    auto endCounterSymbol = std::make_shared<VariableSymbol>(
        endName, rangeType, BindingKind::Immutable, false, endName, moduleName,
        Visibility::Private);
    currentScope_->declare(endName, endCounterSymbol);
    initBlock->statements.push_back(std::make_unique<BoundVariableDeclaration>(
        endCounterSymbol, std::move(end)));

    auto stepCounterName = makeSyntheticLoopName("step");
    auto stepCounterSymbol = std::make_shared<VariableSymbol>(
        stepCounterName, rangeType, BindingKind::Immutable, false,
        stepCounterName, moduleName, Visibility::Private);
    currentScope_->declare(stepCounterName, stepCounterSymbol);
    initBlock->statements.push_back(std::make_unique<BoundVariableDeclaration>(
        stepCounterSymbol, std::move(step)));

    std::shared_ptr<VariableSymbol> idxCounterSymbol = nullptr;
    if (!node.indexName_.empty()) {
      auto idxCounterName = makeSyntheticLoopName("idx");
      idxCounterSymbol = std::make_shared<VariableSymbol>(
          idxCounterName, intType, BindingKind::Mutable, false, idxCounterName,
          moduleName, Visibility::Private);
      currentScope_->declare(idxCounterName, idxCounterSymbol);
      initBlock->statements.push_back(
          std::make_unique<BoundVariableDeclaration>(
              idxCounterSymbol, std::make_unique<BoundLiteral>("0", intType)));
    }

    auto boolType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Bool);
    std::unique_ptr<BoundExpression> condition;

    if (constantStep.has_value()) {
      std::string cmpOp = (*constantStep < 0) ? ">" : "<";
      condition = std::make_unique<BoundBinaryExpression>(
          std::make_unique<BoundVariableExpression>(valCounterSymbol), cmpOp,
          std::make_unique<BoundVariableExpression>(endCounterSymbol),
          boolType);
    } else {
      auto zeroLiteral = std::make_unique<BoundLiteral>("0", rangeType);
      auto stepIsPositive = std::make_unique<BoundBinaryExpression>(
          std::make_unique<BoundVariableExpression>(stepCounterSymbol), ">",
          std::move(zeroLiteral), boolType);
      auto posCondition = std::make_unique<BoundBinaryExpression>(
          std::make_unique<BoundVariableExpression>(valCounterSymbol), "<",
          std::make_unique<BoundVariableExpression>(endCounterSymbol),
          boolType);
      auto negCondition = std::make_unique<BoundBinaryExpression>(
          std::make_unique<BoundVariableExpression>(valCounterSymbol), ">",
          std::make_unique<BoundVariableExpression>(endCounterSymbol),
          boolType);

      condition = std::make_unique<BoundTernaryExpression>(
          std::move(stepIsPositive), std::move(posCondition),
          std::move(negCondition), boolType);
    }

    auto increment = std::make_unique<BoundAssignment>(
        std::make_unique<BoundVariableExpression>(valCounterSymbol),
        std::make_unique<BoundBinaryExpression>(
            std::make_unique<BoundVariableExpression>(valCounterSymbol), "+",
            std::make_unique<BoundVariableExpression>(stepCounterSymbol),
            rangeType));

    pushScope();
    auto itemSymbol = std::make_shared<VariableSymbol>(
        node.itemName_, rangeType, BindingKind::Immutable, false,
        node.itemName_, moduleName, Visibility::Private);
    itemSymbol->syntaxName = node.itemSyntaxName_;
    if (!currentScope_->declare(node.itemSyntaxName_, itemSymbol)) {
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
      indexUserSymbol->syntaxName = node.indexSyntaxName_;
      if (semanticInfo_)
        semanticInfo_->recordName(node.indexSyntaxName_, indexUserSymbol);
      if (!currentScope_->declare(node.indexSyntaxName_, indexUserSymbol)) {
        error(node.span,
              "Variable '" + node.indexName_ + "' already declared.");
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
  itemSymbol->syntaxName = node.itemSyntaxName_;
  if (!currentScope_->declare(node.itemSyntaxName_, itemSymbol)) {
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
    indexUserSymbol->syntaxName = node.indexSyntaxName_;
    if (semanticInfo_)
      semanticInfo_->recordName(node.indexSyntaxName_, indexUserSymbol);
    if (!currentScope_->declare(node.indexSyntaxName_, indexUserSymbol)) {
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

} // namespace sema
