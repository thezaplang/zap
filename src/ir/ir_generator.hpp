#pragma once
#include "../sema/bound_nodes.hpp"
#include "function_reachability.hpp"
#include "module.hpp"
#include <map>
#include <memory>
#include <set>
#include <stack>
#include <string>
#include <vector>

namespace zir {

class BoundIRGenerator : public sema::BoundVisitor {
public:
  std::unique_ptr<Module> generate(sema::BoundRootNode &root);

  void visit(sema::BoundRootNode &node) override;
  void visit(sema::BoundFunctionDeclaration &node) override;
  void visit(sema::BoundExternalFunctionDeclaration &node) override;
  void visit(sema::BoundBlock &node) override;
  void visit(sema::BoundVariableDeclaration &node) override;
  void visit(sema::BoundReturnStatement &node) override;
  void visit(sema::BoundAssignment &node) override;
  void visit(sema::BoundExpressionStatement &node) override;
  void visit(sema::BoundLiteral &node) override;
  void visit(sema::BoundRangeExpression &node) override;
  void visit(sema::BoundVariableExpression &node) override;
  void visit(sema::BoundClassTypeTest &node) override;
  void visit(sema::BoundCompoundTargetLoad &node) override;
  void visit(sema::BoundBinaryExpression &node) override;
  void visit(sema::BoundTernaryExpression &node) override;
  void visit(sema::BoundUnaryExpression &node) override;
  void visit(sema::BoundFunctionCall &node) override;
  void visit(sema::BoundIndirectCall &node) override;
  void visit(sema::BoundFunctionReference &node) override;
  void visit(sema::BoundArrayLiteral &node) override;
  void visit(sema::BoundIndexAccess &node) override;
  void visit(sema::BoundRecordDeclaration &node) override;
  void visit(sema::BoundEnumDeclaration &node) override;
  void visit(sema::BoundTaggedUnionDeclaration &node) override;
  void visit(sema::BoundMemberAccess &node) override;
  void visit(sema::BoundStructLiteral &node) override;
  void visit(sema::BoundTaggedUnionLiteral &node) override;
  void visit(sema::BoundModuleReference &node) override;
  void visit(sema::BoundIfStatement &node) override;
  void visit(sema::BoundCaseStatement &node) override;
  void visit(sema::BoundWhileStatement &node) override;
  void visit(sema::BoundForStatement &node) override;
  void visit(sema::BoundBreakStatement &node) override;
  void visit(sema::BoundContinueStatement &node) override;
  void visit(sema::BoundAsmStatement &node) override;
  void visit(sema::BoundCast &node) override;
  void visit(sema::BoundNewExpression &node) override;
  void visit(sema::BoundWeakLockExpression &node) override;
  void visit(sema::BoundWeakAliveExpression &node) override;
  void visit(sema::BoundTryExpression &node) override;
  void visit(sema::BoundFallbackExpression &node) override;
  void visit(sema::BoundFailableHandleExpression &node) override;
  void visit(sema::BoundFailStatement &node) override;

private:
  std::unique_ptr<Module> module_;
  FunctionReachability reachability_;
  Function *currentFunction_ = nullptr;
  BasicBlock *currentBlock_ = nullptr;

  std::map<std::shared_ptr<sema::Symbol>, std::shared_ptr<Value>> symbolMap_;
  std::map<std::shared_ptr<sema::Symbol>, std::shared_ptr<Value>>
      globalSymbolMap_;
  std::stack<std::shared_ptr<Value>> valueStack_;

  int nextRegisterId_ = 0;
  int nextBlockId_ = 0;

  std::vector<std::pair<std::string, std::string>> loopLabelStack_;

  std::shared_ptr<Value> lastResultValue_ = nullptr;
  bool evaluateAsAddress_ = false;
  std::shared_ptr<Value> compoundTargetAddr_ = nullptr;
  std::shared_ptr<Value>
  createRegister(std::shared_ptr<Type> type,
                 ValueOwnership ownership = ValueOwnership::Borrowed);
  void emitInitializationStore(std::shared_ptr<Value> value,
                               std::shared_ptr<Value> destination);
  std::shared_ptr<Value> materializeOwnedValue(std::shared_ptr<Value> value);
  void prepareCallArgument(std::shared_ptr<Value> &value,
                           ParameterOwnership parameterOwnership);
  void emitReturn(std::shared_ptr<Value> value = nullptr);
  std::string createBlockLabel(const std::string &prefix);
  void emitCaseRecordTest(const sema::BoundCasePattern &record,
                          const std::shared_ptr<Value> &address,
                          const std::string &successLabel,
                          const std::string &failureLabel);
  void materializeCaseRecordBindings(const sema::BoundCasePattern &pattern,
                                     const std::shared_ptr<Value> &address);
  void materializeCasePayloadBinding(
      const sema::BoundCaseArm &arm,
      const std::shared_ptr<Value> &taggedUnionAddress);
  std::shared_ptr<Value>
  lowerConstantExpression(const sema::BoundExpression &expression);
  std::shared_ptr<Value> lowerConstantExpression(
      const sema::BoundExpression &expression,
      std::set<const sema::VariableSymbol *> &resolvingConstants);

  std::shared_ptr<Value>
  emitFailableFieldLoad(const std::shared_ptr<Value> &value, int fieldIndex,
                        const std::shared_ptr<Type> &fieldType);
  std::shared_ptr<Value> emitFailableOk(const std::shared_ptr<Value> &value);
  std::shared_ptr<Value> emitFailableValue(const std::shared_ptr<Value> &value);
  std::shared_ptr<Value> emitFailableError(const std::shared_ptr<Value> &value);
};

} // namespace zir
