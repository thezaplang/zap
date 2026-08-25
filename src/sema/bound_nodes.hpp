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
class BoundClassTypeTest;
class BoundCompoundTargetLoad;
class BoundBinaryExpression;
class BoundTernaryExpression;
class BoundUnaryExpression;
class BoundFunctionCall;
class BoundArrayLiteral;
class BoundIndexAccess;
class BoundRecordDeclaration;
class BoundEnumDeclaration;
class BoundTaggedUnionDeclaration;
class BoundMemberAccess;
class BoundStructLiteral;
class BoundTaggedUnionLiteral;
class BoundModuleReference;
class BoundIfStatement;
class BoundCaseStatement;
class BoundWhileStatement;
class BoundForStatement;
class BoundBreakStatement;
class BoundContinueStatement;
class BoundAsmStatement;
class BoundCast;
class BoundNewExpression;
class BoundWeakLockExpression;
class BoundWeakAliveExpression;
class BoundRangeExpression;
class BoundTryExpression;
class BoundFallbackExpression;
class BoundFailableHandleExpression;
class BoundFailStatement;
class BoundIndirectCall;
class BoundFunctionReference;

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
  virtual void visit(BoundClassTypeTest &node) = 0;
  virtual void visit(BoundCompoundTargetLoad &node) = 0;
  virtual void visit(BoundBinaryExpression &node) = 0;
  virtual void visit(BoundTernaryExpression &node) = 0;
  virtual void visit(BoundUnaryExpression &node) = 0;
  virtual void visit(BoundFunctionCall &node) = 0;
  virtual void visit(BoundIndirectCall &node) = 0;
  virtual void visit(BoundFunctionReference &node) = 0;
  virtual void visit(BoundArrayLiteral &node) = 0;
  virtual void visit(BoundIndexAccess &node) = 0;
  virtual void visit(BoundRecordDeclaration &node) = 0;
  virtual void visit(BoundEnumDeclaration &node) = 0;
  virtual void visit(BoundTaggedUnionDeclaration &node) = 0;
  virtual void visit(BoundMemberAccess &node) = 0;
  virtual void visit(BoundStructLiteral &node) = 0;
  virtual void visit(BoundTaggedUnionLiteral &node) = 0;
  virtual void visit(BoundModuleReference &node) = 0;
  virtual void visit(BoundIfStatement &node) = 0;
  virtual void visit(BoundCaseStatement &node) = 0;
  virtual void visit(BoundWhileStatement &node) = 0;
  virtual void visit(BoundForStatement &node) = 0;
  virtual void visit(BoundBreakStatement &node) = 0;
  virtual void visit(BoundContinueStatement &node) = 0;
  virtual void visit(BoundAsmStatement &node) = 0;
  virtual void visit(BoundCast &node) = 0;
  virtual void visit(BoundNewExpression &node) = 0;
  virtual void visit(BoundWeakLockExpression &node) = 0;
  virtual void visit(BoundWeakAliveExpression &node) = 0;
  virtual void visit(BoundTryExpression &node) = 0;
  virtual void visit(BoundFallbackExpression &node) = 0;
  virtual void visit(BoundFailableHandleExpression &node) = 0;
  virtual void visit(BoundFailStatement &node) = 0;
  virtual void visit(BoundRangeExpression &node) = 0;
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

class BoundClassTypeTest : public BoundExpression {
public:
  std::unique_ptr<BoundExpression> expression;
  std::shared_ptr<zir::ClassType> targetType;

  BoundClassTypeTest(std::unique_ptr<BoundExpression> value,
                     std::shared_ptr<zir::ClassType> target)
      : BoundExpression(
            std::make_shared<zir::PrimitiveType>(zir::TypeKind::Bool)),
        expression(std::move(value)), targetType(std::move(target)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
  std::unique_ptr<BoundExpression> clone() const override {
    return std::make_unique<BoundClassTypeTest>(expression->clone(),
                                                targetType);
  }
};

/// Reads the value of a compound-assignment target (`a` in `a += b`) from the
/// address the enclosing BoundAssignment computes once.
class BoundCompoundTargetLoad : public BoundExpression {
public:
  explicit BoundCompoundTargetLoad(std::shared_ptr<zir::Type> t)
      : BoundExpression(std::move(t)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
  std::unique_ptr<BoundExpression> clone() const override {
    return std::make_unique<BoundCompoundTargetLoad>(type);
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

class BoundIndirectCall : public BoundExpression {
public:
  std::unique_ptr<BoundExpression> callee;
  std::vector<std::unique_ptr<BoundExpression>> arguments;

  BoundIndirectCall(std::unique_ptr<BoundExpression> c,
                    std::vector<std::unique_ptr<BoundExpression>> args,
                    std::shared_ptr<zir::Type> retType)
      : BoundExpression(std::move(retType)), callee(std::move(c)),
        arguments(std::move(args)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
  std::unique_ptr<BoundExpression> clone() const override {
    std::vector<std::unique_ptr<BoundExpression>> clonedArgs;
    for (const auto &arg : arguments)
      clonedArgs.push_back(arg->clone());
    return std::make_unique<BoundIndirectCall>(callee->clone(),
                                               std::move(clonedArgs), type);
  }
};

class BoundFunctionReference : public BoundExpression {
public:
  std::shared_ptr<FunctionSymbol> symbol;

  BoundFunctionReference(std::shared_ptr<FunctionSymbol> s,
                         std::shared_ptr<zir::Type> fpType)
      : BoundExpression(std::move(fpType)), symbol(std::move(s)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
  std::unique_ptr<BoundExpression> clone() const override {
    return std::make_unique<BoundFunctionReference>(symbol, type);
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
        expression->clone(), errorSymbol,
        handler ? handler->cloneBlock() : nullptr, type, errorType);
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
  bool returnsRef = false;
  explicit BoundReturnStatement(std::unique_ptr<BoundExpression> e,
                                bool ref = false)
      : expression(std::move(e)), returnsRef(ref) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
  std::unique_ptr<BoundStatement> cloneStatement() const override {
    return std::make_unique<BoundReturnStatement>(
        expression ? expression->clone() : nullptr, returnsRef);
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
  bool isCompound = false;

  BoundAssignment(std::unique_ptr<BoundExpression> t,
                  std::unique_ptr<BoundExpression> e, bool compound = false)
      : target(std::move(t)), expression(std::move(e)), isCompound(compound) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
  std::unique_ptr<BoundStatement> cloneStatement() const override {
    return std::make_unique<BoundAssignment>(target->clone(),
                                             expression->clone(), isCompound);
  }
};

class BoundIfStatement : public BoundStatement {
public:
  std::unique_ptr<BoundExpression> condition;
  std::unique_ptr<BoundBlock> thenBody;
  std::unique_ptr<BoundBlock> elseBody;
  std::shared_ptr<VariableSymbol> narrowedSource;
  std::shared_ptr<VariableSymbol> narrowedVariable;

  BoundIfStatement(std::unique_ptr<BoundExpression> cond,
                   std::unique_ptr<BoundBlock> thenB,
                   std::unique_ptr<BoundBlock> elseB,
                   std::shared_ptr<VariableSymbol> source = nullptr,
                   std::shared_ptr<VariableSymbol> narrowed = nullptr)
      : condition(std::move(cond)), thenBody(std::move(thenB)),
        elseBody(std::move(elseB)), narrowedSource(std::move(source)),
        narrowedVariable(std::move(narrowed)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
  std::unique_ptr<BoundStatement> cloneStatement() const override {
    return std::make_unique<BoundIfStatement>(
        condition->clone(), thenBody->cloneBlock(),
        elseBody ? elseBody->cloneBlock() : nullptr, narrowedSource,
        narrowedVariable);
  }
};

enum class BoundCasePatternKind {
  Literal,
  EnumVariant,
  TaggedUnionVariant,
  Record,
};

class BoundCasePattern;

struct BoundCaseRecordField {
  int index = -1;
  std::unique_ptr<BoundCasePattern> nested;
  std::shared_ptr<VariableSymbol> binding;
};

class BoundCasePattern {
public:
  BoundCasePatternKind kind = BoundCasePatternKind::Literal;
  std::unique_ptr<BoundExpression> value;
  int64_t variantTag = 0;
  std::shared_ptr<zir::Type> payloadType;
  std::unique_ptr<BoundExpression> payloadValue;
  std::unique_ptr<BoundCasePattern> payloadPattern;
  std::shared_ptr<VariableSymbol> payloadBinding;
  std::shared_ptr<zir::RecordType> recordType;
  std::vector<BoundCaseRecordField> recordFields;

  explicit BoundCasePattern(std::unique_ptr<BoundExpression> patternValue)
      : value(std::move(patternValue)) {}

  BoundCasePattern(BoundCasePatternKind patternKind, int64_t tag,
                   std::shared_ptr<zir::Type> payload = nullptr,
                   std::unique_ptr<BoundExpression> payloadLiteral = nullptr,
                   std::unique_ptr<BoundCasePattern> nestedPayload = nullptr,
                   std::shared_ptr<VariableSymbol> binding = nullptr)
      : kind(patternKind), variantTag(tag), payloadType(std::move(payload)),
        payloadValue(std::move(payloadLiteral)),
        payloadPattern(std::move(nestedPayload)),
        payloadBinding(std::move(binding)) {}

  BoundCasePattern(std::shared_ptr<zir::RecordType> type,
                   std::vector<BoundCaseRecordField> fields)
      : kind(BoundCasePatternKind::Record), recordType(std::move(type)),
        recordFields(std::move(fields)) {}

  BoundCasePattern clone() const {
    if (kind == BoundCasePatternKind::Literal) {
      return BoundCasePattern(value ? value->clone() : nullptr);
    }
    if (kind == BoundCasePatternKind::Record) {
      std::vector<BoundCaseRecordField> fields;
      fields.reserve(recordFields.size());
      for (const auto &field : recordFields) {
        fields.push_back({field.index,
                          field.nested ? std::make_unique<BoundCasePattern>(
                                             field.nested->clone())
                                       : nullptr,
                          field.binding});
      }
      return BoundCasePattern(recordType, std::move(fields));
    }
    return BoundCasePattern(kind, variantTag, payloadType,
                            payloadValue ? payloadValue->clone() : nullptr,
                            payloadPattern ? std::make_unique<BoundCasePattern>(
                                                 payloadPattern->clone())
                                           : nullptr,
                            payloadBinding);
  }
};

class BoundCaseArm {
public:
  bool isElse = false;
  std::vector<BoundCasePattern> patterns;
  std::shared_ptr<VariableSymbol> payloadBinding;
  std::vector<std::shared_ptr<VariableSymbol>> recordBindings;
  std::unique_ptr<BoundBlock> body;

  BoundCaseArm(bool wildcard, std::vector<BoundCasePattern> armPatterns,
               std::shared_ptr<VariableSymbol> binding,
               std::vector<std::shared_ptr<VariableSymbol>> bindings,
               std::unique_ptr<BoundBlock> armBody)
      : isElse(wildcard), patterns(std::move(armPatterns)),
        payloadBinding(std::move(binding)), recordBindings(std::move(bindings)),
        body(std::move(armBody)) {}

  BoundCaseArm clone() const {
    std::vector<BoundCasePattern> clonedPatterns;
    clonedPatterns.reserve(patterns.size());
    for (const auto &pattern : patterns) {
      clonedPatterns.push_back(pattern.clone());
    }
    return BoundCaseArm(isElse, std::move(clonedPatterns), payloadBinding,
                        recordBindings, body ? body->cloneBlock() : nullptr);
  }
};

class BoundCaseStatement : public BoundStatement {
public:
  std::unique_ptr<BoundExpression> scrutinee;
  std::vector<BoundCaseArm> arms;
  bool guaranteesMatch = false;

  BoundCaseStatement(std::unique_ptr<BoundExpression> subject,
                     std::vector<BoundCaseArm> caseArms, bool guarantees)
      : scrutinee(std::move(subject)), arms(std::move(caseArms)),
        guaranteesMatch(guarantees) {}

  void accept(BoundVisitor &v) override { v.visit(*this); }
  std::unique_ptr<BoundStatement> cloneStatement() const override {
    std::vector<BoundCaseArm> clonedArms;
    clonedArms.reserve(arms.size());
    for (const auto &arm : arms) {
      clonedArms.push_back(arm.clone());
    }
    return std::make_unique<BoundCaseStatement>(
        scrutinee->clone(), std::move(clonedArms), guaranteesMatch);
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

struct BoundAsmOperand {
  std::string constraint;
  std::unique_ptr<BoundExpression> expr;
};

class BoundAsmStatement : public BoundStatement {
public:
  std::string assembly;
  std::vector<BoundAsmOperand> outputs;
  std::vector<BoundAsmOperand> inputs;
  std::vector<std::string> clobbers;

  BoundAsmStatement(std::string asmStr, std::vector<BoundAsmOperand> outs,
                    std::vector<BoundAsmOperand> ins,
                    std::vector<std::string> clob)
      : assembly(std::move(asmStr)), outputs(std::move(outs)),
        inputs(std::move(ins)), clobbers(std::move(clob)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
  std::unique_ptr<BoundStatement> cloneStatement() const override {
    std::vector<BoundAsmOperand> outs;
    outs.reserve(outputs.size());
    for (const auto &o : outputs)
      outs.push_back({o.constraint, o.expr->clone()});
    std::vector<BoundAsmOperand> ins;
    ins.reserve(inputs.size());
    for (const auto &i : inputs)
      ins.push_back({i.constraint, i.expr->clone()});
    return std::make_unique<BoundAsmStatement>(assembly, std::move(outs),
                                               std::move(ins), clobbers);
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

class BoundTaggedUnionDeclaration : public BoundNode {
public:
  std::shared_ptr<zir::TaggedUnionType> type;
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

class BoundTaggedUnionLiteral : public BoundExpression {
public:
  std::string variantName;
  int64_t tag;
  std::unique_ptr<BoundExpression> payload;

  BoundTaggedUnionLiteral(std::shared_ptr<zir::TaggedUnionType> t,
                          std::string variant, int64_t variantTag,
                          std::unique_ptr<BoundExpression> payloadExpr)
      : BoundExpression(std::move(t)), variantName(std::move(variant)),
        tag(variantTag), payload(std::move(payloadExpr)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
  std::unique_ptr<BoundExpression> clone() const override {
    return std::make_unique<BoundTaggedUnionLiteral>(
        std::static_pointer_cast<zir::TaggedUnionType>(type), variantName, tag,
        payload ? payload->clone() : nullptr);
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
  ~BoundRootNode() override {
    for (const auto &record : records) {
      if (record && record->type) {
        record->type->clearFields();
      }
    }
    for (const auto &taggedUnion : taggedUnions) {
      if (taggedUnion && taggedUnion->type) {
        taggedUnion->type->clearVariants();
      }
    }
    for (const auto &type : genericTypes) {
      type->clearFields();
    }
  }

  std::vector<std::unique_ptr<BoundRecordDeclaration>> records;
  std::vector<std::unique_ptr<BoundEnumDeclaration>> enums;
  std::vector<std::unique_ptr<BoundTaggedUnionDeclaration>> taggedUnions;
  std::vector<std::unique_ptr<BoundVariableDeclaration>> globals;
  std::vector<std::unique_ptr<BoundFunctionDeclaration>> functions;
  std::vector<std::unique_ptr<BoundExternalFunctionDeclaration>>
      externalFunctions;
  // Generic instances do not necessarily have a source declaration, but their
  // fields can recursively refer to the same instantiated type. Keep them
  // alive for semantic analysis and codegen, then release their field graph
  // when this compilation unit is discarded.
  std::vector<std::shared_ptr<zir::RecordType>> genericTypes;
  void accept(BoundVisitor &v) override { v.visit(*this); }
};

class BoundRangeExpression : public BoundExpression {
public:
  std::unique_ptr<BoundExpression> start;
  std::unique_ptr<BoundExpression> end;
  std::unique_ptr<BoundExpression> step;

  BoundRangeExpression(std::unique_ptr<BoundExpression> s,
                       std::unique_ptr<BoundExpression> e,
                       std::unique_ptr<BoundExpression> st,
                       std::shared_ptr<zir::Type> t)
      : BoundExpression(std::move(t)), start(std::move(s)),
        end(std::move(e)), step(std::move(st)) {}
  void accept(BoundVisitor &v) override { v.visit(*this); }
  std::unique_ptr<BoundExpression> clone() const override {
    return std::make_unique<BoundRangeExpression>(
        start->clone(), end->clone(), step ? step->clone() : nullptr, type);
  }
};

} // namespace sema
