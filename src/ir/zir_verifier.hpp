#pragma once

#include "module.hpp"
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace zir {

enum class VerificationErrorCode {
    NullNode,
    DuplicateSymbol,
    DuplicateBlock,
    DuplicateValue,
    MissingBody,
    MissingTerminator,
    InstructionAfterTerminator,
    InvalidBranchTarget,
    InvalidConditionType,
    UndefinedValue,
    UseBeforeDefinition,
    DominanceViolation,
    TypeMismatch,
    InvalidResult,
    InvalidOperand,
    InvalidCall,
    InvalidPhi,
    InvalidReturn,
    OwnershipViolation,
};

struct VerificationError {
    VerificationErrorCode code;
    std::string function;
    std::string block;
    std::optional<size_t> instructionIndex;
    std::string message;
};

class VerificationResult {
public:
    bool ok() const { return errors_.empty(); }

    explicit operator bool() const { return ok(); }

    const std::vector<VerificationError>& errors() const { return errors_; }

    std::string format() const;

private:
    friend class ZirVerifier;
    std::vector<VerificationError> errors_;
};

class ZirVerifier {
public:
    VerificationResult verify(const Module& module) const;
    VerificationResult verifyForCodegen(const Module& module) const;
    VerificationResult verifyOwnershipObligations(const Module& module) const;
};

} // namespace zir
