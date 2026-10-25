#include "call_contract.hpp"

#include "module.hpp"

namespace zir {
namespace {

bool ownsManagedValue(const std::shared_ptr<Value>& value) {
    return value && isOwned(value->getOwnership()) && containsManagedValues(value->getType());
}

bool isBorrowedMethodSelf(const Module& module, const CallInst& call, size_t argumentIndex) {
    if (call.isIndirect() || argumentIndex != 0) {
        return false;
    }
    const auto* callee = module.findFunction(call.getFunctionName());
    return callee && (!callee->ownerTypeCodegenName.empty() || callee->hasExtensionReceiver)
        && !callee->getArguments().empty() && callee->getArguments().front()
        && callee->getArguments().front()->getRawName() == "self";
}

} // namespace

std::optional<CallParameterContract> resolveCallParameterContract(
    const Module& module,
    const CallInst& call,
    size_t argumentIndex
) {
    if (call.isIndirect()) {
        const auto functionType = call.getCalleeValue()
            ? std::dynamic_pointer_cast<FunctionPointerType>(call.getCalleeValue()->getType())
            : nullptr;
        if (!functionType || argumentIndex >= functionType->getParameterOwnership().size()) {
            return std::nullopt;
        }
        const auto& escapes = functionType->getParameterEscapes();
        return CallParameterContract{functionType->getParameterOwnership()[argumentIndex],
            argumentIndex < escapes.size() ? escapes[argumentIndex] : ParameterEscape::Unspecified};
    }

    const auto* callee = module.findFunction(call.getFunctionName());
    if (!callee) {
        return std::nullopt;
    }
    size_t fixedIndex = 0;
    for (const auto& parameter : callee->getArguments()) {
        if (!parameter || parameter->isVariadicPack()) {
            continue;
        }
        if (fixedIndex++ == argumentIndex) {
            return CallParameterContract{parameter->getParameterOwnership(),
                parameter->getParameterEscape()};
        }
    }
    return std::nullopt;
}

ResultBorrowContract resolveCallResultBorrowContract(const Module& module, const CallInst& call) {
    if (call.isIndirect()) {
        const auto functionType = call.getCalleeValue()
            ? std::dynamic_pointer_cast<FunctionPointerType>(call.getCalleeValue()->getType())
            : nullptr;
        return functionType ? functionType->getResultBorrow() : ResultBorrowContract{};
    }
    const auto* callee = module.findFunction(call.getFunctionName());
    return callee ? callee->resultBorrow : ResultBorrowContract{};
}

bool callReturnsRef(const Module& module, const CallInst& call) {
    if (call.isIndirect()) {
        const auto functionType = call.getCalleeValue()
            ? std::dynamic_pointer_cast<FunctionPointerType>(call.getCalleeValue()->getType())
            : nullptr;
        return functionType && functionType->returnsRef();
    }
    const auto* callee = module.findFunction(call.getFunctionName());
    return callee && callee->returnsRef;
}

bool callTransfersOwnership(const Module& module, const CallInst& call, size_t argumentIndex) {
    if (argumentIndex >= call.getArguments().size()
        || !ownsManagedValue(call.getArguments()[argumentIndex])
        || (argumentIndex < call.getArgumentIsRef().size()
            && call.getArgumentIsRef()[argumentIndex])) {
        return false;
    }
    const auto contract = resolveCallParameterContract(module, call, argumentIndex);
    return contract && transfersOwnership(contract->ownership)
        && !isBorrowedMethodSelf(module, call, argumentIndex);
}

} // namespace zir
