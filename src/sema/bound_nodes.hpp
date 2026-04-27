#pragma once
#include "../ir/type.hpp"
#include "symbol.hpp"
#include <memory>
#include <string>
#include <vector>

namespace sema {

class BoundRootNode;
class BoundFunctionDeclaration;
class BoundExternalFunctionDeclaration;
class BoundBlock;
class BoundVariableDeclaration;
class BoundReturnStatement;
class BoundAssignment;
class BoundExpressionStatement;
class BoundExpression;
class BoundLiteral;
class BoundVariableExpression;
class BoundBinaryExpression;
class BoundTernaryExpression;
class BoundUnaryExpression;
class BoundFunctionCall;
class BoundArrayLiteral;
class BoundIndexAccess;
class BoundRecordDeclaration;
class BoundEnumDeclaration;
class BoundMemberAccess;
class BoundStructLiteral;
class BoundModuleReference;
class BoundIfStatement;
class BoundWhileStatement;
class BoundForStatement;
class BoundBreakStatement;
class BoundContinueStatement;
class BoundCast;
class BoundNewExpression;
class BoundWeakLockExpression;
class BoundWeakAliveExpression;
class BoundTryExpression;
class BoundFallbackExpression;
class BoundFailableHandleExpression;
class BoundFailStatement;

class BoundVisitor {
public:
  virtual ~BoundVisitor() = default;
  virtual void visit(BoundRootNode &node) = 0;
  virtual void visit(BoundFunctionDeclaration &node) = 0;
  virtual void visit(BoundExternalFunctionDeclaration &node) = 0;
  virtual void visit(BoundBlock &node) = 0;
  virtual void visit(BoundVariableDeclaration &node) = 0;
  virtual void visit(BoundReturnStatement &node) = 0;
  virtual void visit(BoundAssignment &node) = 0;
  virtual void visit(BoundExpressionStatement &node) = 0;
  virtual void visit(BoundLiteral &node) = 0;
  virtual void visit(BoundVariableExpression &node) = 0;
  virtual void visit(BoundBinaryExpression &node) = 0;
  virtual void visit(BoundTernaryExpression &node) = 0;
  virtual void visit(BoundUnaryExpression &node) = 0;
  virtual void visit(BoundFunctionCall &node) = 0;
  virtual void visit(BoundArrayLiteral &node) = 0;
  virtual void visit(BoundIndexAccess &node) = 0;
  virtual void visit(BoundRecordDeclaration &node) = 0;
  virtual void visit(BoundEnumDeclaration &node) = 0;
  virtual void visit(BoundMemberAccess &node) = 0;
  virtual void visit(BoundStructLiteral &node) = 0;
  virtual void visit(BoundModuleReference &node) = 0;
  virtual void visit(BoundIfStatement &node) = 0;
  virtual void visit(BoundWhileStatement &node) = 0;
  virtual void visit(BoundForStatement &node) = 0;
  virtual void visit(BoundBreakStatement &node) = 0;
  virtual void visit(BoundContinueStatement &node) = 0;
  virtual void visit(BoundCast &node) = 0;
  virtual void visit(BoundNewExpression &node) = 0;
  virtual void visit(BoundWeakLockExpression &node) = 0;
  virtual void visit(BoundWeakAliveExpression &node) = 0;
  virtual void visit(BoundTryExpression &node) = 0;
  virtual void visit(BoundFallbackExpression &node) = 0;
  virtual void visit(BoundFailableHandleExpression &node) = 0;
  virtual void visit(BoundFailStatement &node) = 0;
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
  virtual std::unique_ptr<BoundExpression> clone() const = 0;
};

class BoundStatement : public BoundNode {
public:
  virtual std::unique_ptr<BoundStatement> cloneStatement() const = 0;
};

class BoundExpressionStatement : public BoundStatement {
public:
  std::unique_ptr<BoundExpression> expression;

  explicit BoundExpressionStatement(std::unique_ptr<BoundExpression> expr)
      : expression(std::move(expr)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
  std::unique_ptr<BoundStatement> cloneStatement() const override {
    return std::make_unique<BoundExpressionStatement>(expression->clone());
  }
};

class BoundBlock : public BoundStatement {
public:
  std::vector<std::unique_ptr<BoundStatement>> statements;
  std::unique_ptr<BoundExpression> result;
  void accept(BoundVisitor &v) override { v.visit(*this); }
  std::unique_ptr<BoundStatement> cloneStatement() const override {
    auto cloned = std::make_unique<BoundBlock>();
    for (const auto &stmt : statements)
      cloned->statements.push_back(stmt->cloneStatement());
    if (result)
      cloned->result = result->clone();
    return cloned;
  }
  std::unique_ptr<BoundBlock> cloneBlock() const {
    auto res = cloneStatement();
    return std::unique_ptr<BoundBlock>(
        static_cast<BoundBlock *>(res.release()));
  }
};

class BoundLiteral : public BoundExpression {
public:
  std::string value;
  BoundLiteral(std::string v, std::shared_ptr<zir::Type> t)
      : BoundExpression(std::move(t)), value(std::move(v)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
  std::unique_ptr<BoundExpression> clone() const override {
    return std::make_unique<BoundLiteral>(value, type);
  }
};

class BoundCast : public BoundExpression {
public:
  std::unique_ptr<BoundExpression> expression;
  BoundCast(std::unique_ptr<BoundExpression> e, std::shared_ptr<zir::Type> t)
      : BoundExpression(std::move(t)), expression(std::move(e)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
  std::unique_ptr<BoundExpression> clone() const override {
    return std::make_unique<BoundCast>(expression->clone(), type);
  }
};

class BoundVariableExpression : public BoundExpression {
public:
  std::shared_ptr<VariableSymbol> symbol;
  explicit BoundVariableExpression(std::shared_ptr<VariableSymbol> s)
      : BoundExpression(s->type), symbol(std::move(s)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
  std::unique_ptr<BoundExpression> clone() const override {
    return std::make_unique<BoundVariableExpression>(symbol);
  }
};

class BoundModuleReference : public BoundExpression {
public:
  std::shared_ptr<ModuleSymbol> symbol;

  explicit BoundModuleReference(std::shared_ptr<ModuleSymbol> s)
      : BoundExpression(
            std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void)),
        symbol(std::move(s)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
  std::unique_ptr<BoundExpression> clone() const override {
    return std::make_unique<BoundModuleReference>(symbol);
  }
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
  std::unique_ptr<BoundExpression> clone() const override {
    return std::make_unique<BoundBinaryExpression>(left->clone(), op,
                                                   right->clone(), type);
  }
};

class BoundTernaryExpression : public BoundExpression {
public:
  std::unique_ptr<BoundExpression> condition;
  std::unique_ptr<BoundExpression> thenExpr;
  std::unique_ptr<BoundExpression> elseExpr;

  BoundTernaryExpression(std::unique_ptr<BoundExpression> cond,
                         std::unique_ptr<BoundExpression> thenE,
                         std::unique_ptr<BoundExpression> elseE,
                         std::shared_ptr<zir::Type> t)
      : BoundExpression(std::move(t)), condition(std::move(cond)),
        thenExpr(std::move(thenE)), elseExpr(std::move(elseE)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
  std::unique_ptr<BoundExpression> clone() const override {
    return std::make_unique<BoundTernaryExpression>(
        condition->clone(), thenExpr->clone(), elseExpr->clone(), type);
  }
};

class BoundUnaryExpression : public BoundExpression {
public:
  std::string op;
  std::unique_ptr<BoundExpression> expr;

  BoundUnaryExpression(std::string o, std::unique_ptr<BoundExpression> e,
                       std::shared_ptr<zir::Type> t)
      : BoundExpression(std::move(t)), op(std::move(o)), expr(std::move(e)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
  std::unique_ptr<BoundExpression> clone() const override {
    return std::make_unique<BoundUnaryExpression>(op, expr->clone(), type);
  }
};

class BoundFunctionCall : public BoundExpression {
public:
  std::shared_ptr<FunctionSymbol> symbol;
  std::vector<std::unique_ptr<BoundExpression>> arguments;
  std::vector<bool> argumentIsRef;
  std::unique_ptr<BoundExpression> variadicPack;

  BoundFunctionCall(std::shared_ptr<FunctionSymbol> s,
                    std::vector<std::unique_ptr<BoundExpression>> args,
                    std::vector<bool> argIsRef = {},
                    std::unique_ptr<BoundExpression> pack = nullptr)
      : BoundExpression(s->returnType), symbol(std::move(s)),
        arguments(std::move(args)), argumentIsRef(std::move(argIsRef)),
        variadicPack(std::move(pack)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
  std::unique_ptr<BoundExpression> clone() const override {
    std::vector<std::unique_ptr<BoundExpression>> clonedArgs;
    for (const auto &arg : arguments)
      clonedArgs.push_back(arg->clone());
    return std::make_unique<BoundFunctionCall>(
        symbol, std::move(clonedArgs), argumentIsRef,
        variadicPack ? variadicPack->clone() : nullptr);
  }
};

class BoundArrayLiteral : public BoundExpression {
public:
  std::vector<std::unique_ptr<BoundExpression>> elements;
  BoundArrayLiteral(std::vector<std::unique_ptr<BoundExpression>> elems,
                    std::shared_ptr<zir::Type> t)
      : BoundExpression(std::move(t)), elements(std::move(elems)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
  std::unique_ptr<BoundExpression> clone() const override {
    std::vector<std::unique_ptr<BoundExpression>> clonedElems;
    for (const auto &elem : elements)
      clonedElems.push_back(elem->clone());
    return std::make_unique<BoundArrayLiteral>(std::move(clonedElems), type);
  }
};

class BoundNewExpression : public BoundExpression {
public:
  std::shared_ptr<zir::ClassType> classType;
  std::shared_ptr<FunctionSymbol> constructor;
  std::vector<std::unique_ptr<BoundExpression>> arguments;
  std::vector<bool> argumentIsRef;

  BoundNewExpression(std::shared_ptr<zir::ClassType> type,
                     std::shared_ptr<FunctionSymbol> ctor,
                     std::vector<std::unique_ptr<BoundExpression>> args,
                     std::vector<bool> argRefs = {})
      : BoundExpression(type), classType(std::move(type)),
        constructor(std::move(ctor)), arguments(std::move(args)),
        argumentIsRef(std::move(argRefs)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
  std::unique_ptr<BoundExpression> clone() const override {
    std::vector<std::unique_ptr<BoundExpression>> clonedArgs;
    for (const auto &arg : arguments)
      clonedArgs.push_back(arg->clone());
    return std::make_unique<BoundNewExpression>(
        classType, constructor, std::move(clonedArgs), argumentIsRef);
  }
};

class BoundWeakLockExpression : public BoundExpression {
public:
  std::unique_ptr<BoundExpression> weakExpression;

  BoundWeakLockExpression(std::unique_ptr<BoundExpression> expr,
                          std::shared_ptr<zir::Type> t)
      : BoundExpression(std::move(t)), weakExpression(std::move(expr)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
  std::unique_ptr<BoundExpression> clone() const override {
    return std::make_unique<BoundWeakLockExpression>(weakExpression->clone(),
                                                     type);
  }
};

class BoundWeakAliveExpression : public BoundExpression {
public:
  std::unique_ptr<BoundExpression> weakExpression;

  explicit BoundWeakAliveExpression(std::unique_ptr<BoundExpression> expr)
      : BoundExpression(
            std::make_shared<zir::PrimitiveType>(zir::TypeKind::Bool)),
        weakExpression(std::move(expr)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
  std::unique_ptr<BoundExpression> clone() const override {
    return std::make_unique<BoundWeakAliveExpression>(weakExpression->clone());
  }
};

class BoundIndexAccess : public BoundExpression {
public:
  std::unique_ptr<BoundExpression> left;
  std::unique_ptr<BoundExpression> index;

  BoundIndexAccess(std::unique_ptr<BoundExpression> l,
                   std::unique_ptr<BoundExpression> i,
                   std::shared_ptr<zir::Type> t)
      : BoundExpression(std::move(t)), left(std::move(l)), index(std::move(i)) {
  }
  void accept(BoundVisitor &v) override { v.visit(*this); }
  std::unique_ptr<BoundExpression> clone() const override {
    return std::make_unique<BoundIndexAccess>(left->clone(), index->clone(),
                                              type);
  }
};

class BoundTryExpression : public BoundExpression {
public:
  std::unique_ptr<BoundExpression> expression;
  std::shared_ptr<zir::Type> propagatedType;
  std::shared_ptr<zir::Type> errorType;

  BoundTryExpression(std::unique_ptr<BoundExpression> e,
                     std::shared_ptr<zir::Type> resultType,
                     std::shared_ptr<zir::Type> propagated,
                     std::shared_ptr<zir::Type> errType)
      : BoundExpression(std::move(resultType)), expression(std::move(e)),
        propagatedType(std::move(propagated)), errorType(std::move(errType)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
  std::unique_ptr<BoundExpression> clone() const override {
    return std::make_unique<BoundTryExpression>(expression->clone(), type,
                                                propagatedType, errorType);
  }
};

class BoundFallbackExpression : public BoundExpression {
public:
  std::unique_ptr<BoundExpression> expression;
  std::unique_ptr<BoundExpression> fallback;
  std::shared_ptr<zir::Type> errorType;

  BoundFallbackExpression(std::unique_ptr<BoundExpression> e,
                          std::unique_ptr<BoundExpression> fb,
                          std::shared_ptr<zir::Type> resultType,
                          std::shared_ptr<zir::Type> errType)
      : BoundExpression(std::move(resultType)), expression(std::move(e)),
        fallback(std::move(fb)), errorType(std::move(errType)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
  std::unique_ptr<BoundExpression> clone() const override {
    return std::make_unique<BoundFallbackExpression>(
        expression->clone(), fallback->clone(), type, errorType);
  }
};

class BoundFailableHandleExpression : public BoundExpression {
public:
  std::unique_ptr<BoundExpression> expression;
  std::shared_ptr<VariableSymbol> errorSymbol;
  std::unique_ptr<BoundBlock> handler;
  std::shared_ptr<zir::Type> errorType;

  BoundFailableHandleExpression(std::unique_ptr<BoundExpression> e,
                                std::shared_ptr<VariableSymbol> errSym,
                                std::unique_ptr<BoundBlock> h,
                                std::shared_ptr<zir::Type> resultType,
                                std::shared_ptr<zir::Type> errType)
      : BoundExpression(std::move(resultType)), expression(std::move(e)),
        errorSymbol(std::move(errSym)), handler(std::move(h)),
        errorType(std::move(errType)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
  std::unique_ptr<BoundExpression> clone() const override {
    return std::make_unique<BoundFailableHandleExpression>(
        expression->clone(), errorSymbol, handler ? handler->cloneBlock() : nullptr,
        type, errorType);
  }
};

class BoundVariableDeclaration : public BoundStatement {
public:
  std::shared_ptr<VariableSymbol> symbol;
  std::unique_ptr<BoundExpression> initializer;

  BoundVariableDeclaration(std::shared_ptr<VariableSymbol> s,
                           std::unique_ptr<BoundExpression> init)
      : symbol(std::move(s)), initializer(std::move(init)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
  std::unique_ptr<BoundStatement> cloneStatement() const override {
    return std::make_unique<BoundVariableDeclaration>(
        symbol, initializer ? initializer->clone() : nullptr);
  }
};

class BoundReturnStatement : public BoundStatement {
public:
  std::unique_ptr<BoundExpression> expression;
  explicit BoundReturnStatement(std::unique_ptr<BoundExpression> e)
      : expression(std::move(e)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
  std::unique_ptr<BoundStatement> cloneStatement() const override {
    return std::make_unique<BoundReturnStatement>(
        expression ? expression->clone() : nullptr);
  }
};

class BoundFailStatement : public BoundStatement {
public:
  std::unique_ptr<BoundExpression> errorExpression;
  std::shared_ptr<zir::Type> propagatedType;
  std::shared_ptr<zir::Type> errorType;

  BoundFailStatement(std::unique_ptr<BoundExpression> errExpr,
                     std::shared_ptr<zir::Type> propagated,
                     std::shared_ptr<zir::Type> errType)
      : errorExpression(std::move(errExpr)),
        propagatedType(std::move(propagated)), errorType(std::move(errType)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
  std::unique_ptr<BoundStatement> cloneStatement() const override {
    return std::make_unique<BoundFailStatement>(
        errorExpression ? errorExpression->clone() : nullptr, propagatedType,
        errorType);
  }
};

class BoundAssignment : public BoundStatement {
public:
  std::unique_ptr<BoundExpression> target;
  std::unique_ptr<BoundExpression> expression;

  BoundAssignment(std::unique_ptr<BoundExpression> t,
                  std::unique_ptr<BoundExpression> e)
      : target(std::move(t)), expression(std::move(e)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
  std::unique_ptr<BoundStatement> cloneStatement() const override {
    return std::make_unique<BoundAssignment>(target->clone(),
                                             expression->clone());
  }
};

class BoundIfStatement : public BoundStatement {
public:
  std::unique_ptr<BoundExpression> condition;
  std::unique_ptr<BoundBlock> thenBody;
  std::unique_ptr<BoundBlock> elseBody;

  BoundIfStatement(std::unique_ptr<BoundExpression> cond,
                   std::unique_ptr<BoundBlock> thenB,
                   std::unique_ptr<BoundBlock> elseB)
      : condition(std::move(cond)), thenBody(std::move(thenB)),
        elseBody(std::move(elseB)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
  std::unique_ptr<BoundStatement> cloneStatement() const override {
    return std::make_unique<BoundIfStatement>(
        condition->clone(), thenBody->cloneBlock(),
        elseBody ? elseBody->cloneBlock() : nullptr);
  }
};

class BoundWhileStatement : public BoundStatement {
public:
  std::unique_ptr<BoundExpression> condition;
  std::unique_ptr<BoundBlock> body;

  BoundWhileStatement(std::unique_ptr<BoundExpression> cond,
                      std::unique_ptr<BoundBlock> b)
      : condition(std::move(cond)), body(std::move(b)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
  std::unique_ptr<BoundStatement> cloneStatement() const override {
    return std::make_unique<BoundWhileStatement>(condition->clone(),
                                                 body->cloneBlock());
  }
};

class BoundForStatement : public BoundStatement {
public:
  std::unique_ptr<BoundStatement> initializer;
  std::unique_ptr<BoundExpression> condition;
  std::unique_ptr<BoundStatement> increment;
  std::unique_ptr<BoundBlock> body;

  BoundForStatement(std::unique_ptr<BoundStatement> init,
                    std::unique_ptr<BoundExpression> cond,
                    std::unique_ptr<BoundStatement> inc,
                    std::unique_ptr<BoundBlock> loopBody)
      : initializer(std::move(init)), condition(std::move(cond)),
        increment(std::move(inc)), body(std::move(loopBody)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
  std::unique_ptr<BoundStatement> cloneStatement() const override {
    return std::make_unique<BoundForStatement>(
        initializer ? initializer->cloneStatement() : nullptr,
        condition ? condition->clone() : nullptr,
        increment ? increment->cloneStatement() : nullptr,
        body ? body->cloneBlock() : nullptr);
  }
};

class BoundBreakStatement : public BoundStatement {
public:
  BoundBreakStatement() = default;
  void accept(BoundVisitor &v) override { v.visit(*this); }
  std::unique_ptr<BoundStatement> cloneStatement() const override {
    return std::make_unique<BoundBreakStatement>();
  }
};

class BoundContinueStatement : public BoundStatement {
public:
  BoundContinueStatement() = default;
  void accept(BoundVisitor &v) override { v.visit(*this); }
  std::unique_ptr<BoundStatement> cloneStatement() const override {
    return std::make_unique<BoundContinueStatement>();
  }
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

class BoundExternalFunctionDeclaration : public BoundNode {
public:
  std::shared_ptr<FunctionSymbol> symbol;

  explicit BoundExternalFunctionDeclaration(std::shared_ptr<FunctionSymbol> s)
      : symbol(std::move(s)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
};

class BoundRecordDeclaration : public BoundNode {
public:
  std::shared_ptr<zir::RecordType> type;
  void accept(BoundVisitor &v) override { v.visit(*this); }
};

class BoundEnumDeclaration : public BoundNode {
public:
  std::shared_ptr<zir::EnumType> type;
  void accept(BoundVisitor &v) override { v.visit(*this); }
};

class BoundMemberAccess : public BoundExpression {
public:
  std::unique_ptr<BoundExpression> left;
  std::string member;

  BoundMemberAccess(std::unique_ptr<BoundExpression> l, std::string m,
                    std::shared_ptr<zir::Type> t)
      : BoundExpression(std::move(t)), left(std::move(l)),
        member(std::move(m)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
  std::unique_ptr<BoundExpression> clone() const override {
    return std::make_unique<BoundMemberAccess>(left->clone(), member, type);
  }
};

class BoundStructLiteral : public BoundExpression {
public:
  std::vector<std::pair<std::string, std::unique_ptr<BoundExpression>>> fields;

  BoundStructLiteral(
      std::vector<std::pair<std::string, std::unique_ptr<BoundExpression>>> f,
      std::shared_ptr<zir::Type> t)
      : BoundExpression(std::move(t)), fields(std::move(f)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
  std::unique_ptr<BoundExpression> clone() const override {
    std::vector<std::pair<std::string, std::unique_ptr<BoundExpression>>>
        clonedFields;
    for (const auto &field : fields) {
      clonedFields.push_back({field.first, field.second->clone()});
    }
    return std::make_unique<BoundStructLiteral>(std::move(clonedFields), type);
  }
};

class BoundRootNode : public BoundNode {
public:
  std::vector<std::unique_ptr<BoundRecordDeclaration>> records;
  std::vector<std::unique_ptr<BoundEnumDeclaration>> enums;
  std::vector<std::unique_ptr<BoundVariableDeclaration>> globals;
  std::vector<std::unique_ptr<BoundFunctionDeclaration>> functions;
  std::vector<std::unique_ptr<BoundExternalFunctionDeclaration>>
      externalFunctions;
  void accept(BoundVisitor &v) override { v.visit(*this); }
};

} // namespace sema
