#include "function_reachability.hpp"

#include "../sema/bound_nodes.hpp"

#include <deque>
#include <unordered_map>

namespace zir {
namespace {

class ReachabilityVisitor final : public sema::BoundVisitor {
public:
  explicit ReachabilityVisitor(FunctionReachability &result)
      : result_(result) {}

  void visit(sema::BoundRootNode &) override {}
  void visit(sema::BoundFunctionDeclaration &node) override {
    visitNode(node.body);
  }
  void visit(sema::BoundExternalFunctionDeclaration &) override {}
  void visit(sema::BoundBlock &node) override {
    for (auto &statement : node.statements)
      visitNode(statement);
    visitNode(node.result);
  }
  void visit(sema::BoundVariableDeclaration &node) override {
    visitNode(node.initializer);
  }
  void visit(sema::BoundReturnStatement &node) override {
    visitNode(node.expression);
  }
  void visit(sema::BoundAssignment &node) override {
    visitNode(node.target);
    visitNode(node.expression);
  }
  void visit(sema::BoundExpressionStatement &node) override {
    visitNode(node.expression);
  }
  void visit(sema::BoundLiteral &) override {}
  void visit(sema::BoundVariableExpression &) override {}
  void visit(sema::BoundCompoundTargetLoad &) override {}
  void visit(sema::BoundBinaryExpression &node) override {
    visitNode(node.left);
    visitNode(node.right);
  }
  void visit(sema::BoundTernaryExpression &node) override {
    visitNode(node.condition);
    visitNode(node.thenExpr);
    visitNode(node.elseExpr);
  }
  void visit(sema::BoundUnaryExpression &node) override {
    visitNode(node.expr);
  }
  void visit(sema::BoundFunctionCall &node) override {
    markFunction(node.symbol);
    visitExpressions(node.arguments);
    visitNode(node.variadicPack);
  }
  void visit(sema::BoundIndirectCall &node) override {
    visitNode(node.callee);
    visitExpressions(node.arguments);
  }
  void visit(sema::BoundFunctionReference &node) override {
    markFunction(node.symbol);
  }
  void visit(sema::BoundArrayLiteral &node) override {
    visitExpressions(node.elements);
  }
  void visit(sema::BoundIndexAccess &node) override {
    visitNode(node.left);
    visitNode(node.index);
  }
  void visit(sema::BoundRecordDeclaration &) override {}
  void visit(sema::BoundEnumDeclaration &) override {}
  void visit(sema::BoundTaggedUnionDeclaration &) override {}
  void visit(sema::BoundMemberAccess &node) override { visitNode(node.left); }
  void visit(sema::BoundStructLiteral &node) override {
    for (auto &field : node.fields)
      visitNode(field.second);
  }
  void visit(sema::BoundTaggedUnionLiteral &node) override {
    visitNode(node.payload);
  }
  void visit(sema::BoundModuleReference &) override {}
  void visit(sema::BoundIfStatement &node) override {
    visitNode(node.condition);
    visitNode(node.thenBody);
    visitNode(node.elseBody);
  }
  void visit(sema::BoundWhileStatement &node) override {
    visitNode(node.condition);
    visitNode(node.body);
  }
  void visit(sema::BoundForStatement &node) override {
    visitNode(node.initializer);
    visitNode(node.condition);
    visitNode(node.increment);
    visitNode(node.body);
  }
  void visit(sema::BoundBreakStatement &) override {}
  void visit(sema::BoundContinueStatement &) override {}
  void visit(sema::BoundAsmStatement &node) override {
    for (auto &operand : node.outputs)
      visitNode(operand.expr);
    for (auto &operand : node.inputs)
      visitNode(operand.expr);
  }
  void visit(sema::BoundCast &node) override { visitNode(node.expression); }
  void visit(sema::BoundNewExpression &node) override {
    if (node.classType)
      result_.liveClassCodegenNames.insert(node.classType->getCodegenName());
    markFunction(node.constructor);
    visitExpressions(node.arguments);
  }
  void visit(sema::BoundWeakLockExpression &node) override {
    visitNode(node.weakExpression);
  }
  void visit(sema::BoundWeakAliveExpression &node) override {
    visitNode(node.weakExpression);
  }
  void visit(sema::BoundTryExpression &node) override {
    visitNode(node.expression);
  }
  void visit(sema::BoundFallbackExpression &node) override {
    visitNode(node.expression);
    visitNode(node.fallback);
  }
  void visit(sema::BoundFailableHandleExpression &node) override {
    visitNode(node.expression);
    visitNode(node.handler);
  }
  void visit(sema::BoundFailStatement &node) override {
    visitNode(node.errorExpression);
  }

  void setPending(std::deque<const sema::FunctionSymbol *> *pending) {
    pending_ = pending;
  }

private:
  template <typename T> void visitNode(const std::unique_ptr<T> &node) {
    if (node)
      node->accept(*this);
  }

  void visitExpressions(
      const std::vector<std::unique_ptr<sema::BoundExpression>> &expressions) {
    for (const auto &expression : expressions)
      visitNode(expression);
  }

  void markFunction(const std::shared_ptr<sema::FunctionSymbol> &symbol) {
    if (!symbol)
      return;
    if (!symbol->ownerTypeCodegenName.empty()) {
      result_.liveClassCodegenNames.insert(symbol->ownerTypeCodegenName);
      if (symbol->vtableSlot >= 0)
        result_.liveVtableSlots[symbol->ownerTypeCodegenName].insert(
            symbol->vtableSlot);
    }
    if (symbol->isExternal) {
      result_.externalFunctions.insert(symbol.get());
    } else if (result_.functions.insert(symbol.get()).second && pending_) {
      pending_->push_back(symbol.get());
    }
  }

  FunctionReachability &result_;
  std::deque<const sema::FunctionSymbol *> *pending_ = nullptr;
};

} // namespace

FunctionReachability
FunctionReachabilityAnalyzer::analyze(sema::BoundRootNode &root) {
  FunctionReachability result;
  std::unordered_map<const sema::FunctionSymbol *,
                     sema::BoundFunctionDeclaration *>
      declarations;
  for (const auto &function : root.functions) {
    if (function && function->symbol)
      declarations.emplace(function->symbol.get(), function.get());
  }

  std::deque<const sema::FunctionSymbol *> pending;
  ReachabilityVisitor visitor(result);
  visitor.setPending(&pending);

  for (const auto &global : root.globals)
    visitor.visit(*global);

  for (const auto &function : root.functions) {
    const auto &symbol = function->symbol;
    if (symbol && (symbol->isEntryModule || symbol->name == "main" ||
                   symbol->hasEntry || symbol->hasNoMangle)) {
      if (result.functions.insert(symbol.get()).second)
        pending.push_back(symbol.get());
    }
  }

  while (!pending.empty()) {
    const auto *symbol = pending.front();
    pending.pop_front();
    const auto declaration = declarations.find(symbol);
    if (declaration != declarations.end())
      visitor.visit(*declaration->second);
  }

  return result;
}

} // namespace zir
