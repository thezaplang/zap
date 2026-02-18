#pragma once
// Forward declarations of AST node types for visitor interface (global namespace)
class Node;
class RootNode;
class TopLevel;
class FunDecl;
class BodyNode;
class StatementNode;
class VarDecl;
class ReturnNode;
class ExpressionNode;
class BinExpr;
class ConstInt;

// Simple visitor interface with empty implementations to allow selective overrides.
struct Visitor {
  virtual ~Visitor() = default;
  virtual void visit(Node &) {}
  virtual void visit(RootNode &) {}
  virtual void visit(FunDecl &) {}
  virtual void visit(BodyNode &) {}
  virtual void visit(StatementNode &) {}
  virtual void visit(VarDecl &) {}
  virtual void visit(ReturnNode &) {}
  virtual void visit(ExpressionNode &) {}
  virtual void visit(BinExpr &) {}
  virtual void visit(ConstInt &) {}
};
