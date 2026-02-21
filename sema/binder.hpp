#pragma once
#include "../ast/nodes.hpp"
#include "../ast/visitor.hpp"
#include "bound_nodes.hpp"
#include "symbol_table.hpp"
#include <memory>
#include <stack>

namespace sema {

class Binder : public Visitor {
public:
  std::unique_ptr<BoundRootNode> bind(RootNode& root);

  void visit(RootNode& node) override;
  void visit(FunDecl& node) override;
  void visit(BodyNode& node) override;
  void visit(VarDecl& node) override;
  void visit(ReturnNode& node) override;
  void visit(BinExpr& node) override;
  void visit(ConstInt& node) override;
  void visit(IfNode& node) override;
  void visit(WhileNode& node) override;
  void visit(AssignNode& node) override;
  void visit(FunCall& node) override;
  void visit(ConstId& node) override;
  void visit(ConstFloat& node) override;
  void visit(ConstString& node) override;

private:
  std::shared_ptr<SymbolTable> currentScope_;
  std::unique_ptr<BoundRootNode> boundRoot_;
  
  // Stacks to store bound nodes during traversal
  std::stack<std::unique_ptr<BoundExpression>> expressionStack_;
  std::stack<std::unique_ptr<BoundStatement>> statementStack_;
  std::unique_ptr<BoundBlock> currentBlock_;

  void pushScope();
  void popScope();
  
  std::shared_ptr<zir::Type> mapType(const std::string& typeName);
  void error(const std::string& message);

  bool hadError_ = false;
};

} // namespace sema
