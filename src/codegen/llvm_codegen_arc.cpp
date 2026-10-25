#include "../ir/string_type.hpp"
#include "class_arc_emitter.hpp"
#include "llvm_codegen.hpp"

namespace codegen {
namespace {
bool isStringLikeStruct(llvm::Type* ty) {
    auto* st = llvm::dyn_cast_or_null<llvm::StructType>(ty);
    return st && st->getNumElements() == 2;
}
} // namespace

#if defined(ZAP_RUNTIME_INSTRUMENTATION)
void LLVMCodeGen::emitRuntimeOwnershipEvent(const char* name) {
    auto it = functionMap_.find(name);
    if (it == functionMap_.end()) {
        auto* eventType = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), {}, false);
        auto* eventFn =
            llvm::Function::Create(eventType, llvm::Function::ExternalLinkage, name, *module_);
        it = functionMap_.emplace(name, eventFn).first;
    }
    builder_.CreateCall(it->second);
}
#endif

bool LLVMCodeGen::isClassType(const std::shared_ptr<zir::Type>& type) const {
    return arcEmitter_->isClassType(type);
}

bool LLVMCodeGen::isWeakClassType(const std::shared_ptr<zir::Type>& type) const {
    return arcEmitter_->isWeakClassType(type);
}

bool LLVMCodeGen::isOwnedStringType(const std::shared_ptr<zir::Type>& type) const {
    return zir::isIntrinsicStringType(type) && !zir::isIntrinsicStringViewType(type);
}

bool LLVMCodeGen::containsManagedValues(const std::shared_ptr<zir::Type>& type) const {
    return zir::containsManagedValues(type);
}

void LLVMCodeGen::emitManagedRetain(llvm::Value* value, const std::shared_ptr<zir::Type>& type) {
    if (!value || !type) {
        return;
    }
    if (isOwnedStringType(type)) {
        (void)emitStringRetainIfNeeded(value, type);
        return;
    }
    if (isWeakClassType(type)) {
        emitRetainWeakIfNeeded(value, type);
        return;
    }
    if (isClassType(type)) {
        emitRetainIfNeeded(value, type);
        return;
    }
    if (type->getKind() == zir::TypeKind::Record) {
        const auto& record = static_cast<const zir::RecordType&>(*type);
        for (size_t i = 0; i < record.getFields().size(); ++i) {
            const auto& field = record.getFields()[i];
            if (containsManagedValues(field.type)) {
                emitManagedRetain(
                    builder_.CreateExtractValue(value, {static_cast<unsigned>(i)}),
                    field.type
                );
            }
        }
    } else if (type->getKind() == zir::TypeKind::Array) {
        const auto& array = static_cast<const zir::ArrayType&>(*type);
        if (!containsManagedValues(array.getBaseType())) {
            return;
        }
        for (size_t i = 0; i < array.getSize(); ++i) {
            emitManagedRetain(
                builder_.CreateExtractValue(value, {static_cast<unsigned>(i)}),
                array.getBaseType()
            );
        }
    } else if (type->getKind() == zir::TypeKind::TaggedUnion) {
        emitManagedForActiveTaggedUnion(
            value,
            std::static_pointer_cast<zir::TaggedUnionType>(type),
            &LLVMCodeGen::emitManagedRetain
        );
    }
}

void LLVMCodeGen::emitManagedRelease(llvm::Value* value, const std::shared_ptr<zir::Type>& type) {
    if (!value || !type) {
        return;
    }
    if (isOwnedStringType(type)) {
        emitStringReleaseIfNeeded(value, type);
        return;
    }
    if (isWeakClassType(type)) {
        emitReleaseWeakIfNeeded(value, type);
        return;
    }
    if (isClassType(type)) {
        emitReleaseIfNeeded(value, type);
        return;
    }
    if (type->getKind() == zir::TypeKind::Record) {
        const auto& record = static_cast<const zir::RecordType&>(*type);
        for (size_t i = record.getFields().size(); i > 0; --i) {
            const auto& field = record.getFields()[i - 1];
            if (containsManagedValues(field.type)) {
                emitManagedRelease(
                    builder_.CreateExtractValue(value, {static_cast<unsigned>(i - 1)}),
                    field.type
                );
            }
        }
    } else if (type->getKind() == zir::TypeKind::Array) {
        const auto& array = static_cast<const zir::ArrayType&>(*type);
        if (!containsManagedValues(array.getBaseType())) {
            return;
        }
        for (size_t i = array.getSize(); i > 0; --i) {
            emitManagedRelease(
                builder_.CreateExtractValue(value, {static_cast<unsigned>(i - 1)}),
                array.getBaseType()
            );
        }
    } else if (type->getKind() == zir::TypeKind::TaggedUnion) {
        emitManagedForActiveTaggedUnion(
            value,
            std::static_pointer_cast<zir::TaggedUnionType>(type),
            &LLVMCodeGen::emitManagedRelease
        );
    }
}

void LLVMCodeGen::emitManagedForActiveTaggedUnion(
    llvm::Value* value,
    const std::shared_ptr<zir::TaggedUnionType>& type,
    void (LLVMCodeGen::*operation)(llvm::Value*, const std::shared_ptr<zir::Type>&)
) {
    if (!value || !type || !containsManagedValues(type)) {
        return;
    }

    auto* unionTy = llvm::cast<llvm::StructType>(toLLVMType(*type));
    auto* storage = createEntryAlloca(currentFn_, "arc.union.addr", unionTy);
    builder_.CreateStore(value, storage);
    auto* tagAddr = builder_.CreateStructGEP(unionTy, storage, 0, "arc.union.tag.addr");
    auto* tag = builder_.CreateLoad(llvm::Type::getInt32Ty(ctx_), tagAddr, "arc.union.tag");
    auto* done = llvm::BasicBlock::Create(ctx_, "arc.union.done", currentFn_);

    for (const auto& variant : type->getVariants()) {
        if (!containsManagedValues(variant.payloadType)) {
            continue;
        }

        auto* active = llvm::BasicBlock::Create(ctx_, "arc.union.active", currentFn_);
        auto* next = llvm::BasicBlock::Create(ctx_, "arc.union.next", currentFn_);
        auto* isActive = builder_.CreateICmpEQ(
            tag,
            llvm::ConstantInt::getSigned(llvm::Type::getInt32Ty(ctx_), variant.tag),
            "arc.union.is_active"
        );
        builder_.CreateCondBr(isActive, active, next);

        builder_.SetInsertPoint(active);
        auto* payloadAddr = builder_.CreateStructGEP(unionTy, storage, 1, "arc.union.payload.addr");
        auto* payload =
            builder_.CreateLoad(toLLVMType(*variant.payloadType), payloadAddr, "arc.union.payload");
        (this->*operation)(payload, variant.payloadType);
        builder_.CreateBr(done);

        builder_.SetInsertPoint(next);
    }

    builder_.CreateBr(done);
    builder_.SetInsertPoint(done);
}

void LLVMCodeGen::emitOwnershipRelease(
    llvm::Value* value,
    const std::shared_ptr<zir::Type>& type,
    zir::ValueOwnership ownership
) {
    if (ownership == zir::ValueOwnership::OwnedStrong && isWeakClassType(type)) {
        auto strongType =
            std::make_shared<zir::ClassType>(*std::static_pointer_cast<zir::ClassType>(type));
        strongType->setWeak(false);
        emitReleaseIfNeeded(value, strongType);
        return;
    }
    emitManagedRelease(value, type);
}

void LLVMCodeGen::emitArcCollectionSafePoint() {
    // ZIR lowering calls this only after local releases on a non-destructor
    // return, so collection cannot reenter through an active destructor.
    auto* rawPtrTy = llvm::PointerType::getUnqual(ctx_);
    auto contextIt = functionMap_.find("zap_arc_default_context");
    if (contextIt == functionMap_.end()) {
        auto* contextTy = llvm::FunctionType::get(rawPtrTy, {}, false);
        auto* contextFn = llvm::Function::Create(
            contextTy,
            llvm::Function::ExternalLinkage,
            "zap_arc_default_context",
            *module_
        );
        contextIt = functionMap_.emplace("zap_arc_default_context", contextFn).first;
    }
    auto it = functionMap_.find("zap_arc_collect_at_safepoint");
    if (it == functionMap_.end()) {
        auto* safePointTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), {rawPtrTy}, false);
        auto* safePointFn = llvm::Function::Create(
            safePointTy,
            llvm::Function::ExternalLinkage,
            "zap_arc_collect_at_safepoint",
            *module_
        );
        it = functionMap_.emplace("zap_arc_collect_at_safepoint", safePointFn).first;
    }
    auto* context = builder_.CreateCall(contextIt->second, {}, "arc.context");
    builder_.CreateCall(it->second, {context});
}

void LLVMCodeGen::emitRetainIfNeeded(llvm::Value* value, const std::shared_ptr<zir::Type>& type) {
    if (isOwnedStringType(type)) {
        (void)emitStringRetainIfNeeded(value, type);
        return;
    }
    arcEmitter_->emitRetainIfNeeded(value, type);
}

void LLVMCodeGen::emitReleaseIfNeeded(llvm::Value* value, const std::shared_ptr<zir::Type>& type) {
    if (isOwnedStringType(type)) {
        emitStringReleaseIfNeeded(value, type);
        return;
    }
    arcEmitter_->emitReleaseIfNeeded(value, type);
}

llvm::Value* LLVMCodeGen::emitStringRetainIfNeeded(
    llvm::Value* value,
    const std::shared_ptr<zir::Type>& type
) {
    if (!isOwnedStringType(type)) {
        return value;
    }
    auto* stringTy = toLLVMType(*type);
    if (value->getType() != stringTy && isStringLikeStruct(value->getType())
        && isStringLikeStruct(stringTy)) {
        auto* ptr = builder_.CreateExtractValue(value, {0}, "str.cvt.ptr");
        auto* len = builder_.CreateExtractValue(value, {1}, "str.cvt.len");
        llvm::Value* converted = llvm::UndefValue::get(stringTy);
        converted = builder_.CreateInsertValue(converted, ptr, {0}, "str.cvt.ptr.i");
        converted = builder_.CreateInsertValue(converted, len, {1}, "str.cvt.len.i");
        value = converted;
    }
    auto* fnTy = llvm::FunctionType::get(stringTy, {stringTy}, false);
    auto callee = module_->getOrInsertFunction("zap_string_retain", fnTy);
    return builder_.CreateCall(fnTy, callee.getCallee(), {value}, "str.retain");
}

void LLVMCodeGen::emitStringReleaseIfNeeded(
    llvm::Value* value,
    const std::shared_ptr<zir::Type>& type
) {
    if (!isOwnedStringType(type)) {
        return;
    }
    auto* stringTy = toLLVMType(*type);
    if (value->getType() != stringTy && isStringLikeStruct(value->getType())
        && isStringLikeStruct(stringTy)) {
        auto* ptr = builder_.CreateExtractValue(value, {0}, "str.cvt.ptr");
        auto* len = builder_.CreateExtractValue(value, {1}, "str.cvt.len");
        llvm::Value* converted = llvm::UndefValue::get(stringTy);
        converted = builder_.CreateInsertValue(converted, ptr, {0}, "str.cvt.ptr.i");
        converted = builder_.CreateInsertValue(converted, len, {1}, "str.cvt.len.i");
        value = converted;
    }
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), {stringTy}, false);
    auto callee = module_->getOrInsertFunction("zap_string_release", fnTy);
    builder_.CreateCall(fnTy, callee.getCallee(), {value});
}

void LLVMCodeGen::emitRetainWeakIfNeeded(
    llvm::Value* value,
    const std::shared_ptr<zir::Type>& type
) {
    arcEmitter_->emitRetainWeakIfNeeded(value, type);
}

void LLVMCodeGen::emitReleaseWeakIfNeeded(
    llvm::Value* value,
    const std::shared_ptr<zir::Type>& type
) {
    arcEmitter_->emitReleaseWeakIfNeeded(value, type);
}

llvm::Value* LLVMCodeGen::emitWeakAlive(
    llvm::Value* value,
    const std::shared_ptr<zir::Type>& type
) {
    return arcEmitter_->emitWeakAlive(value, type);
}

llvm::Value* LLVMCodeGen::emitWeakLock(llvm::Value* value, const std::shared_ptr<zir::Type>& type) {
    return arcEmitter_->emitWeakLock(value, type);
}

void LLVMCodeGen::emitStoreWithArc(
    llvm::Value* addr,
    llvm::Value* value,
    const std::shared_ptr<zir::Type>& type,
    zir::ValueOwnership valueOwnership,
    bool skipReleaseOld
) {
    if (isOwnedStringType(type)) {
        emitStoreWithStringArc(addr, value, type, zir::isOwned(valueOwnership), skipReleaseOld);
        return;
    }
    arcEmitter_->emitStoreWithArc(addr, value, type, valueOwnership, skipReleaseOld);
}

void LLVMCodeGen::emitStoreWithStringArc(
    llvm::Value* addr,
    llvm::Value* value,
    const std::shared_ptr<zir::Type>& type,
    bool valueIsOwned,
    bool skipReleaseOld
) {
    if (!skipReleaseOld) {
        auto* oldValue = builder_.CreateLoad(toLLVMType(*type), addr, "str.old");
        emitStringReleaseIfNeeded(oldValue, type);
    }

    auto* storedValue = valueIsOwned ? value : emitStringRetainIfNeeded(value, type);
    builder_.CreateStore(storedValue, addr);
}

void LLVMCodeGen::ensureClassArcSupport(const std::shared_ptr<zir::ClassType>& classType) {
    arcEmitter_->ensureClassArcSupport(classType);
}

} // namespace codegen
