#pragma once
#include "../ast/nodes.hpp"
#include "../ast/visitor.hpp"
#include "../utils/diagnostics.hpp"
#include "bound_nodes.hpp"
#include "symbol_table.hpp"
#include <memory>
#include <stack>

namespace sema {

class Binder : public Visitor {
public:
  Binder(zap::DiagnosticEngine &diag);
  std::unique_ptr<BoundRootNode> bind(RootNode &root);

  void visit(RootNode &node) override;
  void visit(FunDecl &node) override;
  void visit(BodyNode &node) override;
  void visit(VarDecl &node) override;
  void visit(ReturnNode &node) override;
  void visit(BinExpr &node) override;
  void visit(ConstInt &node) override;
  void visit(ConstBool &node) override;
  void visit(IfNode &node) override;
  void visit(WhileNode &node) override;
  void visit(AssignNode &node) override;
  void visit(FunCall &node) override;
  void visit(ConstId &node) override;
  void visit(ConstFloat &node) override;
  void visit(ConstString &node) override;
  void visit(UnaryExpr &node) override;
  void visit(ArrayLiteralNode &node) override;

private:
  zap::DiagnosticEngine &_diag;
  std::shared_ptr<SymbolTable> currentScope_;
  std::unique_ptr<BoundRootNode> boundRoot_;

  std::stack<std::unique_ptr<BoundExpression>> expressionStack_;
  std::stack<std::unique_ptr<BoundStatement>> statementStack_;
  std::unique_ptr<BoundBlock> currentBlock_;

  void pushScope();
  void popScope();

  std::shared_ptr<FunctionSymbol> currentFunction_ = nullptr;

  std::shared_ptr<zir::Type> mapType(const TypeNode &typeNode);
  void error(SourceSpan span, const std::string &message);

  bool isNumeric(std::shared_ptr<zir::Type> type);
  bool canConvert(std::shared_ptr<zir::Type> from,
                  std::shared_ptr<zir::Type> to);
  std::shared_ptr<zir::Type> getPromotedType(std::shared_ptr<zir::Type> t1,
                                             std::shared_ptr<zir::Type> t2);

  bool hadError_ = false;
};

} // namespace sema
