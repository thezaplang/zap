#pragma once

// Forward declarations of AST node types for visitor interface
class Node;
class RootNode;
class TopLevel;
class FunDecl;
class ExtDecl;
class BodyNode;
class StatementNode;
class VarDecl;
class ConstDecl;
class ReturnNode;
class IfNode;
class WhileNode;
class BreakNode;
class ContinueNode;
class MemberAccessNode;
class EnumDecl;
class RecordDecl;
class ImportNode;
class ParameterNode;
class TypeNode;

class ExpressionNode;
class BinExpr;
class UnaryExpr;
class FunCall;
class ArrayLiteralNode;
class AssignNode;

class ConstInt;
class ConstFloat;
class ConstString;
class ConstChar;
class ConstBool;
class ConstId;

///
/// @brief Visitor interface with empty implementations to allow selective overrides.
///
struct Visitor
{
  virtual ~Visitor() noexcept = default;

  virtual void visit(Node &) {}
  virtual void visit(RootNode &) {}
  virtual void visit(TopLevel &) {}
  virtual void visit(FunDecl &) {}
  virtual void visit(ExtDecl &) {}
  virtual void visit(BodyNode &) {}
  virtual void visit(StatementNode &) {}
  virtual void visit(VarDecl &) {}
  virtual void visit(ConstDecl &) {}
  virtual void visit(ReturnNode &) {}
  virtual void visit(IfNode &) {}
  virtual void visit(WhileNode &) {}
  virtual void visit(MemberAccessNode &) {}
  virtual void visit(BreakNode &) {}
  virtual void visit(ContinueNode &) {}
  virtual void visit(EnumDecl &) {}
  virtual void visit(RecordDecl &) {}
  virtual void visit(ImportNode &) {}
  virtual void visit(ParameterNode &) {}
  virtual void visit(TypeNode &) {}

  virtual void visit(ExpressionNode &) {}
  virtual void visit(BinExpr &) {}
  virtual void visit(UnaryExpr &) {}
  virtual void visit(FunCall &) {}
  virtual void visit(ArrayLiteralNode &) {}
  virtual void visit(AssignNode &) {}

  virtual void visit(ConstInt &) {}
  virtual void visit(ConstFloat &) {}
  virtual void visit(ConstString &) {}
  virtual void visit(ConstChar &) {}
  virtual void visit(ConstBool &) {}
  virtual void visit(ConstId &) {}
};
