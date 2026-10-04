#include "borrow_verifier.hpp"

#include "borrow_provenance.hpp"
#include "call_contract.hpp"
#include "string_type.hpp"

#include <algorithm>

namespace zir::verifier_detail {
namespace {

std::string formatSources(const BorrowProvenance::OwnerSet& sources) {
    std::vector<std::string> names;
    names.reserve(sources.size());
    for (const auto* source : sources) {
        if (source) {
            names.push_back(source->getName());
        }
    }
    std::sort(names.begin(), names.end());

    std::string result;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i != 0) {
            result += ", ";
        }
        result += names[i];
    }
    return result;
}

const Value* resultBorrowSource(const Function& function) {
    if (!function.resultBorrow.hasSource()) {
        return nullptr;
    }
    const size_t sourceIndex = *function.resultBorrow.sourceParameter();
    return sourceIndex < function.getArguments().size() ? function.getArguments()[sourceIndex].get()
                                                        : nullptr;
}

bool hasDisallowedReturnSource(
    const Function& function,
    const BorrowProvenance::OwnerSet& sources
) {
    const Value* allowedSource = resultBorrowSource(function);
    return std::any_of(sources.begin(), sources.end(), [allowedSource](const Value* source) {
        return source != allowedSource;
    });
}

bool isBorrowTrackedValue(const std::shared_ptr<Value>& value) {
    return value && value->getType()
        && value->getType()->getIntrinsicKind() == IntrinsicTypeKind::StringView;
}

void addError(
    std::vector<VerificationError>& errors,
    const Function& function,
    VerificationErrorCode code,
    const BasicBlock* block,
    std::optional<size_t> instructionIndex,
    std::string message
) {
    errors.push_back(
        {code,
            function.name,
            block ? block->label : std::string{},
            instructionIndex,
            std::move(message)}
    );
}

void verifyEscapes(
    const Module& module,
    const Function& function,
    const ControlFlowGraph& cfg,
    std::vector<VerificationError>& errors
) {
    const auto provenance = analyzeBorrowProvenance(module, function, cfg);
    for (const auto& blockOwner : function.getBlocks()) {
        if (!blockOwner) {
            continue;
        }
        const auto& block = *blockOwner;
        for (size_t i = 0; i < block.getInstructions().size(); ++i) {
            const auto& instruction = block.getInstructions()[i];
            if (!instruction) {
                continue;
            }
            if (instruction->getOpCode() == OpCode::Ret) {
                const auto& ret = static_cast<const ReturnInst&>(*instruction);
                if (!isBorrowTrackedValue(ret.getValue())) {
                    continue;
                }
                const auto sources = provenance.ownersAtDefinition(ret.getValue());
                if (hasDisallowedReturnSource(function, sources)) {
                    addError(
                        errors,
                        function,
                        VerificationErrorCode::InvalidReturn,
                        &block,
                        i,
                        "cannot return " + ret.getValue()->getName()
                            + " backed by non-escaping borrow source " + formatSources(sources)
                    );
                }
            } else if (instruction->getOpCode() == OpCode::Store) {
                const auto& store = static_cast<const StoreInst&>(*instruction);
                if (!isBorrowTrackedValue(store.getSource())) {
                    continue;
                }
                const auto sources = provenance.ownersAtDefinition(store.getSource());
                if (!sources.empty() && !provenance.isLocalStorage(store.getDestination())) {
                    addError(
                        errors,
                        function,
                        VerificationErrorCode::InvalidOperand,
                        &block,
                        i,
                        "cannot store " + store.getSource()->getName()
                            + " backed by non-escaping borrow source " + formatSources(sources)
                            + " outside local storage"
                    );
                }
            } else if (instruction->getOpCode() == OpCode::Call) {
                const auto& call = static_cast<const CallInst&>(*instruction);
                for (size_t argumentIndex = 0; argumentIndex < call.getArguments().size();
                    ++argumentIndex) {
                    if (!isBorrowTrackedValue(call.getArguments()[argumentIndex])) {
                        continue;
                    }
                    const auto sources =
                        provenance.ownersAtDefinition(call.getArguments()[argumentIndex]);
                    const auto resultBorrow = resolveCallResultBorrowContract(module, call);
                    const bool mayBackResult = resultBorrow.hasSource()
                        && *resultBorrow.sourceParameter() == argumentIndex;
                    const auto contract = resolveCallParameterContract(module, call, argumentIndex);
                    if (!sources.empty()
                        && (!contract || contract->escape != ParameterEscape::NoEscape)
                        && !mayBackResult) {
                        addError(
                            errors,
                            function,
                            VerificationErrorCode::InvalidCall,
                            &block,
                            i,
                            "cannot pass " + call.getArguments()[argumentIndex]->getName()
                                + " backed by tracked borrow source " + formatSources(sources)
                                + " to a parameter with an unspecified escape contract"
                        );
                    }
                }
            }
        }
    }
}

void verifyResultContract(const Function& function, std::vector<VerificationError>& errors) {
    if (!function.resultBorrow.hasSource()) {
        return;
    }

    const size_t sourceIndex = *function.resultBorrow.sourceParameter();
    if (sourceIndex >= function.getArguments().size()) {
        addError(
            errors,
            function,
            VerificationErrorCode::InvalidReturn,
            nullptr,
            std::nullopt,
            "result borrow source parameter is out of range"
        );
    } else {
        const auto& source = function.getArguments()[sourceIndex];
        if (!source || source->getParameterOwnership() != ParameterOwnership::Borrow) {
            addError(
                errors,
                function,
                VerificationErrorCode::InvalidReturn,
                nullptr,
                std::nullopt,
                "result borrow source parameter must be borrowed"
            );
        } else if (source->getParameterEscape() == ParameterEscape::NoEscape) {
            addError(
                errors,
                function,
                VerificationErrorCode::InvalidReturn,
                nullptr,
                std::nullopt,
                "noescape parameter cannot back the function result"
            );
        }
    }
    if (function.returnsRef || !function.getReturnType()
        || function.getReturnType()->getIntrinsicKind() != IntrinsicTypeKind::StringView) {
        addError(
            errors,
            function,
            VerificationErrorCode::InvalidReturn,
            nullptr,
            std::nullopt,
            "result borrow contract requires a by-value StringView result"
        );
    }
}

} // namespace

std::vector<VerificationError> verifyBorrowContracts(
    const Module& module,
    const Function& function,
    const ControlFlowGraph& cfg
) {
    std::vector<VerificationError> errors;
    verifyEscapes(module, function, cfg, errors);
    verifyResultContract(function, errors);
    return errors;
}

} // namespace zir::verifier_detail
