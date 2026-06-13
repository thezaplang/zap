#include "class_arc_emitter.hpp"
#include "class_layout.hpp"
#include "llvm_codegen.hpp"
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>

namespace codegen {

ClassArcEmitter::ClassArcEmitter(LLVMCodeGen &codegen) : codegen_(codegen) {}

bool ClassArcEmitter::isClassType(
    const std::shared_ptr<zir::Type> &type) const {
  return type && type->getKind() == zir::TypeKind::Class;
}

bool ClassArcEmitter::isWeakClassType(
    const std::shared_ptr<zir::Type> &type) const {
  return isClassType(type) &&
         std::static_pointer_cast<zir::ClassType>(type)->isWeak();
}

bool ClassArcEmitter::expressionProducesOwnedClass(
    const sema::BoundExpression *expr) const {
  if (!expr || !isClassType(expr->type)) {
    return false;
  }
  if (dynamic_cast<const sema::BoundNewExpression *>(expr)) {
    return true;
  }
  if (dynamic_cast<const sema::BoundFunctionCall *>(expr)) {
    return true;
  }
  if (dynamic_cast<const sema::BoundWeakLockExpression *>(expr)) {
    return true;
  }
  if (auto cast = dynamic_cast<const sema::BoundCast *>(expr)) {
    return expressionProducesOwnedClass(cast->expression.get());
  }
  if (auto ternary = dynamic_cast<const sema::BoundTernaryExpression *>(expr)) {
    return expressionProducesOwnedClass(ternary->thenExpr.get()) &&
           expressionProducesOwnedClass(ternary->elseExpr.get());
  }
  return false;
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

  auto *retainBB = llvm::BasicBlock::Create(codegen_.ctx_, "arc.retain.do",
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
      value, llvm::PointerType::getUnqual(objectTy), "arc.retain.cast");
  auto *countAddr = codegen_.builder_.CreateStructGEP(
      objectTy, typedPtr, kClassStrongCountIndex, "arc.retain.count.addr");
  auto *count = codegen_.builder_.CreateLoad(
      llvm::Type::getInt64Ty(codegen_.ctx_), countAddr, "arc.retain.count");
  auto *next = codegen_.builder_.CreateAdd(
      count, llvm::ConstantInt::get(llvm::Type::getInt64Ty(codegen_.ctx_), 1),
      "arc.retain.next");
  codegen_.builder_.CreateStore(next, countAddr);
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
      value, llvm::PointerType::getUnqual(objectTy), "arc.release.cast");
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
      llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(codegen_.ctx_)),
      releaseAddr, "arc.release.fn");
  auto *rawObject = codegen_.builder_.CreateBitCast(
      typedPtr,
      llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(codegen_.ctx_)));
  auto *releaseTy = llvm::FunctionType::get(
      llvm::Type::getVoidTy(codegen_.ctx_),
      {llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(codegen_.ctx_))},
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

  auto *retainBB = llvm::BasicBlock::Create(codegen_.ctx_, "arc.weak.retain.do",
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
      value, llvm::PointerType::getUnqual(objectTy), "arc.weak.retain.cast");
  auto *countAddr = codegen_.builder_.CreateStructGEP(
      objectTy, typedPtr, kClassWeakCountIndex, "arc.weak.count.addr");
  auto *count = codegen_.builder_.CreateLoad(
      llvm::Type::getInt64Ty(codegen_.ctx_), countAddr, "arc.weak.count");
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
      codegen_.ctx_, "arc.weak.release.do", codegen_.currentFn_);
  auto *checkFreeBB = llvm::BasicBlock::Create(
      codegen_.ctx_, "arc.weak.release.checkfree", codegen_.currentFn_);
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
      value, llvm::PointerType::getUnqual(objectTy), "arc.weak.release.cast");
  auto *weakAddr = codegen_.builder_.CreateStructGEP(
      objectTy, typedPtr, kClassWeakCountIndex, "arc.weak.release.count.addr");
  auto *weakCount =
      codegen_.builder_.CreateLoad(llvm::Type::getInt64Ty(codegen_.ctx_),
                                   weakAddr, "arc.weak.release.count");
  auto *nextWeak = codegen_.builder_.CreateSub(
      weakCount,
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(codegen_.ctx_), 1),
      "arc.weak.release.next");
  codegen_.builder_.CreateStore(nextWeak, weakAddr);
  auto *isWeakZero = codegen_.builder_.CreateICmpEQ(
      nextWeak,
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(codegen_.ctx_), 0));
  codegen_.builder_.CreateCondBr(isWeakZero, checkFreeBB, contBB);

  codegen_.builder_.SetInsertPoint(checkFreeBB);
  auto *strongAddr = codegen_.builder_.CreateStructGEP(
      objectTy, typedPtr, kClassStrongCountIndex,
      "arc.weak.release.strong.addr");
  auto *strongCount =
      codegen_.builder_.CreateLoad(llvm::Type::getInt64Ty(codegen_.ctx_),
                                   strongAddr, "arc.weak.release.strong");
  auto *isStrongZero = codegen_.builder_.CreateICmpEQ(
      strongCount,
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(codegen_.ctx_), 0));
  codegen_.builder_.CreateCondBr(isStrongZero, freeBB, contBB);

  codegen_.builder_.SetInsertPoint(freeBB);
  auto *rawObject = codegen_.builder_.CreateBitCast(
      typedPtr,
      llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(codegen_.ctx_)));
  if (codegen_.functionMap_.count("zap_arc_remove_possible_root") == 0) {
    auto *removeTy = llvm::FunctionType::get(
        llvm::Type::getVoidTy(codegen_.ctx_), {rawObject->getType()}, false);
    auto *removeFn = llvm::Function::Create(
        removeTy, llvm::Function::ExternalLinkage,
        "zap_arc_remove_possible_root", *codegen_.module_);
    codegen_.functionMap_["zap_arc_remove_possible_root"] = removeFn;
  }
  codegen_.builder_.CreateCall(
      codegen_.functionMap_.at("zap_arc_remove_possible_root"), {rawObject});
  if (codegen_.functionMap_.count("free") == 0) {
    auto *freeTy = llvm::FunctionType::get(llvm::Type::getVoidTy(codegen_.ctx_),
                                           {rawObject->getType()}, false);
    auto *freeFn = llvm::Function::Create(
        freeTy, llvm::Function::ExternalLinkage, "free", *codegen_.module_);
    codegen_.functionMap_["free"] = freeFn;
  }
  codegen_.builder_.CreateCall(codegen_.functionMap_.at("free"), {rawObject});
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
      value, llvm::PointerType::getUnqual(objectTy), "arc.weak.alive.cast");
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
      value, llvm::PointerType::getUnqual(objectTy), "arc.weak.lock.cast");
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
                                       bool valueIsOwned, bool skipReleaseOld) {
  if (!isClassType(type)) {
    codegen_.builder_.CreateStore(value, addr);
    return;
  }

  if (isWeakClassType(type)) {
    emitRetainWeakIfNeeded(value, type);
    if (!skipReleaseOld) {
      auto *oldValue = codegen_.builder_.CreateLoad(codegen_.toLLVMType(*type),
                                                    addr, "arc.weak.store.old");
      codegen_.builder_.CreateStore(value, addr);
      emitReleaseWeakIfNeeded(oldValue, type);
    } else {
      codegen_.builder_.CreateStore(value, addr);
    }
    if (valueIsOwned) {
      auto strongType = std::make_shared<zir::ClassType>(
          *std::static_pointer_cast<zir::ClassType>(type));
      strongType->setWeak(false);
      emitReleaseIfNeeded(value, strongType);
    }
    return;
  }

  if (!valueIsOwned) {
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

void ClassArcEmitter::emitScopeReleases() {
  if (codegen_.scopeClassLocals_.empty()) {
    return;
  }

  auto &locals = codegen_.scopeClassLocals_.back();
  for (auto it = locals.rbegin(); it != locals.rend(); ++it) {
    auto *addr = it->second;
    auto *value = codegen_.builder_.CreateLoad(codegen_.toLLVMType(*it->first),
                                               addr, "arc.scope.value");
    if (isWeakClassType(it->first)) {
      emitReleaseWeakIfNeeded(value, it->first);
    } else {
      emitReleaseIfNeeded(value, it->first);
    }
  }
  locals.clear();
}

void ClassArcEmitter::ensureClassArcSupport(
    const std::shared_ptr<zir::ClassType> &classType) {
  if (!classType || codegen_.classReleaseFns_.count(classType->getName())) {
    return;
  }

  codegen_.toLLVMType(*classType);
  auto *objectTy =
      codegen_.structCache_.at(classType->getCodegenName() + ".obj");
  auto *rawPtrTy =
      llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(codegen_.ctx_));
  auto *helperTy = llvm::FunctionType::get(llvm::Type::getVoidTy(codegen_.ctx_),
                                           {rawPtrTy}, false);
  auto *releaseHelper = llvm::Function::Create(
      helperTy, llvm::Function::InternalLinkage,
      "__zap_arc_release_" + classType->getCodegenName(), *codegen_.module_);
  auto *destroyHelper = llvm::Function::Create(
      helperTy, llvm::Function::InternalLinkage,
      "__zap_arc_destroy_" + classType->getCodegenName(), *codegen_.module_);
  codegen_.classReleaseFns_[classType->getName()] = releaseHelper;
  codegen_.classDestroyFns_[classType->getName()] = destroyHelper;

  for (const auto &field : classType->getFields()) {
    if (field.type && field.type->getKind() == zir::TypeKind::Class) {
      ensureClassArcSupport(
          std::static_pointer_cast<zir::ClassType>(field.type));
    }
  }

  if (!codegen_.classVTables_.count(classType->getName())) {
    std::vector<llvm::Constant *> entries;
    if (auto base = classType->getBase()) {
      ensureClassArcSupport(base);
      auto *baseVTable = codegen_.classVTables_.at(base->getName());
      if (auto *baseInit = baseVTable->getInitializer()) {
        for (unsigned i = 0; i < baseInit->getNumOperands(); ++i) {
          entries.push_back(
              llvm::cast<llvm::Constant>(baseInit->getOperand(i)));
        }
      }
    }

    auto methodsIt = codegen_.classVirtualMethodFns_.find(classType->getName());
    if (methodsIt != codegen_.classVirtualMethodFns_.end()) {
      auto *i8PtrTy =
          llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(codegen_.ctx_));
      for (const auto &[slot, fn] : methodsIt->second) {
        if (slot >= static_cast<int>(entries.size())) {
          entries.resize(static_cast<size_t>(slot + 1),
                         llvm::ConstantPointerNull::get(i8PtrTy));
        }
        entries[slot] = llvm::ConstantExpr::getBitCast(fn, i8PtrTy);
      }
    }

    auto *i8PtrTy =
        llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(codegen_.ctx_));
    auto *vtableTy =
        llvm::ArrayType::get(i8PtrTy, static_cast<uint64_t>(entries.size()));
    auto *init = llvm::ConstantArray::get(vtableTy, entries);
    auto *gv = new llvm::GlobalVariable(
        *codegen_.module_, vtableTy, true, llvm::GlobalValue::InternalLinkage,
        init, "__zap_vtable_" + classType->getCodegenName());
    codegen_.classVTables_[classType->getName()] = gv;
  }

  if (!codegen_.classMetadataGlobals_.count(classType->getName())) {
    std::vector<uint32_t> strongFieldOffsets;
    auto *layout = codegen_.module_->getDataLayout().getStructLayout(objectTy);
    for (size_t i = 0; i < classType->getFields().size(); ++i) {
      const auto &field = classType->getFields()[i];
      if (!field.type || field.type->getKind() != zir::TypeKind::Class ||
          isWeakClassType(field.type)) {
        continue;
      }
      strongFieldOffsets.push_back(
          static_cast<uint32_t>(layout->getElementOffset(
              static_cast<unsigned>(i + kClassFieldStartIndex))));
    }

    auto *i32Ty = llvm::Type::getInt32Ty(codegen_.ctx_);
    auto *i32PtrTy = llvm::PointerType::getUnqual(i32Ty);
    llvm::Constant *offsetPtr = llvm::ConstantPointerNull::get(i32PtrTy);
    if (!strongFieldOffsets.empty()) {
      std::vector<llvm::Constant *> offsetConstants;
      for (uint32_t offset : strongFieldOffsets) {
        offsetConstants.push_back(llvm::ConstantInt::get(i32Ty, offset));
      }
      auto *offsetArrayTy = llvm::ArrayType::get(i32Ty, offsetConstants.size());
      auto *offsetArrayInit =
          llvm::ConstantArray::get(offsetArrayTy, offsetConstants);
      auto *offsetGlobal = new llvm::GlobalVariable(
          *codegen_.module_, offsetArrayTy, true,
          llvm::GlobalValue::InternalLinkage, offsetArrayInit,
          "__zap_arc_offsets_" + classType->getCodegenName());
      auto *zero =
          llvm::ConstantInt::get(llvm::Type::getInt32Ty(codegen_.ctx_), 0);
      llvm::Constant *indices[] = {zero, zero};
      offsetPtr = llvm::ConstantExpr::getInBoundsGetElementPtr(
          offsetArrayTy, offsetGlobal, indices);
    }

    auto *metaTy = llvm::StructType::create(
        codegen_.ctx_, "__zap_arc_meta_" + classType->getCodegenName());
    metaTy->setBody({i32Ty, i32PtrTy});
    auto *metaInit = llvm::ConstantStruct::get(
        metaTy,
        llvm::ConstantInt::get(
            i32Ty, static_cast<uint32_t>(strongFieldOffsets.size())),
        offsetPtr);
    auto *metaGlobal = new llvm::GlobalVariable(
        *codegen_.module_, metaTy, true, llvm::GlobalValue::InternalLinkage,
        metaInit, "__zap_arc_meta_" + classType->getCodegenName());
    codegen_.classMetadataGlobals_[classType->getName()] = metaGlobal;
  }

  auto savedFn = codegen_.currentFn_;
  auto savedLocals = codegen_.localValues_;
  auto savedBlock = codegen_.builder_.GetInsertBlock();

  codegen_.currentFn_ = destroyHelper;
  codegen_.localValues_.clear();
  auto *destroyEntry =
      llvm::BasicBlock::Create(codegen_.ctx_, "entry", destroyHelper);
  auto *destroyReturnBB =
      llvm::BasicBlock::Create(codegen_.ctx_, "arc.ret", destroyHelper);
  auto *destroyFreeBB =
      llvm::BasicBlock::Create(codegen_.ctx_, "arc.free", destroyHelper);
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
  auto *destroyTypedObject = codegen_.builder_.CreateBitCast(
      destroyRawObject, llvm::PointerType::getUnqual(objectTy), "object");
  auto *aliveAddr = codegen_.builder_.CreateStructGEP(
      objectTy, destroyTypedObject, kClassAliveIndex, "alive.addr");
  codegen_.builder_.CreateStore(
      llvm::ConstantInt::get(llvm::Type::getInt8Ty(codegen_.ctx_), 0),
      aliveAddr);
  auto dtorIt = codegen_.classDestructorFns_.find(classType->getName());
  if (dtorIt != codegen_.classDestructorFns_.end()) {
    auto *dtorPtr = codegen_.builder_.CreateBitCast(
        destroyTypedObject, dtorIt->second->getArg(0)->getType(), "dtor.self");
    codegen_.builder_.CreateCall(dtorIt->second, {dtorPtr});
  }

  for (size_t i = 0; i < classType->getFields().size(); ++i) {
    const auto &field = classType->getFields()[i];
    if (!field.type || field.type->getKind() != zir::TypeKind::Class) {
      continue;
    }
    auto *fieldAddr = codegen_.builder_.CreateStructGEP(
        objectTy, destroyTypedObject,
        static_cast<unsigned>(i + kClassFieldStartIndex));
    auto *fieldValue = codegen_.builder_.CreateLoad(
        codegen_.toLLVMType(*field.type), fieldAddr, field.name);
    if (isWeakClassType(field.type)) {
      emitReleaseWeakIfNeeded(fieldValue, field.type);
    } else {
      emitReleaseIfNeeded(fieldValue, field.type);
    }
  }

  auto *weakAddr = codegen_.builder_.CreateStructGEP(
      objectTy, destroyTypedObject, kClassWeakCountIndex, "weakcount.addr");
  auto *weakCount = codegen_.builder_.CreateLoad(
      llvm::Type::getInt64Ty(codegen_.ctx_), weakAddr, "weakcount");
  auto *isWeakZero = codegen_.builder_.CreateICmpEQ(
      weakCount,
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(codegen_.ctx_), 0));
  codegen_.builder_.CreateCondBr(isWeakZero, destroyFreeBB, destroyReturnBB);

  codegen_.builder_.SetInsertPoint(destroyFreeBB);
  if (codegen_.functionMap_.count("zap_arc_remove_possible_root") == 0) {
    auto *removeTy = llvm::FunctionType::get(
        llvm::Type::getVoidTy(codegen_.ctx_), {rawPtrTy}, false);
    auto *removeFn = llvm::Function::Create(
        removeTy, llvm::Function::ExternalLinkage,
        "zap_arc_remove_possible_root", *codegen_.module_);
    codegen_.functionMap_["zap_arc_remove_possible_root"] = removeFn;
  }
  codegen_.builder_.CreateCall(
      codegen_.functionMap_.at("zap_arc_remove_possible_root"),
      {destroyRawObject});
  if (codegen_.functionMap_.count("free") == 0) {
    auto *freeTy = llvm::FunctionType::get(llvm::Type::getVoidTy(codegen_.ctx_),
                                           {rawPtrTy}, false);
    auto *freeFn = llvm::Function::Create(
        freeTy, llvm::Function::ExternalLinkage, "free", *codegen_.module_);
    codegen_.functionMap_["free"] = freeFn;
  }
  codegen_.builder_.CreateCall(codegen_.functionMap_.at("free"),
                               {destroyRawObject});
  codegen_.builder_.CreateBr(destroyReturnBB);

  codegen_.builder_.SetInsertPoint(destroyReturnBB);
  codegen_.builder_.CreateRetVoid();

  codegen_.currentFn_ = releaseHelper;
  codegen_.localValues_.clear();
  auto *entry = llvm::BasicBlock::Create(codegen_.ctx_, "entry", releaseHelper);
  auto *decrementedBB =
      llvm::BasicBlock::Create(codegen_.ctx_, "arc.dec", releaseHelper);
  auto *destroyBB =
      llvm::BasicBlock::Create(codegen_.ctx_, "arc.destroy", releaseHelper);
  bool canBeCyclic = codegen_.cyclicClasses_.count(classType->getName()) != 0;
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
      rawObject, llvm::PointerType::getUnqual(objectTy), "object");
  auto *countAddr = codegen_.builder_.CreateStructGEP(
      objectTy, typedObject, kClassStrongCountIndex, "refcount.addr");
  auto *count = codegen_.builder_.CreateLoad(
      llvm::Type::getInt64Ty(codegen_.ctx_), countAddr, "refcount");
  auto *nextCount = codegen_.builder_.CreateSub(
      count, llvm::ConstantInt::get(llvm::Type::getInt64Ty(codegen_.ctx_), 1),
      "refcount.next");
  codegen_.builder_.CreateStore(nextCount, countAddr);
  auto *isZero = codegen_.builder_.CreateICmpEQ(
      nextCount,
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(codegen_.ctx_), 0));
  codegen_.builder_.CreateCondBr(isZero, destroyBB,
                                 canBeCyclic ? cycleBB : returnBB);

  codegen_.builder_.SetInsertPoint(destroyBB);
  auto *destroyAddr = codegen_.builder_.CreateStructGEP(
      objectTy, typedObject, kClassDestroyFnIndex, "destroy.addr");
  auto *destroyFn = codegen_.builder_.CreateLoad(
      llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(codegen_.ctx_)),
      destroyAddr, "destroy.fn");
  auto *destroyTy = llvm::FunctionType::get(
      llvm::Type::getVoidTy(codegen_.ctx_), {rawPtrTy}, false);
  codegen_.builder_.CreateCall(destroyTy, destroyFn, {rawObject});
  codegen_.builder_.CreateBr(returnBB);

  if (canBeCyclic) {
    codegen_.builder_.SetInsertPoint(cycleBB);
    if (codegen_.functionMap_.count("zap_arc_add_possible_root") == 0) {
      auto *addTy = llvm::FunctionType::get(
          llvm::Type::getVoidTy(codegen_.ctx_), {rawPtrTy}, false);
      auto *addFn = llvm::Function::Create(
          addTy, llvm::Function::ExternalLinkage, "zap_arc_add_possible_root",
          *codegen_.module_);
      codegen_.functionMap_["zap_arc_add_possible_root"] = addFn;
    }
    codegen_.builder_.CreateCall(
        codegen_.functionMap_.at("zap_arc_add_possible_root"), {rawObject});
    if (codegen_.functionMap_.count("zap_arc_cycle_collect") == 0) {
      auto *collectTy = llvm::FunctionType::get(
          llvm::Type::getVoidTy(codegen_.ctx_), {}, false);
      auto *collectFn =
          llvm::Function::Create(collectTy, llvm::Function::ExternalLinkage,
                                 "zap_arc_cycle_collect", *codegen_.module_);
      codegen_.functionMap_["zap_arc_cycle_collect"] = collectFn;
    }
    codegen_.builder_.CreateCall(
        codegen_.functionMap_.at("zap_arc_cycle_collect"));
    codegen_.builder_.CreateBr(returnBB);
  }

  codegen_.builder_.SetInsertPoint(returnBB);
  codegen_.builder_.CreateRetVoid();

  codegen_.currentFn_ = savedFn;
  codegen_.localValues_ = std::move(savedLocals);
  if (savedBlock) {
    codegen_.builder_.SetInsertPoint(savedBlock);
  }
}

void ClassArcEmitter::ensureArcSupport(sema::BoundRootNode &root) {
  for (const auto &rec : root.records) {
    if (rec->type && rec->type->getKind() == zir::TypeKind::Class) {
      codegen_.toLLVMType(*rec->type);
    }
  }
}
} // namespace codegen
