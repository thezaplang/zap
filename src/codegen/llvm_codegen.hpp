#pragma once
#include "../ir/module.hpp"
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
#include <vector>

namespace codegen {
class ClassArcEmitter;

class LLVMCodeGen {
public:
  explicit LLVMCodeGen(std::string targetTriple = {},
                       bool freestanding = false);
  ~LLVMCodeGen();

  void generate(const zir::Module &module);

  void printIR(llvm::raw_ostream &) const;
  bool verifyModule(llvm::raw_ostream &diagnostics) const;

  bool emitObjectFile(const std::string &path, int optimization_level = 0);
  bool emitAssemblyFile(const std::string &path, int optimization_level = 0);

private:
  llvm::LLVMContext ctx_;
  llvm::IRBuilder<> builder_;
  std::unique_ptr<llvm::Module> module_;
  std::string targetTriple_;
  bool freestanding_ = false;

  llvm::Function *currentFn_ = nullptr;
  std::map<std::string, llvm::GlobalVariable *> globalValues_;
  std::map<std::string, llvm::Function *> functionMap_;
  std::map<std::string, const zir::Function *> zirFunctionMap_;
  std::map<std::string, llvm::StructType *> structCache_;
  std::map<std::string, std::map<int, llvm::Function *>> classVirtualMethodFns_;
  std::map<std::string, llvm::GlobalVariable *> classVTables_;
  std::map<std::string, llvm::GlobalVariable *> classInterfaceTables_;
  std::map<std::string, llvm::Function *> classRetainFns_;
  std::map<std::string, llvm::Function *> classReleaseFns_;
  std::map<std::string, llvm::Function *> classDestroyFns_;
  std::map<std::string, llvm::Function *> classTraceFns_;
  std::map<std::string, llvm::Function *> classDestructorFns_;
  std::map<std::string, llvm::GlobalVariable *> classMetadataGlobals_;
  std::map<std::string, std::shared_ptr<zir::ClassType>> classTypes_;
  std::unordered_set<std::string> cyclicClasses_;
  std::unique_ptr<ClassArcEmitter> arcEmitter_;
  std::unordered_map<const zir::Value *, llvm::Value *> zirValueMap_;
  std::unordered_set<const zir::Value *> refReturnValues_;
  std::unordered_map<std::string, llvm::BasicBlock *> zirBlockMap_;
  std::unordered_map<std::string, llvm::BasicBlock *> zirBlockExitMap_;
  struct PendingPhiIncoming {
    llvm::PHINode *phi;
    std::vector<std::pair<std::string, std::shared_ptr<zir::Value>>> incoming;
  };
  std::vector<PendingPhiIncoming> pendingPhiIncoming_;
  std::unordered_set<const zir::Value *> zirClassParamAllocas_;
  std::unordered_set<const zir::Value *> zirPendingClassParamInitAllocas_;
  std::vector<std::pair<std::shared_ptr<zir::Type>, llvm::Value *>>
      zirFunctionClassLocals_;
  std::vector<std::pair<std::shared_ptr<zir::Type>, llvm::Value *>>
      zirFunctionStringLocals_;
  std::vector<std::pair<std::shared_ptr<zir::Type>, llvm::Value *>>
      zirFunctionAggregateLocals_;
  const zir::Function *currentZIRFunction_ = nullptr;
  size_t zirParamSpillIndex_ = 0;

  int nextStringId_ = 0;

  llvm::Constant *getOrCreateGlobalString(const std::string &str,
                                          std::string &globalName,
                                          bool owned);

  llvm::Type *toLLVMType(const zir::Type &ty);
  llvm::IntegerType *nativeIntegerType();
  llvm::Type *toLLVMAggregateFieldType(const std::shared_ptr<zir::Type> &type);
  llvm::FunctionType *buildFunctionType(const zir::Function &fn);
  void initializeModule();
  void declareZIRFunction(const zir::Function &fn, bool isExternal);
  void emitZIRFunction(const zir::Function &fn);
  void emitZIRInstruction(const zir::Instruction &inst);
  void resolveZIRPhiIncomingBlocks();
  void buildInlineAsmCall(const std::string &assembly,
                          const std::vector<std::string> &outConstraints,
                          const std::vector<llvm::Value *> &outAddrs,
                          const std::vector<llvm::Type *> &outValueTypes,
                          const std::vector<std::string> &inConstraints,
                          const std::vector<llvm::Value *> &inValues,
                          const std::vector<std::string> &clobbers);
  llvm::Value *lowerZIRValue(const std::shared_ptr<zir::Value> &value);
  llvm::Value *lowerZIRRValue(const std::shared_ptr<zir::Value> &value);
  llvm::Constant *lowerZIRConstant(const zir::Constant &constant);
  llvm::Constant *
  lowerZIRAggregateConstant(const zir::AggregateConstant &constant);
  llvm::Constant *lowerZIRArrayConstant(const zir::ArrayConstant &constant);
  llvm::Value *lowerZIRCast(llvm::Value *src,
                            const std::shared_ptr<zir::Type> &sourceType,
                            const std::shared_ptr<zir::Type> &targetType);
  llvm::Value *emitStringConversion(
      llvm::Value *source, const std::shared_ptr<zir::Type> &sourceType,
      const std::shared_ptr<zir::Type> &targetType,
      const llvm::Twine &namePrefix);
  llvm::Value *emitStringConcat(llvm::Value *lhs, llvm::Value *rhs,
                                const std::shared_ptr<zir::Type> &lhsType,
                                const std::shared_ptr<zir::Type> &rhsType,
                                const std::shared_ptr<zir::Type> &resultType);

  llvm::AllocaInst *createEntryAlloca(llvm::Function *fn,
                                      const std::string &name, llvm::Type *ty);
  bool isClassType(const std::shared_ptr<zir::Type> &type) const;
  bool isWeakClassType(const std::shared_ptr<zir::Type> &type) const;
  bool isOwnedStringType(const std::shared_ptr<zir::Type> &type) const;
  bool containsManagedValues(const std::shared_ptr<zir::Type> &type) const;
  void emitManagedRetain(llvm::Value *value,
                         const std::shared_ptr<zir::Type> &type);
  void emitManagedRelease(llvm::Value *value,
                          const std::shared_ptr<zir::Type> &type);
  void emitManagedForActiveTaggedUnion(
      llvm::Value *value, const std::shared_ptr<zir::TaggedUnionType> &type,
      void (LLVMCodeGen::*operation)(llvm::Value *,
                                     const std::shared_ptr<zir::Type> &));
  void emitOwnershipRelease(llvm::Value *value,
                            const std::shared_ptr<zir::Type> &type,
                            zir::ValueOwnership ownership);
  void emitArcCollectionSafePoint();
#if defined(ZAP_RUNTIME_INSTRUMENTATION)
  void emitRuntimeOwnershipEvent(const char *name);
#endif
  void emitZIRFunctionReleases();
  void emitRetainIfNeeded(llvm::Value *value,
                          const std::shared_ptr<zir::Type> &type);
  void emitReleaseIfNeeded(llvm::Value *value,
                           const std::shared_ptr<zir::Type> &type);
  llvm::Value *emitStringRetainIfNeeded(llvm::Value *value,
                                        const std::shared_ptr<zir::Type> &type);
  void emitStringReleaseIfNeeded(llvm::Value *value,
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
                        zir::ValueOwnership valueOwnership,
                        bool skipReleaseOld = false);
  void emitStoreWithStringArc(llvm::Value *addr, llvm::Value *value,
                              const std::shared_ptr<zir::Type> &type,
                              bool valueIsOwned, bool skipReleaseOld = false);
  void ensureClassArcSupport(const std::shared_ptr<zir::ClassType> &classType);
  void emitInterfaceMethodTrampolines(const zir::Module &module);
  void computeCyclicClasses(const zir::Module &module);
  llvm::StructType *getOrCreateClassStruct(const zir::ClassType &ct);
  void finalizeClassStruct(const zir::ClassType &ct);

  friend class ClassArcEmitter;
};

} // namespace codegen
