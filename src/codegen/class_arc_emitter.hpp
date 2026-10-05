#pragma once

#include "../ir/value.hpp"
#include <cstdint>
#include <memory>
#include <vector>

namespace llvm {
class Function;
class StructType;
class Value;
} // namespace llvm

namespace codegen {
class LLVMCodeGen;

class ClassArcEmitter {
public:
    explicit ClassArcEmitter(LLVMCodeGen& codegen);

    bool isClassType(const std::shared_ptr<zir::Type>& type) const;
    bool isWeakClassType(const std::shared_ptr<zir::Type>& type) const;
    void emitRetainIfNeeded(llvm::Value* value, const std::shared_ptr<zir::Type>& type);
    void emitReleaseIfNeeded(llvm::Value* value, const std::shared_ptr<zir::Type>& type);
    void emitRetainWeakIfNeeded(llvm::Value* value, const std::shared_ptr<zir::Type>& type);
    void emitReleaseWeakIfNeeded(llvm::Value* value, const std::shared_ptr<zir::Type>& type);
    llvm::Value* emitWeakAlive(llvm::Value* value, const std::shared_ptr<zir::Type>& type);
    llvm::Value* emitWeakLock(llvm::Value* value, const std::shared_ptr<zir::Type>& type);
    void emitStoreWithArc(
        llvm::Value* addr,
        llvm::Value* value,
        const std::shared_ptr<zir::Type>& type,
        zir::ValueOwnership valueOwnership,
        bool skipReleaseOld = false
    );
    void ensureClassArcSupport(const std::shared_ptr<zir::ClassType>& classType);

private:
    llvm::Function* getOrCreateRefcountFailureFunction(const char* name);
    llvm::Function* getOrCreateArcDeallocateFunction();
    llvm::Value* emitArcRuntimeContext();
    void emitRefcountFailure(const char* name);
    void ensureNestedClassArcSupport(const std::shared_ptr<zir::Type>& type);
    llvm::Function* emitClassTraceFunction(
        const std::shared_ptr<zir::ClassType>& classType,
        llvm::StructType* objectType
    );
    void emitTraceChildren(
        const std::shared_ptr<zir::Type>& type,
        llvm::Value* address,
        llvm::Value* visitor,
        llvm::Value* context
    );

    LLVMCodeGen& codegen_;
};
} // namespace codegen
