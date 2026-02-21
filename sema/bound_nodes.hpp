#pragma once
#include "../ir/type.hpp"
#include "symbol.hpp"
#include <memory>
#include <string>
#include <vector>

namespace sema {

class BoundRootNode;
class BoundFunctionDeclaration;
class BoundBlock;
class BoundVariableDeclaration;
class BoundReturnStatement;
class BoundAssignment;
class BoundExpression;
class BoundLiteral;
class BoundVariableExpression;
class BoundBinaryExpression;
class BoundUnaryExpression;
class BoundFunctionCall;
class BoundArrayLiteral;

class BoundVisitor {
public:
  virtual ~BoundVisitor() = default;
  virtual void visit(BoundRootNode &node) = 0;
  virtual void visit(BoundFunctionDeclaration &node) = 0;
  virtual void visit(BoundBlock &node) = 0;
  virtual void visit(BoundVariableDeclaration &node) = 0;
  virtual void visit(BoundReturnStatement &node) = 0;
  virtual void visit(BoundAssignment &node) = 0;
  virtual void visit(BoundLiteral &node) = 0;
  virtual void visit(BoundVariableExpression &node) = 0;
  virtual void visit(BoundBinaryExpression &node) = 0;
  virtual void visit(BoundUnaryExpression &node) = 0;
  virtual void visit(BoundFunctionCall &node) = 0;
  virtual void visit(BoundArrayLiteral &node) = 0;
};

class BoundNode {
public:
  virtual ~BoundNode() = default;
  virtual void accept(BoundVisitor &v) = 0;
};

class BoundExpression : public BoundNode {
public:
  std::shared_ptr<zir::Type> type;
  explicit BoundExpression(std::shared_ptr<zir::Type> t) : type(std::move(t)) {}
};

class BoundStatement : public BoundNode {};

class BoundBlock : public BoundStatement {
public:
  std::vector<std::unique_ptr<BoundStatement>> statements;
  void accept(BoundVisitor &v) override { v.visit(*this); }
};

class BoundLiteral : public BoundExpression {
public:
  std::string value;
  BoundLiteral(std::string v, std::shared_ptr<zir::Type> t)
      : BoundExpression(std::move(t)), value(std::move(v)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
};

class BoundVariableExpression : public BoundExpression {
public:
  std::shared_ptr<VariableSymbol> symbol;
  explicit BoundVariableExpression(std::shared_ptr<VariableSymbol> s)
      : BoundExpression(s->type), symbol(std::move(s)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
};

class BoundBinaryExpression : public BoundExpression {
public:
  std::unique_ptr<BoundExpression> left;
  std::string op;
  std::unique_ptr<BoundExpression> right;

  BoundBinaryExpression(std::unique_ptr<BoundExpression> l, std::string o,
                        std::unique_ptr<BoundExpression> r,
                        std::shared_ptr<zir::Type> t)
      : BoundExpression(std::move(t)), left(std::move(l)), op(std::move(o)),
        right(std::move(r)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
};

class BoundUnaryExpression : public BoundExpression {
public:
  std::string op;
  std::unique_ptr<BoundExpression> expr;

  BoundUnaryExpression(std::string o, std::unique_ptr<BoundExpression> e,
                       std::shared_ptr<zir::Type> t)
      : BoundExpression(std::move(t)), op(std::move(o)), expr(std::move(e)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
};

class BoundFunctionCall : public BoundExpression {
public:
  std::shared_ptr<FunctionSymbol> symbol;
  std::vector<std::unique_ptr<BoundExpression>> arguments;

  BoundFunctionCall(std::shared_ptr<FunctionSymbol> s,
                    std::vector<std::unique_ptr<BoundExpression>> args)
      : BoundExpression(s->returnType), symbol(std::move(s)),
        arguments(std::move(args)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
};

class BoundArrayLiteral : public BoundExpression {
public:
  std::vector<std::unique_ptr<BoundExpression>> elements;
  BoundArrayLiteral(std::vector<std::unique_ptr<BoundExpression>> elems,
                    std::shared_ptr<zir::Type> t)
      : BoundExpression(std::move(t)), elements(std::move(elems)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
};

class BoundVariableDeclaration : public BoundStatement {
public:
  std::shared_ptr<VariableSymbol> symbol;
  std::unique_ptr<BoundExpression> initializer;

  BoundVariableDeclaration(std::shared_ptr<VariableSymbol> s,
                           std::unique_ptr<BoundExpression> init)
      : symbol(std::move(s)), initializer(std::move(init)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
};

class BoundReturnStatement : public BoundStatement {
public:
  std::unique_ptr<BoundExpression> expression;
  explicit BoundReturnStatement(std::unique_ptr<BoundExpression> e)
      : expression(std::move(e)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
};

class BoundAssignment : public BoundStatement {
public:
  std::shared_ptr<VariableSymbol> symbol;
  std::unique_ptr<BoundExpression> expression;

  BoundAssignment(std::shared_ptr<VariableSymbol> s,
                  std::unique_ptr<BoundExpression> e)
      : symbol(std::move(s)), expression(std::move(e)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
};

class BoundFunctionDeclaration : public BoundNode {
public:
  std::shared_ptr<FunctionSymbol> symbol;
  std::unique_ptr<BoundBlock> body;

  BoundFunctionDeclaration(std::shared_ptr<FunctionSymbol> s,
                           std::unique_ptr<BoundBlock> b)
      : symbol(std::move(s)), body(std::move(b)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
};

class BoundRootNode : public BoundNode {
public:
  std::vector<std::unique_ptr<BoundFunctionDeclaration>> functions;
  void accept(BoundVisitor &v) override { v.visit(*this); }
};

} // namespace sema
