#pragma once

#include "instruction.hpp"

#include <cstddef>
#include <optional>

namespace zir {

class Module;

struct CallParameterContract {
    ParameterOwnership ownership;
    ParameterEscape escape;
};

// Resolves the declared contract for a fixed call argument. Variadic arguments
// have no ownership or escape contract and therefore return std::nullopt.
std::optional<CallParameterContract> resolveCallParameterContract(
    const Module& module,
    const CallInst& call,
    size_t argumentIndex
);

ResultBorrowContract resolveCallResultBorrowContract(const Module& module, const CallInst& call);

bool callReturnsRef(const Module& module, const CallInst& call);

bool callTransfersOwnership(const Module& module, const CallInst& call, size_t argumentIndex);

} // namespace zir
