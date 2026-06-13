#include "../utils/string_type_utils.hpp"
#include "class_arc_emitter.hpp"
#include "llvm_codegen.hpp"

namespace codegen {
namespace {
bool isStringLikeStruct(llvm::Type *ty) {
  auto *st = llvm::dyn_cast_or_null<llvm::StructType>(ty);
  return st && st->getNumElements() == 2;
}
} // namespace

bool LLVMCodeGen::isClassType(const std::shared_ptr<zir::Type> &type) const {
  return arcEmitter_->isClassType(type);
}

bool LLVMCodeGen::isWeakClassType(
    const std::shared_ptr<zir::Type> &type) const {
  return arcEmitter_->isWeakClassType(type);
}

bool LLVMCodeGen::isOwnedStringType(
    const std::shared_ptr<zir::Type> &type) const {
  return zap::text::isStringType(type) && !zap::text::isStringViewType(type);
}

bool LLVMCodeGen::expressionProducesOwnedClass(
    const sema::BoundExpression *expr) const {
  return arcEmitter_->expressionProducesOwnedClass(expr);
}

bool LLVMCodeGen::expressionProducesOwnedString(
    const sema::BoundExpression *expr) const {
  if (!expr || !isOwnedStringType(expr->type)) {
    return false;
  }
  return dynamic_cast<const sema::BoundLiteral *>(expr) == nullptr;
}

void LLVMCodeGen::emitRetainIfNeeded(llvm::Value *value,
                                     const std::shared_ptr<zir::Type> &type) {
  if (isOwnedStringType(type)) {
    (void)emitStringRetainIfNeeded(value, type);
    return;
  }
  arcEmitter_->emitRetainIfNeeded(value, type);
}

void LLVMCodeGen::emitReleaseIfNeeded(llvm::Value *value,
                                      const std::shared_ptr<zir::Type> &type) {
  if (isOwnedStringType(type)) {
    emitStringReleaseIfNeeded(value, type);
    return;
  }
  arcEmitter_->emitReleaseIfNeeded(value, type);
}

llvm::Value *
LLVMCodeGen::emitStringRetainIfNeeded(llvm::Value *value,
                                      const std::shared_ptr<zir::Type> &type) {
  if (!isOwnedStringType(type)) {
    return value;
  }
  auto *stringTy = toLLVMType(*type);
  if (value->getType() != stringTy && isStringLikeStruct(value->getType()) &&
      isStringLikeStruct(stringTy)) {
    auto *ptr = builder_.CreateExtractValue(value, {0}, "str.cvt.ptr");
    auto *len = builder_.CreateExtractValue(value, {1}, "str.cvt.len");
    llvm::Value *converted = llvm::UndefValue::get(stringTy);
    converted =
        builder_.CreateInsertValue(converted, ptr, {0}, "str.cvt.ptr.i");
    converted =
        builder_.CreateInsertValue(converted, len, {1}, "str.cvt.len.i");
    value = converted;
  }
  auto *fnTy = llvm::FunctionType::get(stringTy, {stringTy}, false);
  auto callee = module_->getOrInsertFunction("zap_string_retain", fnTy);
  return builder_.CreateCall(fnTy, callee.getCallee(), {value}, "str.retain");
}

void LLVMCodeGen::emitStringReleaseIfNeeded(
    llvm::Value *value, const std::shared_ptr<zir::Type> &type) {
  if (!isOwnedStringType(type)) {
    return;
  }
  auto *stringTy = toLLVMType(*type);
  if (value->getType() != stringTy && isStringLikeStruct(value->getType()) &&
      isStringLikeStruct(stringTy)) {
    auto *ptr = builder_.CreateExtractValue(value, {0}, "str.cvt.ptr");
    auto *len = builder_.CreateExtractValue(value, {1}, "str.cvt.len");
    llvm::Value *converted = llvm::UndefValue::get(stringTy);
    converted =
        builder_.CreateInsertValue(converted, ptr, {0}, "str.cvt.ptr.i");
    converted =
        builder_.CreateInsertValue(converted, len, {1}, "str.cvt.len.i");
    value = converted;
  }
  auto *fnTy =
      llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), {stringTy}, false);
  auto callee = module_->getOrInsertFunction("zap_string_release", fnTy);
  builder_.CreateCall(fnTy, callee.getCallee(), {value});
}

void LLVMCodeGen::emitRetainWeakIfNeeded(
    llvm::Value *value, const std::shared_ptr<zir::Type> &type) {
  arcEmitter_->emitRetainWeakIfNeeded(value, type);
}

void LLVMCodeGen::emitReleaseWeakIfNeeded(
    llvm::Value *value, const std::shared_ptr<zir::Type> &type) {
  arcEmitter_->emitReleaseWeakIfNeeded(value, type);
}

llvm::Value *
LLVMCodeGen::emitWeakAlive(llvm::Value *value,
                           const std::shared_ptr<zir::Type> &type) {
  return arcEmitter_->emitWeakAlive(value, type);
}

llvm::Value *LLVMCodeGen::emitWeakLock(llvm::Value *value,
                                       const std::shared_ptr<zir::Type> &type) {
  return arcEmitter_->emitWeakLock(value, type);
}

void LLVMCodeGen::emitStoreWithArc(llvm::Value *addr, llvm::Value *value,
                                   const std::shared_ptr<zir::Type> &type,
                                   bool valueIsOwned, bool skipReleaseOld) {
  if (isOwnedStringType(type)) {
    emitStoreWithStringArc(addr, value, type, valueIsOwned, skipReleaseOld);
    return;
  }
  arcEmitter_->emitStoreWithArc(addr, value, type, valueIsOwned,
                                skipReleaseOld);
}

void LLVMCodeGen::emitStoreWithStringArc(llvm::Value *addr, llvm::Value *value,
                                         const std::shared_ptr<zir::Type> &type,
                                         bool valueIsOwned,
                                         bool skipReleaseOld) {
  if (!skipReleaseOld) {
    auto *oldValue = builder_.CreateLoad(toLLVMType(*type), addr, "str.old");
    emitStringReleaseIfNeeded(oldValue, type);
  }

  auto *storedValue =
      valueIsOwned ? value : emitStringRetainIfNeeded(value, type);
  builder_.CreateStore(storedValue, addr);
}

void LLVMCodeGen::emitScopeReleases() {
  arcEmitter_->emitScopeReleases();
  if (scopeStringLocals_.empty()) {
    return;
  }
  auto &locals = scopeStringLocals_.back();
  for (auto it = locals.rbegin(); it != locals.rend(); ++it) {
    auto *value = builder_.CreateLoad(toLLVMType(*it->first), it->second,
                                      "str.scope.release");
    emitStringReleaseIfNeeded(value, it->first);
  }
}

void LLVMCodeGen::ensureClassArcSupport(
    const std::shared_ptr<zir::ClassType> &classType) {
  arcEmitter_->ensureClassArcSupport(classType);
}

void LLVMCodeGen::ensureArcSupport(sema::BoundRootNode &root) {
  arcEmitter_->ensureArcSupport(root);
}
} // namespace codegen
