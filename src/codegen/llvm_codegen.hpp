#pragma once
#include "../ir/module.hpp"
#include "../sema/bound_nodes.hpp"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Target/TargetMachine.h>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace codegen {
class ClassArcEmitter;

class LLVMCodeGen : public sema::BoundVisitor {
public:
  LLVMCodeGen();
  ~LLVMCodeGen();

  void generate(sema::BoundRootNode &root);
  void generate(const zir::Module &module);

  void printIR(llvm::raw_ostream &) const;

  bool emitObjectFile(const std::string &path, int optimization_level = 0);

  void visit(sema::BoundRootNode &node) override;
  void visit(sema::BoundFunctionDeclaration &node) override;
  void visit(sema::BoundExternalFunctionDeclaration &node) override;
  void visit(sema::BoundBlock &node) override;
  void visit(sema::BoundVariableDeclaration &node) override;
  void visit(sema::BoundReturnStatement &node) override;
  void visit(sema::BoundAssignment &node) override;
  void visit(sema::BoundExpressionStatement &node) override;
  void visit(sema::BoundLiteral &node) override;
  void visit(sema::BoundVariableExpression &node) override;
  void visit(sema::BoundBinaryExpression &node) override;
  void visit(sema::BoundTernaryExpression &node) override;
  void visit(sema::BoundUnaryExpression &node) override;
  void visit(sema::BoundFunctionCall &node) override;
  void visit(sema::BoundArrayLiteral &node) override;
  void visit(sema::BoundIndexAccess &node) override;
  void visit(sema::BoundRecordDeclaration &node) override;
  void visit(sema::BoundEnumDeclaration &node) override;
  void visit(sema::BoundMemberAccess &node) override;
  void visit(sema::BoundStructLiteral &node) override;
  void visit(sema::BoundModuleReference &node) override;
  void visit(sema::BoundIfStatement &node) override;
  void visit(sema::BoundWhileStatement &node) override;
  void visit(sema::BoundBreakStatement &node) override;
  void visit(sema::BoundContinueStatement &node) override;
  void visit(sema::BoundCast &node) override;
  void visit(sema::BoundNewExpression &node) override;
  void visit(sema::BoundWeakLockExpression &node) override;
  void visit(sema::BoundWeakAliveExpression &node) override;

private:
  llvm::LLVMContext ctx_;
  llvm::IRBuilder<> builder_;
  std::unique_ptr<llvm::Module> module_;

  llvm::Function *currentFn_ = nullptr;
  llvm::Value *lastValue_ = nullptr;
  bool evaluateAsAddr_ = false;

  std::map<std::string, llvm::Value *> localValues_;
  std::map<std::string, llvm::GlobalVariable *> globalValues_;
  std::map<std::string, llvm::Function *> functionMap_;
  std::map<std::string, const zir::Function *> zirFunctionMap_;
  std::map<std::string, llvm::StructType *> structCache_;
  std::map<std::string, std::map<int, llvm::Function *>> classVirtualMethodFns_;
  std::map<std::string, llvm::GlobalVariable *> classVTables_;
  std::map<std::string, llvm::Function *> classRetainFns_;
  std::map<std::string, llvm::Function *> classReleaseFns_;
  std::map<std::string, llvm::Function *> classDestroyFns_;
  std::map<std::string, llvm::Function *> classDestructorFns_;
  std::map<std::string, llvm::GlobalVariable *> classMetadataGlobals_;
  std::vector<std::vector<std::pair<std::shared_ptr<zir::Type>, llvm::Value *>>>
      scopeClassLocals_;
  std::unique_ptr<ClassArcEmitter> arcEmitter_;
  std::unordered_map<const zir::Value *, llvm::Value *> zirValueMap_;
  std::unordered_map<std::string, llvm::BasicBlock *> zirBlockMap_;
  std::unordered_map<std::string, llvm::BasicBlock *> zirBlockExitMap_;
  std::unordered_set<const zir::Value *> zirOwnedClassValues_;
  std::unordered_set<const zir::Value *> zirClassParamAllocas_;
  std::unordered_set<const zir::Value *> zirPendingClassParamInitAllocas_;
  std::vector<std::pair<std::shared_ptr<zir::Type>, llvm::Value *>>
      zirFunctionClassLocals_;
  const zir::Function *currentZIRFunction_ = nullptr;
  size_t zirParamSpillIndex_ = 0;

  int nextStringId_ = 0;

  llvm::Constant *getOrCreateGlobalString(const std::string &str,
                                          std::string &globalName);

  std::vector<std::pair<llvm::BasicBlock *, llvm::BasicBlock *>> loopBBStack_;

  llvm::Type *toLLVMType(const zir::Type &ty);
  llvm::FunctionType *buildFunctionType(const sema::FunctionSymbol &sym,
                                        bool injectMainProcessArgs = false);
  llvm::FunctionType *buildFunctionType(const zir::Function &fn);
  void initializeModule();
  void declareZIRFunction(const zir::Function &fn, bool isExternal);
  void emitZIRFunction(const zir::Function &fn);
  void emitZIRInstruction(const zir::Instruction &inst);
  llvm::Value *lowerZIRValue(const std::shared_ptr<zir::Value> &value);
  llvm::Value *lowerZIRRValue(const std::shared_ptr<zir::Value> &value);
  llvm::Constant *lowerZIRConstant(const zir::Constant &constant);
  llvm::Constant *lowerZIRAggregateConstant(
      const zir::AggregateConstant &constant);
  llvm::Value *lowerZIRCast(llvm::Value *src,
                            const std::shared_ptr<zir::Type> &sourceType,
                            const std::shared_ptr<zir::Type> &targetType);
  llvm::Value *emitStringConcat(llvm::Value *lhs, llvm::Value *rhs,
                                const std::shared_ptr<zir::Type> &lhsType,
                                const std::shared_ptr<zir::Type> &rhsType,
                                const std::shared_ptr<zir::Type> &resultType);

  llvm::AllocaInst *createEntryAlloca(llvm::Function *fn,
                                      const std::string &name, llvm::Type *ty);
  bool isClassType(const std::shared_ptr<zir::Type> &type) const;
  bool isWeakClassType(const std::shared_ptr<zir::Type> &type) const;
  bool expressionProducesOwnedClass(const sema::BoundExpression *expr) const;
  void emitRetainIfNeeded(llvm::Value *value,
                          const std::shared_ptr<zir::Type> &type);
  void emitReleaseIfNeeded(llvm::Value *value,
                           const std::shared_ptr<zir::Type> &type);
  void emitRetainWeakIfNeeded(llvm::Value *value,
                              const std::shared_ptr<zir::Type> &type);
  void emitReleaseWeakIfNeeded(llvm::Value *value,
                               const std::shared_ptr<zir::Type> &type);
  llvm::Value *emitWeakAlive(llvm::Value *value,
                             const std::shared_ptr<zir::Type> &type);
  llvm::Value *emitWeakLock(llvm::Value *value,
                            const std::shared_ptr<zir::Type> &type);
  void emitStoreWithArc(llvm::Value *addr, llvm::Value *value,
                        const std::shared_ptr<zir::Type> &type,
                        bool valueIsOwned);
  void emitScopeReleases();
  void ensureArcSupport(sema::BoundRootNode &root);
  void ensureClassArcSupport(const std::shared_ptr<zir::ClassType> &classType);

  friend class ClassArcEmitter;
};

} // namespace codegen
