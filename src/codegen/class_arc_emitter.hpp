#pragma once

#include "../sema/bound_nodes.hpp"
#include <memory>

namespace llvm {
class Value;
}

namespace codegen {
class LLVMCodeGen;

class ClassArcEmitter {
public:
  explicit ClassArcEmitter(LLVMCodeGen &codegen);

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
                        bool valueIsOwned, bool skipReleaseOld = false);
  void emitScopeReleases();
  void ensureArcSupport(sema::BoundRootNode &root);
  void ensureClassArcSupport(const std::shared_ptr<zir::ClassType> &classType);

private:
  LLVMCodeGen &codegen_;
};
} // namespace codegen
