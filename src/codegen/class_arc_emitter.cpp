#include "class_arc_emitter.hpp"
#include "class_layout.hpp"
#include "llvm_codegen.hpp"
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>
#include <limits>
#include <stdexcept>

namespace codegen {

ClassArcEmitter::ClassArcEmitter(LLVMCodeGen &codegen) : codegen_(codegen) {}

llvm::Function *
ClassArcEmitter::getOrCreateRefcountFailureFunction(const char *name) {
  auto it = codegen_.functionMap_.find(name);
  if (it != codegen_.functionMap_.end()) {
    return it->second;
  }

  auto *failureType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(codegen_.ctx_), {}, false);
  auto *failureFn = llvm::Function::Create(
      failureType, llvm::Function::ExternalLinkage, name, *codegen_.module_);
  codegen_.functionMap_[name] = failureFn;
  return failureFn;
}

llvm::Function *ClassArcEmitter::getOrCreateArcDeallocateFunction() {
  auto existing = codegen_.functionMap_.find("zap_arc_deallocate");
  if (existing != codegen_.functionMap_.end()) {
    return existing->second;
  }

  auto *rawPtrTy = llvm::PointerType::getUnqual(codegen_.ctx_);
  auto *deallocateType = llvm::FunctionType::get(
      llvm::Type::getVoidTy(codegen_.ctx_), {rawPtrTy, rawPtrTy}, false);
  auto *deallocateFn =
      llvm::Function::Create(deallocateType, llvm::Function::ExternalLinkage,
                             "zap_arc_deallocate", *codegen_.module_);
  codegen_.functionMap_["zap_arc_deallocate"] = deallocateFn;
  return deallocateFn;
}

llvm::Value *ClassArcEmitter::emitArcRuntimeContext() {
  auto it = codegen_.functionMap_.find("zap_arc_default_context");
  if (it == codegen_.functionMap_.end()) {
    auto *rawPtrTy = llvm::PointerType::getUnqual(codegen_.ctx_);
    auto *contextType = llvm::FunctionType::get(rawPtrTy, {}, false);
    auto *contextFn = llvm::Function::Create(
        contextType, llvm::Function::ExternalLinkage,
        "zap_arc_default_context", *codegen_.module_);
    it = codegen_.functionMap_.emplace("zap_arc_default_context", contextFn)
             .first;
  }
  return codegen_.builder_.CreateCall(it->second, {}, "arc.context");
}

void ClassArcEmitter::emitRefcountFailure(const char *name) {
  codegen_.builder_.CreateCall(getOrCreateRefcountFailureFunction(name));
  codegen_.builder_.CreateUnreachable();
}

void ClassArcEmitter::ensureNestedClassArcSupport(
    const std::shared_ptr<zir::Type> &type) {
  if (!type) {
    return;
  }

  switch (type->getKind()) {
  case zir::TypeKind::Class:
    ensureClassArcSupport(std::static_pointer_cast<zir::ClassType>(type));
    return;
  case zir::TypeKind::Record: {
    const auto &record = static_cast<const zir::RecordType &>(*type);
    for (const auto &field : record.getFields()) {
      ensureNestedClassArcSupport(field.type);
    }
    return;
  }
  case zir::TypeKind::Array:
    ensureNestedClassArcSupport(
        static_cast<const zir::ArrayType &>(*type).getBaseType());
    return;
  case zir::TypeKind::TaggedUnion: {
    const auto &taggedUnion = static_cast<const zir::TaggedUnionType &>(*type);
    for (const auto &variant : taggedUnion.getVariants()) {
      ensureNestedClassArcSupport(variant.payloadType);
    }
    return;
  }
  default:
    return;
  }
}

llvm::Function *ClassArcEmitter::emitClassTraceFunction(
    const std::shared_ptr<zir::ClassType> &classType,
    llvm::StructType *objectType) {
  auto existing = codegen_.classTraceFns_.find(classType->getCodegenName());
  if (existing != codegen_.classTraceFns_.end()) {
    return existing->second;
  }

  auto *rawPtrTy = llvm::PointerType::getUnqual(codegen_.ctx_);
  auto *traceTy = llvm::FunctionType::get(
      llvm::Type::getVoidTy(codegen_.ctx_), {rawPtrTy, rawPtrTy, rawPtrTy},
      false);
  auto *traceHelper = llvm::Function::Create(
      traceTy, llvm::Function::InternalLinkage,
      "__zap_arc_trace_" + classType->getCodegenName(), *codegen_.module_);
  codegen_.classTraceFns_[classType->getCodegenName()] = traceHelper;

  auto *savedFunction = codegen_.currentFn_;
  auto *savedBlock = codegen_.builder_.GetInsertBlock();
  codegen_.currentFn_ = traceHelper;
  auto *entry = llvm::BasicBlock::Create(codegen_.ctx_, "entry", traceHelper);
  codegen_.builder_.SetInsertPoint(entry);

  auto argument = traceHelper->arg_begin();
  auto *rawObject = &*argument++;
  rawObject->setName("object.raw");
  auto *visitor = &*argument++;
  visitor->setName("visitor");
  auto *context = &*argument;
  context->setName("context");

  auto *object = codegen_.builder_.CreateBitCast(rawObject, rawPtrTy, "object");
  for (size_t i = 0; i < classType->getFields().size(); ++i) {
    auto *fieldAddr = codegen_.builder_.CreateStructGEP(
        objectType, object, static_cast<unsigned>(i + kClassFieldStartIndex),
        "field.addr");
    emitTraceChildren(classType->getFields()[i].type, fieldAddr, visitor,
                      context);
  }
  codegen_.builder_.CreateRetVoid();

  codegen_.currentFn_ = savedFunction;
  if (savedBlock) {
    codegen_.builder_.SetInsertPoint(savedBlock);
  }
  return traceHelper;
}

void ClassArcEmitter::emitTraceChildren(
    const std::shared_ptr<zir::Type> &type, llvm::Value *address,
    llvm::Value *visitor, llvm::Value *context) {
  if (!type || !address) {
    return;
  }

  if (isClassType(type)) {
    if (isWeakClassType(type)) {
      return;
    }
    auto *child = codegen_.builder_.CreateLoad(codegen_.toLLVMType(*type),
                                                address, "trace.child");
    auto *rawPtrTy = llvm::PointerType::getUnqual(codegen_.ctx_);
    auto *visitorTy = llvm::FunctionType::get(
        llvm::Type::getVoidTy(codegen_.ctx_), {rawPtrTy, rawPtrTy}, false);
    codegen_.builder_.CreateCall(visitorTy, visitor, {context, child});
    return;
  }

  if (type->getKind() == zir::TypeKind::Record) {
    const auto &record = static_cast<const zir::RecordType &>(*type);
    auto *recordTy = llvm::cast<llvm::StructType>(codegen_.toLLVMType(*type));
    for (size_t i = 0; i < record.getFields().size(); ++i) {
      auto *fieldAddr = codegen_.builder_.CreateStructGEP(
          recordTy, address, static_cast<unsigned>(i), "trace.field.addr");
      emitTraceChildren(record.getFields()[i].type, fieldAddr, visitor,
                        context);
    }
    return;
  }

  if (type->getKind() == zir::TypeKind::Array) {
    const auto &array = static_cast<const zir::ArrayType &>(*type);
    auto *arrayTy = llvm::cast<llvm::ArrayType>(codegen_.toLLVMType(*type));
    auto *i32Ty = llvm::Type::getInt32Ty(codegen_.ctx_);
    for (size_t i = 0; i < array.getSize(); ++i) {
      llvm::Value *indices[] = {
          llvm::ConstantInt::get(i32Ty, 0), llvm::ConstantInt::get(i32Ty, i)};
      auto *elementAddr = codegen_.builder_.CreateInBoundsGEP(
          arrayTy, address, indices, "trace.element.addr");
      emitTraceChildren(array.getBaseType(), elementAddr, visitor, context);
    }
    return;
  }

  if (type->getKind() != zir::TypeKind::TaggedUnion) {
    return;
  }

  const auto &taggedUnion = static_cast<const zir::TaggedUnionType &>(*type);
  auto *unionTy = llvm::cast<llvm::StructType>(codegen_.toLLVMType(*type));
  auto *tagAddr =
      codegen_.builder_.CreateStructGEP(unionTy, address, 0, "trace.tag.addr");
  auto *tag = codegen_.builder_.CreateLoad(llvm::Type::getInt32Ty(codegen_.ctx_),
                                            tagAddr, "trace.tag");
  auto *done = llvm::BasicBlock::Create(codegen_.ctx_, "trace.union.done",
                                        codegen_.currentFn_);

  for (const auto &variant : taggedUnion.getVariants()) {
    if (!variant.payloadType) {
      continue;
    }

    auto *active = llvm::BasicBlock::Create(codegen_.ctx_, "trace.union.active",
                                             codegen_.currentFn_);
    auto *next = llvm::BasicBlock::Create(codegen_.ctx_, "trace.union.next",
                                           codegen_.currentFn_);
    auto *matches = codegen_.builder_.CreateICmpEQ(
        tag, llvm::ConstantInt::getSigned(llvm::Type::getInt32Ty(codegen_.ctx_),
                                           variant.tag),
        "trace.union.is_active");
    codegen_.builder_.CreateCondBr(matches, active, next);

    codegen_.builder_.SetInsertPoint(active);
    auto *payloadAddr = codegen_.builder_.CreateStructGEP(
        unionTy, address, 1, "trace.payload.addr");
    emitTraceChildren(variant.payloadType, payloadAddr, visitor, context);
    codegen_.builder_.CreateBr(done);

    codegen_.builder_.SetInsertPoint(next);
  }

  codegen_.builder_.CreateBr(done);
  codegen_.builder_.SetInsertPoint(done);
}

bool ClassArcEmitter::isClassType(
    const std::shared_ptr<zir::Type> &type) const {
  return type && type->getKind() == zir::TypeKind::Class;
}

bool ClassArcEmitter::isWeakClassType(
    const std::shared_ptr<zir::Type> &type) const {
  return isClassType(type) &&
         std::static_pointer_cast<zir::ClassType>(type)->isWeak();
}

void ClassArcEmitter::emitRetainIfNeeded(
    llvm::Value *value, const std::shared_ptr<zir::Type> &type) {
  if (!value || !isClassType(type) || isWeakClassType(type)) {
    return;
  }

  auto *valuePtrTy = llvm::dyn_cast<llvm::PointerType>(value->getType());
  if (!valuePtrTy) {
    return;
  }

  auto *retainBB = llvm::BasicBlock::Create(codegen_.ctx_, "arc.retain.check",
                                            codegen_.currentFn_);
  auto *countBB = llvm::BasicBlock::Create(codegen_.ctx_, "arc.retain.count",
                                            codegen_.currentFn_);
  auto *incrementBB = llvm::BasicBlock::Create(codegen_.ctx_, "arc.retain.do",
                                                codegen_.currentFn_);
  auto *overflowBB = llvm::BasicBlock::Create(codegen_.ctx_,
                                               "arc.retain.overflow",
                                               codegen_.currentFn_);
  auto *deadBB = llvm::BasicBlock::Create(codegen_.ctx_, "arc.retain.dead",
                                           codegen_.currentFn_);
  auto *contBB = llvm::BasicBlock::Create(codegen_.ctx_, "arc.retain.cont",
                                          codegen_.currentFn_);
  auto *isNull = codegen_.builder_.CreateICmpEQ(
      value, llvm::ConstantPointerNull::get(valuePtrTy), "arc.retain.isnull");
  codegen_.builder_.CreateCondBr(isNull, contBB, retainBB);

  codegen_.builder_.SetInsertPoint(retainBB);
  auto classType = std::static_pointer_cast<zir::ClassType>(type);
  auto *objectTy =
      codegen_.structCache_.at(classType->getCodegenName() + ".obj");
  auto *typedPtr = codegen_.builder_.CreateBitCast(
      value, llvm::PointerType::getUnqual(codegen_.ctx_), "arc.retain.cast");
  auto *aliveAddr = codegen_.builder_.CreateStructGEP(
      objectTy, typedPtr, kClassAliveIndex, "arc.retain.alive.addr");
  auto *alive = codegen_.builder_.CreateLoad(
      llvm::Type::getInt8Ty(codegen_.ctx_), aliveAddr, "arc.retain.alive");
  auto *isDead = codegen_.builder_.CreateICmpEQ(
      alive,
      llvm::ConstantInt::get(llvm::Type::getInt8Ty(codegen_.ctx_), 0));
  codegen_.builder_.CreateCondBr(isDead, deadBB, countBB);

  codegen_.builder_.SetInsertPoint(deadBB);
  emitRefcountFailure("zap_arc_retain_dead_object");

  codegen_.builder_.SetInsertPoint(countBB);
  auto *countAddr = codegen_.builder_.CreateStructGEP(
      objectTy, typedPtr, kClassStrongCountIndex, "arc.retain.count.addr");
  auto *count = codegen_.builder_.CreateLoad(
      llvm::Type::getInt64Ty(codegen_.ctx_), countAddr, "arc.retain.count");
  auto *maximum = llvm::ConstantInt::getSigned(
      llvm::Type::getInt64Ty(codegen_.ctx_), INT64_MAX);
  auto *isOverflow = codegen_.builder_.CreateICmpSGE(
      count, maximum, "arc.retain.overflowed");
  codegen_.builder_.CreateCondBr(isOverflow, overflowBB, incrementBB);

  codegen_.builder_.SetInsertPoint(overflowBB);
  emitRefcountFailure("zap_arc_strong_refcount_overflow");

  codegen_.builder_.SetInsertPoint(incrementBB);
  auto *next = codegen_.builder_.CreateAdd(
      count, llvm::ConstantInt::get(llvm::Type::getInt64Ty(codegen_.ctx_), 1),
      "arc.retain.next");
  codegen_.builder_.CreateStore(next, countAddr);
#if defined(ZAP_RUNTIME_INSTRUMENTATION)
  codegen_.emitRuntimeOwnershipEvent(
      "zap_runtime_ownership_note_strong_retain");
#endif
  codegen_.builder_.CreateBr(contBB);
  codegen_.builder_.SetInsertPoint(contBB);
}

void ClassArcEmitter::emitReleaseIfNeeded(
    llvm::Value *value, const std::shared_ptr<zir::Type> &type) {
  if (!value || !isClassType(type) || isWeakClassType(type)) {
    return;
  }

  auto *valuePtrTy = llvm::dyn_cast<llvm::PointerType>(value->getType());
  if (!valuePtrTy) {
    return;
  }

  auto *releaseBB = llvm::BasicBlock::Create(codegen_.ctx_, "arc.release.do",
                                             codegen_.currentFn_);
  auto *contBB = llvm::BasicBlock::Create(codegen_.ctx_, "arc.release.cont",
                                          codegen_.currentFn_);
  auto *isNull = codegen_.builder_.CreateICmpEQ(
      value, llvm::ConstantPointerNull::get(valuePtrTy), "arc.release.isnull");
  codegen_.builder_.CreateCondBr(isNull, contBB, releaseBB);

  codegen_.builder_.SetInsertPoint(releaseBB);
  auto classType = std::static_pointer_cast<zir::ClassType>(type);
  auto *objectTy =
      codegen_.structCache_.at(classType->getCodegenName() + ".obj");
  auto *typedPtr = codegen_.builder_.CreateBitCast(
      value, llvm::PointerType::getUnqual(codegen_.ctx_), "arc.release.cast");
  auto *gcMarkAddr = codegen_.builder_.CreateStructGEP(
      objectTy, typedPtr, kClassGcMarkIndex, "arc.release.gcmark.addr");
  auto *gcMark = codegen_.builder_.CreateLoad(
      llvm::Type::getInt8Ty(codegen_.ctx_), gcMarkAddr, "arc.release.gcmark");
  auto *callBB = llvm::BasicBlock::Create(codegen_.ctx_, "arc.release.call",
                                          codegen_.currentFn_);
  auto *skipBB = llvm::BasicBlock::Create(codegen_.ctx_, "arc.release.skip",
                                          codegen_.currentFn_);
  auto *garbageBit = codegen_.builder_.CreateAnd(
      gcMark, llvm::ConstantInt::get(llvm::Type::getInt8Ty(codegen_.ctx_),
                                     kClassGcGarbageMask));
  auto *isMarked = codegen_.builder_.CreateICmpNE(
      garbageBit,
      llvm::ConstantInt::get(llvm::Type::getInt8Ty(codegen_.ctx_), 0));
  codegen_.builder_.CreateCondBr(isMarked, skipBB, callBB);

  codegen_.builder_.SetInsertPoint(callBB);
  auto *releaseAddr = codegen_.builder_.CreateStructGEP(
      objectTy, typedPtr, kClassReleaseFnIndex, "arc.release.fn.addr");
  auto *releaseFn = codegen_.builder_.CreateLoad(
      llvm::PointerType::getUnqual(codegen_.ctx_),
      releaseAddr, "arc.release.fn");
  auto *rawObject = codegen_.builder_.CreateBitCast(
      typedPtr,
      llvm::PointerType::getUnqual(codegen_.ctx_));
  auto *releaseTy = llvm::FunctionType::get(
      llvm::Type::getVoidTy(codegen_.ctx_),
      {llvm::PointerType::getUnqual(codegen_.ctx_)},
      false);
  codegen_.builder_.CreateCall(releaseTy, releaseFn, {rawObject});
  codegen_.builder_.CreateBr(contBB);
  codegen_.builder_.SetInsertPoint(skipBB);
  codegen_.builder_.CreateBr(contBB);
  codegen_.builder_.SetInsertPoint(contBB);
}

void ClassArcEmitter::emitRetainWeakIfNeeded(
    llvm::Value *value, const std::shared_ptr<zir::Type> &type) {
  if (!value || !isWeakClassType(type)) {
    return;
  }

  auto *valuePtrTy = llvm::dyn_cast<llvm::PointerType>(value->getType());
  if (!valuePtrTy) {
    return;
  }

  auto *retainBB = llvm::BasicBlock::Create(codegen_.ctx_,
                                            "arc.weak.retain.check",
                                            codegen_.currentFn_);
  auto *incrementBB = llvm::BasicBlock::Create(codegen_.ctx_,
                                                "arc.weak.retain.do",
                                                codegen_.currentFn_);
  auto *overflowBB = llvm::BasicBlock::Create(codegen_.ctx_,
                                               "arc.weak.retain.overflow",
                                               codegen_.currentFn_);
  auto *contBB = llvm::BasicBlock::Create(codegen_.ctx_, "arc.weak.retain.cont",
                                          codegen_.currentFn_);
  auto *isNull = codegen_.builder_.CreateICmpEQ(
      value, llvm::ConstantPointerNull::get(valuePtrTy),
      "arc.weak.retain.isnull");
  codegen_.builder_.CreateCondBr(isNull, contBB, retainBB);

  codegen_.builder_.SetInsertPoint(retainBB);
  auto classType = std::static_pointer_cast<zir::ClassType>(type);
  auto *objectTy =
      codegen_.structCache_.at(classType->getCodegenName() + ".obj");
  auto *typedPtr = codegen_.builder_.CreateBitCast(
      value, llvm::PointerType::getUnqual(codegen_.ctx_), "arc.weak.retain.cast");
  auto *countAddr = codegen_.builder_.CreateStructGEP(
      objectTy, typedPtr, kClassWeakCountIndex, "arc.weak.count.addr");
  auto *count = codegen_.builder_.CreateLoad(
      llvm::Type::getInt64Ty(codegen_.ctx_), countAddr, "arc.weak.count");
  auto *maximum = llvm::ConstantInt::getSigned(
      llvm::Type::getInt64Ty(codegen_.ctx_), INT64_MAX);
  auto *isOverflow = codegen_.builder_.CreateICmpSGE(
      count, maximum, "arc.weak.retain.overflowed");
  codegen_.builder_.CreateCondBr(isOverflow, overflowBB, incrementBB);

  codegen_.builder_.SetInsertPoint(overflowBB);
  emitRefcountFailure("zap_arc_weak_refcount_overflow");

  codegen_.builder_.SetInsertPoint(incrementBB);
  auto *next = codegen_.builder_.CreateAdd(
      count, llvm::ConstantInt::get(llvm::Type::getInt64Ty(codegen_.ctx_), 1),
      "arc.weak.next");
  codegen_.builder_.CreateStore(next, countAddr);
  codegen_.builder_.CreateBr(contBB);
  codegen_.builder_.SetInsertPoint(contBB);
}

void ClassArcEmitter::emitReleaseWeakIfNeeded(
    llvm::Value *value, const std::shared_ptr<zir::Type> &type) {
  if (!value || !isWeakClassType(type)) {
    return;
  }

  auto *valuePtrTy = llvm::dyn_cast<llvm::PointerType>(value->getType());
  if (!valuePtrTy) {
    return;
  }

  auto *releaseBB = llvm::BasicBlock::Create(
      codegen_.ctx_, "arc.weak.release.check", codegen_.currentFn_);
  auto *decrementBB = llvm::BasicBlock::Create(
      codegen_.ctx_, "arc.weak.release.do", codegen_.currentFn_);
  auto *underflowBB = llvm::BasicBlock::Create(
      codegen_.ctx_, "arc.weak.release.underflow", codegen_.currentFn_);
  auto *checkDeadBB = llvm::BasicBlock::Create(
      codegen_.ctx_, "arc.weak.release.checkdead", codegen_.currentFn_);
  auto *freeBB = llvm::BasicBlock::Create(
      codegen_.ctx_, "arc.weak.release.free", codegen_.currentFn_);
  auto *contBB = llvm::BasicBlock::Create(
      codegen_.ctx_, "arc.weak.release.cont", codegen_.currentFn_);
  auto *isNull = codegen_.builder_.CreateICmpEQ(
      value, llvm::ConstantPointerNull::get(valuePtrTy),
      "arc.weak.release.isnull");
  codegen_.builder_.CreateCondBr(isNull, contBB, releaseBB);

  codegen_.builder_.SetInsertPoint(releaseBB);
  auto classType = std::static_pointer_cast<zir::ClassType>(type);
  auto *objectTy =
      codegen_.structCache_.at(classType->getCodegenName() + ".obj");
  auto *typedPtr = codegen_.builder_.CreateBitCast(
      value, llvm::PointerType::getUnqual(codegen_.ctx_), "arc.weak.release.cast");
  auto *weakAddr = codegen_.builder_.CreateStructGEP(
      objectTy, typedPtr, kClassWeakCountIndex, "arc.weak.release.count.addr");
  auto *weakCount =
      codegen_.builder_.CreateLoad(llvm::Type::getInt64Ty(codegen_.ctx_),
                                   weakAddr, "arc.weak.release.count");
  auto *isUnderflow = codegen_.builder_.CreateICmpSLE(
      weakCount, llvm::ConstantInt::get(llvm::Type::getInt64Ty(codegen_.ctx_),
                                         0),
      "arc.weak.release.underflowed");
  codegen_.builder_.CreateCondBr(isUnderflow, underflowBB, decrementBB);

  codegen_.builder_.SetInsertPoint(underflowBB);
  emitRefcountFailure("zap_arc_weak_refcount_underflow");

  codegen_.builder_.SetInsertPoint(decrementBB);
  auto *nextWeak = codegen_.builder_.CreateSub(
      weakCount,
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(codegen_.ctx_), 1),
      "arc.weak.release.next");
  codegen_.builder_.CreateStore(nextWeak, weakAddr);
  auto *isWeakZero = codegen_.builder_.CreateICmpEQ(
      nextWeak,
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(codegen_.ctx_), 0));
  codegen_.builder_.CreateCondBr(isWeakZero, checkDeadBB, contBB);

  codegen_.builder_.SetInsertPoint(checkDeadBB);
  auto *aliveAddr = codegen_.builder_.CreateStructGEP(
      objectTy, typedPtr, kClassAliveIndex, "arc.weak.release.alive.addr");
  auto *alive = codegen_.builder_.CreateLoad(
      llvm::Type::getInt8Ty(codegen_.ctx_), aliveAddr,
      "arc.weak.release.alive");
  auto *isDead = codegen_.builder_.CreateICmpEQ(
      alive,
      llvm::ConstantInt::get(llvm::Type::getInt8Ty(codegen_.ctx_), 0));
  auto *gcMarkAddr = codegen_.builder_.CreateStructGEP(
      objectTy, typedPtr, kClassGcMarkIndex, "arc.weak.release.gcmark.addr");
  auto *gcMark = codegen_.builder_.CreateLoad(
      llvm::Type::getInt8Ty(codegen_.ctx_), gcMarkAddr,
      "arc.weak.release.gcmark");
  auto *protectedBits = codegen_.builder_.CreateAnd(
      gcMark, llvm::ConstantInt::get(llvm::Type::getInt8Ty(codegen_.ctx_),
                                     kClassGcGarbageMask |
                                         kClassGcFinalizingMask));
  auto *isNotFinalizing = codegen_.builder_.CreateICmpEQ(
      protectedBits,
      llvm::ConstantInt::get(llvm::Type::getInt8Ty(codegen_.ctx_), 0));
  auto *canDeallocate = codegen_.builder_.CreateAnd(isDead, isNotFinalizing);
  codegen_.builder_.CreateCondBr(canDeallocate, freeBB, contBB);

  codegen_.builder_.SetInsertPoint(freeBB);
  auto *rawObject = codegen_.builder_.CreateBitCast(
      typedPtr,
      llvm::PointerType::getUnqual(codegen_.ctx_));
  codegen_.builder_.CreateCall(getOrCreateArcDeallocateFunction(),
                               {emitArcRuntimeContext(), rawObject});
  codegen_.builder_.CreateBr(contBB);

  codegen_.builder_.SetInsertPoint(contBB);
}

llvm::Value *
ClassArcEmitter::emitWeakAlive(llvm::Value *value,
                               const std::shared_ptr<zir::Type> &type) {
  if (!value || !isWeakClassType(type)) {
    return llvm::ConstantInt::get(llvm::Type::getInt1Ty(codegen_.ctx_), 0);
  }

  auto classType = std::static_pointer_cast<zir::ClassType>(type);
  auto *objectTy =
      codegen_.structCache_.at(classType->getCodegenName() + ".obj");
  auto *valuePtrTy = llvm::dyn_cast<llvm::PointerType>(value->getType());
  if (!valuePtrTy) {
    return llvm::ConstantInt::get(llvm::Type::getInt1Ty(codegen_.ctx_), 0);
  }

  auto *nonNullBB = llvm::BasicBlock::Create(
      codegen_.ctx_, "arc.weak.alive.nonnull", codegen_.currentFn_);
  auto *mergeBB = llvm::BasicBlock::Create(
      codegen_.ctx_, "arc.weak.alive.merge", codegen_.currentFn_);
  auto *isNull = codegen_.builder_.CreateICmpEQ(
      value, llvm::ConstantPointerNull::get(valuePtrTy),
      "arc.weak.alive.isnull");
  auto *entryBB = codegen_.builder_.GetInsertBlock();
  codegen_.builder_.CreateCondBr(isNull, mergeBB, nonNullBB);

  codegen_.builder_.SetInsertPoint(nonNullBB);
  auto *typedPtr = codegen_.builder_.CreateBitCast(
      value, llvm::PointerType::getUnqual(codegen_.ctx_), "arc.weak.alive.cast");
  auto *aliveAddr = codegen_.builder_.CreateStructGEP(
      objectTy, typedPtr, kClassAliveIndex, "arc.weak.alive.addr");
  auto *aliveValue = codegen_.builder_.CreateLoad(
      llvm::Type::getInt8Ty(codegen_.ctx_), aliveAddr, "arc.weak.alive");
  auto *aliveBool = codegen_.builder_.CreateICmpNE(
      aliveValue,
      llvm::ConstantInt::get(llvm::Type::getInt8Ty(codegen_.ctx_), 0));
  auto *aliveBB = codegen_.builder_.GetInsertBlock();
  codegen_.builder_.CreateBr(mergeBB);

  codegen_.builder_.SetInsertPoint(mergeBB);
  auto *phi = codegen_.builder_.CreatePHI(llvm::Type::getInt1Ty(codegen_.ctx_),
                                          2, "arc.weak.alive.res");
  phi->addIncoming(
      llvm::ConstantInt::get(llvm::Type::getInt1Ty(codegen_.ctx_), 0), entryBB);
  phi->addIncoming(aliveBool, aliveBB);
  return phi;
}

llvm::Value *
ClassArcEmitter::emitWeakLock(llvm::Value *value,
                              const std::shared_ptr<zir::Type> &type) {
  if (!value || !isWeakClassType(type)) {
    return llvm::ConstantPointerNull::get(
        llvm::cast<llvm::PointerType>(codegen_.toLLVMType(*type)));
  }

  auto weakClassType = std::static_pointer_cast<zir::ClassType>(type);
  auto strongClassType = std::make_shared<zir::ClassType>(*weakClassType);
  strongClassType->setWeak(false);
  auto *resultTy =
      llvm::cast<llvm::PointerType>(codegen_.toLLVMType(*strongClassType));
  auto *objectTy =
      codegen_.structCache_.at(weakClassType->getCodegenName() + ".obj");
  auto *valuePtrTy = llvm::dyn_cast<llvm::PointerType>(value->getType());
  if (!valuePtrTy) {
    return llvm::ConstantPointerNull::get(resultTy);
  }

  auto *aliveCheckBB = llvm::BasicBlock::Create(
      codegen_.ctx_, "arc.weak.lock.check", codegen_.currentFn_);
  auto *retainBB = llvm::BasicBlock::Create(
      codegen_.ctx_, "arc.weak.lock.retain", codegen_.currentFn_);
  auto *mergeBB = llvm::BasicBlock::Create(codegen_.ctx_, "arc.weak.lock.merge",
                                           codegen_.currentFn_);
  auto *isNull = codegen_.builder_.CreateICmpEQ(
      value, llvm::ConstantPointerNull::get(valuePtrTy),
      "arc.weak.lock.isnull");
  auto *entryBB = codegen_.builder_.GetInsertBlock();
  codegen_.builder_.CreateCondBr(isNull, mergeBB, aliveCheckBB);

  codegen_.builder_.SetInsertPoint(aliveCheckBB);
  auto *typedPtr = codegen_.builder_.CreateBitCast(
      value, llvm::PointerType::getUnqual(codegen_.ctx_), "arc.weak.lock.cast");
  auto *aliveAddr = codegen_.builder_.CreateStructGEP(
      objectTy, typedPtr, kClassAliveIndex, "arc.weak.lock.alive.addr");
  auto *aliveValue = codegen_.builder_.CreateLoad(
      llvm::Type::getInt8Ty(codegen_.ctx_), aliveAddr, "arc.weak.lock.alive");
  auto *aliveBool = codegen_.builder_.CreateICmpNE(
      aliveValue,
      llvm::ConstantInt::get(llvm::Type::getInt8Ty(codegen_.ctx_), 0));
  auto *aliveCheckBlock = codegen_.builder_.GetInsertBlock();
  codegen_.builder_.CreateCondBr(aliveBool, retainBB, mergeBB);

  codegen_.builder_.SetInsertPoint(retainBB);
  auto *strongValue =
      codegen_.builder_.CreateBitCast(value, resultTy, "arc.weak.lock.value");
  emitRetainIfNeeded(strongValue, strongClassType);
  auto *retainBlock = codegen_.builder_.GetInsertBlock();
  codegen_.builder_.CreateBr(mergeBB);

  codegen_.builder_.SetInsertPoint(mergeBB);
  auto *phi = codegen_.builder_.CreatePHI(resultTy, 3, "arc.weak.lock.res");
  phi->addIncoming(llvm::ConstantPointerNull::get(resultTy), entryBB);
  phi->addIncoming(llvm::ConstantPointerNull::get(resultTy), aliveCheckBlock);
  phi->addIncoming(strongValue, retainBlock);
  return phi;
}

void ClassArcEmitter::emitStoreWithArc(llvm::Value *addr, llvm::Value *value,
                                       const std::shared_ptr<zir::Type> &type,
                                       zir::ValueOwnership valueOwnership,
                                       bool skipReleaseOld) {
  if (!isClassType(type)) {
    codegen_.builder_.CreateStore(value, addr);
    return;
  }

  if (isWeakClassType(type)) {
    if (valueOwnership != zir::ValueOwnership::OwnedWeak) {
      emitRetainWeakIfNeeded(value, type);
    }
    if (!skipReleaseOld) {
      auto *oldValue = codegen_.builder_.CreateLoad(codegen_.toLLVMType(*type),
                                                    addr, "arc.weak.store.old");
      codegen_.builder_.CreateStore(value, addr);
      emitReleaseWeakIfNeeded(oldValue, type);
    } else {
      codegen_.builder_.CreateStore(value, addr);
    }
    if (valueOwnership == zir::ValueOwnership::OwnedStrong) {
      auto strongType = std::make_shared<zir::ClassType>(
          *std::static_pointer_cast<zir::ClassType>(type));
      strongType->setWeak(false);
      emitReleaseIfNeeded(value, strongType);
    }
    return;
  }

  if (valueOwnership != zir::ValueOwnership::OwnedStrong) {
    emitRetainIfNeeded(value, type);
  }
  if (!skipReleaseOld) {
    auto *oldValue = codegen_.builder_.CreateLoad(codegen_.toLLVMType(*type),
                                                  addr, "arc.store.old");
    codegen_.builder_.CreateStore(value, addr);
    emitReleaseIfNeeded(oldValue, type);
  } else {
    codegen_.builder_.CreateStore(value, addr);
  }
}

void ClassArcEmitter::ensureClassArcSupport(
    const std::shared_ptr<zir::ClassType> &classType) {
  if (!classType || classType->isInterface() ||
      codegen_.classReleaseFns_.count(classType->getCodegenName())) {
    return;
  }

  codegen_.toLLVMType(*classType);
  auto *objectTy =
      codegen_.structCache_.at(classType->getCodegenName() + ".obj");
  auto *rawPtrTy =
      llvm::PointerType::getUnqual(codegen_.ctx_);
  auto *helperTy = llvm::FunctionType::get(llvm::Type::getVoidTy(codegen_.ctx_),
                                           {rawPtrTy}, false);
  auto *releaseHelper = llvm::Function::Create(
      helperTy, llvm::Function::InternalLinkage,
      "__zap_arc_release_" + classType->getCodegenName(), *codegen_.module_);
  auto *destroyHelper = llvm::Function::Create(
      helperTy, llvm::Function::InternalLinkage,
      "__zap_arc_destroy_" + classType->getCodegenName(), *codegen_.module_);
  codegen_.classReleaseFns_[classType->getCodegenName()] = releaseHelper;
  codegen_.classDestroyFns_[classType->getCodegenName()] = destroyHelper;

  for (const auto &field : classType->getFields()) {
    ensureNestedClassArcSupport(field.type);
  }

  if (!codegen_.classVTables_.count(classType->getCodegenName())) {
    std::vector<llvm::Constant *> entries;
    if (auto base = classType->getBase()) {
      ensureClassArcSupport(base);
      auto *baseVTable = codegen_.classVTables_.at(base->getCodegenName());
      if (auto *baseInit = baseVTable->getInitializer()) {
        for (unsigned i = 0; i < baseInit->getNumOperands(); ++i) {
          entries.push_back(
              llvm::cast<llvm::Constant>(baseInit->getOperand(i)));
        }
      }
    }

    auto methodsIt =
        codegen_.classVirtualMethodFns_.find(classType->getCodegenName());
    if (methodsIt != codegen_.classVirtualMethodFns_.end()) {
      auto *i8PtrTy =
          llvm::PointerType::getUnqual(codegen_.ctx_);
      for (const auto &[slot, fn] : methodsIt->second) {
        if (slot >= static_cast<int>(entries.size())) {
          entries.resize(static_cast<size_t>(slot + 1),
                         llvm::ConstantPointerNull::get(i8PtrTy));
        }
        entries[slot] = llvm::ConstantExpr::getBitCast(fn, i8PtrTy);
      }
    }

    auto *i8PtrTy =
        llvm::PointerType::getUnqual(codegen_.ctx_);
    auto *vtableTy =
        llvm::ArrayType::get(i8PtrTy, static_cast<uint64_t>(entries.size()));
    auto *init = llvm::ConstantArray::get(vtableTy, entries);
    auto *gv = new llvm::GlobalVariable(
        *codegen_.module_, vtableTy, true, llvm::GlobalValue::InternalLinkage,
        init, "__zap_vtable_" + classType->getCodegenName());
    codegen_.classVTables_[classType->getCodegenName()] = gv;
  }

  if (!classType->getInterfaceConformances().empty() &&
      !codegen_.classInterfaceTables_.count(classType->getCodegenName())) {
    auto *i8PtrTy = llvm::PointerType::getUnqual(codegen_.ctx_);
    auto *i64Ty = llvm::Type::getInt64Ty(codegen_.ctx_);
    auto *entryTy = llvm::StructType::get(codegen_.ctx_, {i8PtrTy, i8PtrTy});

    std::vector<llvm::Constant *> entries;
    for (const auto &conformance : classType->getInterfaceConformances()) {
      std::vector<llvm::Constant *> slotConsts;
      slotConsts.reserve(conformance.methodVtableSlots.size());
      for (int slot : conformance.methodVtableSlots) {
        slotConsts.push_back(
            llvm::ConstantInt::get(i64Ty, static_cast<uint64_t>(slot)));
      }
      auto *slotsArrTy =
          llvm::ArrayType::get(i64Ty, slotConsts.size());
      auto *slotsInit = llvm::ConstantArray::get(slotsArrTy, slotConsts);
      auto *slotsGlobal = new llvm::GlobalVariable(
          *codegen_.module_, slotsArrTy, true,
          llvm::GlobalValue::InternalLinkage, slotsInit,
          "__zap_iface_slots_" + classType->getCodegenName() + "_" +
              conformance.interfaceCodegenName);

      auto *nameConst = llvm::ConstantDataArray::getString(
          codegen_.ctx_, conformance.interfaceCodegenName, true);
      auto *nameGlobal = new llvm::GlobalVariable(
          *codegen_.module_, nameConst->getType(), true,
          llvm::GlobalValue::InternalLinkage, nameConst,
          "__zap_iface_name_" + conformance.interfaceCodegenName);

      llvm::Constant *fields[] = {
          llvm::ConstantExpr::getBitCast(nameGlobal, i8PtrTy),
          llvm::ConstantExpr::getBitCast(slotsGlobal, i8PtrTy)};
      entries.push_back(llvm::ConstantStruct::get(entryTy, fields));
    }
    llvm::Constant *sentinelFields[] = {
        llvm::ConstantPointerNull::get(i8PtrTy),
        llvm::ConstantPointerNull::get(i8PtrTy)};
    entries.push_back(llvm::ConstantStruct::get(entryTy, sentinelFields));

    auto *tableTy = llvm::ArrayType::get(entryTy, entries.size());
    auto *tableInit = llvm::ConstantArray::get(tableTy, entries);
    auto *tableGlobal = new llvm::GlobalVariable(
        *codegen_.module_, tableTy, true, llvm::GlobalValue::InternalLinkage,
        tableInit, "__zap_iface_table_" + classType->getCodegenName());
    codegen_.classInterfaceTables_[classType->getCodegenName()] = tableGlobal;
  }

  if (!codegen_.classMetadataGlobals_.count(classType->getCodegenName())) {
    auto *traceHelper = emitClassTraceFunction(classType, objectTy);
    auto *i32PtrTy = llvm::PointerType::getUnqual(codegen_.ctx_);
    auto *metaTy = llvm::StructType::create(
        codegen_.ctx_, "__zap_arc_meta_" + classType->getCodegenName());
    metaTy->setBody({i32PtrTy});
    auto *metaInit = llvm::ConstantStruct::get(
        metaTy, llvm::ConstantExpr::getBitCast(traceHelper, i32PtrTy));
    auto *metaGlobal = new llvm::GlobalVariable(
        *codegen_.module_, metaTy, true, llvm::GlobalValue::InternalLinkage,
        metaInit, "__zap_arc_meta_" + classType->getCodegenName());
    codegen_.classMetadataGlobals_[classType->getCodegenName()] = metaGlobal;
  }

  auto savedFn = codegen_.currentFn_;
  auto savedBlock = codegen_.builder_.GetInsertBlock();

  codegen_.currentFn_ = destroyHelper;
  auto *destroyEntry =
      llvm::BasicBlock::Create(codegen_.ctx_, "entry", destroyHelper);
  auto *destroyReturnBB =
      llvm::BasicBlock::Create(codegen_.ctx_, "arc.ret", destroyHelper);
  codegen_.builder_.SetInsertPoint(destroyEntry);

  auto *destroyRawObject = &*destroyHelper->arg_begin();
  destroyRawObject->setName("object.raw");
  auto *destroyIsNull = codegen_.builder_.CreateICmpEQ(
      destroyRawObject,
      llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(rawPtrTy)));
  auto *destroyBodyBB =
      llvm::BasicBlock::Create(codegen_.ctx_, "arc.destroy", destroyHelper);
  codegen_.builder_.CreateCondBr(destroyIsNull, destroyReturnBB, destroyBodyBB);

  codegen_.builder_.SetInsertPoint(destroyBodyBB);
#if defined(ZAP_RUNTIME_INSTRUMENTATION)
  codegen_.emitRuntimeOwnershipEvent("zap_runtime_ownership_note_destroy");
#endif
  auto *destroyTypedObject = codegen_.builder_.CreateBitCast(
      destroyRawObject, llvm::PointerType::getUnqual(codegen_.ctx_), "object");
  auto *aliveAddr = codegen_.builder_.CreateStructGEP(
      objectTy, destroyTypedObject, kClassAliveIndex, "alive.addr");
  codegen_.builder_.CreateStore(
      llvm::ConstantInt::get(llvm::Type::getInt8Ty(codegen_.ctx_), 0),
      aliveAddr);
  auto *gcMarkAddr = codegen_.builder_.CreateStructGEP(
      objectTy, destroyTypedObject, kClassGcMarkIndex, "gcmark.addr");
  auto *gcMark = codegen_.builder_.CreateLoad(
      llvm::Type::getInt8Ty(codegen_.ctx_), gcMarkAddr, "gcmark");
  auto *finalizingMark = codegen_.builder_.CreateOr(
      gcMark, llvm::ConstantInt::get(llvm::Type::getInt8Ty(codegen_.ctx_),
                                     kClassGcFinalizingMask));
  codegen_.builder_.CreateStore(finalizingMark, gcMarkAddr);
  auto dtorIt =
      codegen_.classDestructorFns_.find(classType->getCodegenName());
  if (dtorIt != codegen_.classDestructorFns_.end()) {
    auto *dtorPtr = codegen_.builder_.CreateBitCast(
        destroyTypedObject, dtorIt->second->getArg(0)->getType(), "dtor.self");
    codegen_.builder_.CreateCall(dtorIt->second, {dtorPtr});
  }

  for (size_t i = 0; i < classType->getFields().size(); ++i) {
    const auto &field = classType->getFields()[i];
    if (!field.type || !codegen_.containsManagedValues(field.type)) {
      continue;
    }
    auto *fieldAddr = codegen_.builder_.CreateStructGEP(
        objectTy, destroyTypedObject,
        static_cast<unsigned>(i + kClassFieldStartIndex));
    auto *fieldValue = codegen_.builder_.CreateLoad(
        codegen_.toLLVMType(*field.type), fieldAddr, field.name);
    codegen_.emitManagedRelease(fieldValue, field.type);
  }

  auto *markAfterDrop = codegen_.builder_.CreateLoad(
      llvm::Type::getInt8Ty(codegen_.ctx_), gcMarkAddr, "gcmark.after.drop");
  auto *finalizedMark = codegen_.builder_.CreateAnd(
      markAfterDrop,
      llvm::ConstantInt::get(llvm::Type::getInt8Ty(codegen_.ctx_),
                             static_cast<uint8_t>(~kClassGcFinalizingMask)));
  codegen_.builder_.CreateStore(finalizedMark, gcMarkAddr);
  codegen_.builder_.CreateBr(destroyReturnBB);

  codegen_.builder_.SetInsertPoint(destroyReturnBB);
  codegen_.builder_.CreateRetVoid();

  codegen_.currentFn_ = releaseHelper;
  auto *entry = llvm::BasicBlock::Create(codegen_.ctx_, "entry", releaseHelper);
  auto *decrementedBB =
      llvm::BasicBlock::Create(codegen_.ctx_, "arc.dec.check", releaseHelper);
  auto *validReleaseBB =
      llvm::BasicBlock::Create(codegen_.ctx_, "arc.dec", releaseHelper);
  auto *underflowBB = llvm::BasicBlock::Create(codegen_.ctx_,
                                                "arc.dec.underflow",
                                                releaseHelper);
  auto *destroyBB =
      llvm::BasicBlock::Create(codegen_.ctx_, "arc.destroy", releaseHelper);
  auto *deallocateBB =
      llvm::BasicBlock::Create(codegen_.ctx_, "arc.deallocate", releaseHelper);
  bool canBeCyclic =
      codegen_.cyclicClasses_.count(classType->getCodegenName()) != 0;
  auto *cycleBB =
      canBeCyclic
          ? llvm::BasicBlock::Create(codegen_.ctx_, "arc.cycle", releaseHelper)
          : nullptr;
  auto *returnBB =
      llvm::BasicBlock::Create(codegen_.ctx_, "arc.ret", releaseHelper);
  codegen_.builder_.SetInsertPoint(entry);

  auto *rawObject = &*releaseHelper->arg_begin();
  rawObject->setName("object.raw");
  auto *isNull = codegen_.builder_.CreateICmpEQ(
      rawObject,
      llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(rawPtrTy)));
  codegen_.builder_.CreateCondBr(isNull, returnBB, decrementedBB);

  codegen_.builder_.SetInsertPoint(decrementedBB);
  auto *typedObject = codegen_.builder_.CreateBitCast(
      rawObject, llvm::PointerType::getUnqual(codegen_.ctx_), "object");
  auto *countAddr = codegen_.builder_.CreateStructGEP(
      objectTy, typedObject, kClassStrongCountIndex, "refcount.addr");
  auto *count = codegen_.builder_.CreateLoad(
      llvm::Type::getInt64Ty(codegen_.ctx_), countAddr, "refcount");
  auto *isUnderflow = codegen_.builder_.CreateICmpSLE(
      count, llvm::ConstantInt::get(llvm::Type::getInt64Ty(codegen_.ctx_), 0),
      "refcount.underflowed");
  codegen_.builder_.CreateCondBr(isUnderflow, underflowBB, validReleaseBB);

  codegen_.builder_.SetInsertPoint(underflowBB);
  emitRefcountFailure("zap_arc_strong_refcount_underflow");

  codegen_.builder_.SetInsertPoint(validReleaseBB);
  auto *nextCount = codegen_.builder_.CreateSub(
      count, llvm::ConstantInt::get(llvm::Type::getInt64Ty(codegen_.ctx_), 1),
      "refcount.next");
  codegen_.builder_.CreateStore(nextCount, countAddr);
#if defined(ZAP_RUNTIME_INSTRUMENTATION)
  codegen_.emitRuntimeOwnershipEvent(
      "zap_runtime_ownership_note_strong_release");
#endif
  auto *isZero = codegen_.builder_.CreateICmpEQ(
      nextCount,
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(codegen_.ctx_), 0));
  codegen_.builder_.CreateCondBr(isZero, destroyBB,
                                 canBeCyclic ? cycleBB : returnBB);

  codegen_.builder_.SetInsertPoint(destroyBB);
  if (codegen_.functionMap_.count("zap_arc_remove_possible_root") == 0) {
    auto *removeTy = llvm::FunctionType::get(
        llvm::Type::getVoidTy(codegen_.ctx_), {rawPtrTy, rawPtrTy}, false);
    auto *removeFn = llvm::Function::Create(
        removeTy, llvm::Function::ExternalLinkage,
        "zap_arc_remove_possible_root", *codegen_.module_);
    codegen_.functionMap_["zap_arc_remove_possible_root"] = removeFn;
  }
  codegen_.builder_.CreateCall(
      codegen_.functionMap_.at("zap_arc_remove_possible_root"),
      {emitArcRuntimeContext(), rawObject});
  auto *destroyAddr = codegen_.builder_.CreateStructGEP(
      objectTy, typedObject, kClassDestroyFnIndex, "destroy.addr");
  auto *destroyFn = codegen_.builder_.CreateLoad(
      llvm::PointerType::getUnqual(codegen_.ctx_),
      destroyAddr, "destroy.fn");
  auto *destroyTy = llvm::FunctionType::get(
      llvm::Type::getVoidTy(codegen_.ctx_), {rawPtrTy}, false);
  codegen_.builder_.CreateCall(destroyTy, destroyFn, {rawObject});
  auto *weakAddr = codegen_.builder_.CreateStructGEP(
      objectTy, typedObject, kClassWeakCountIndex, "weakcount.addr");
  auto *weakCount = codegen_.builder_.CreateLoad(
      llvm::Type::getInt64Ty(codegen_.ctx_), weakAddr, "weakcount");
  auto *isWeakZero = codegen_.builder_.CreateICmpEQ(
      weakCount,
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(codegen_.ctx_), 0));
  codegen_.builder_.CreateCondBr(isWeakZero, deallocateBB, returnBB);

  codegen_.builder_.SetInsertPoint(deallocateBB);
  codegen_.builder_.CreateCall(getOrCreateArcDeallocateFunction(),
                               {emitArcRuntimeContext(), rawObject});
  codegen_.builder_.CreateBr(returnBB);

  if (canBeCyclic) {
    codegen_.builder_.SetInsertPoint(cycleBB);
    if (codegen_.functionMap_.count("zap_arc_add_possible_root") == 0) {
      auto *addTy = llvm::FunctionType::get(
          llvm::Type::getVoidTy(codegen_.ctx_), {rawPtrTy, rawPtrTy}, false);
      auto *addFn = llvm::Function::Create(
          addTy, llvm::Function::ExternalLinkage, "zap_arc_add_possible_root",
          *codegen_.module_);
      codegen_.functionMap_["zap_arc_add_possible_root"] = addFn;
    }
    codegen_.builder_.CreateCall(
        codegen_.functionMap_.at("zap_arc_add_possible_root"),
        {emitArcRuntimeContext(), rawObject});
    codegen_.builder_.CreateBr(returnBB);
  }

  codegen_.builder_.SetInsertPoint(returnBB);
  codegen_.builder_.CreateRetVoid();

  codegen_.currentFn_ = savedFn;
  if (savedBlock) {
    codegen_.builder_.SetInsertPoint(savedBlock);
  }
}

} // namespace codegen
