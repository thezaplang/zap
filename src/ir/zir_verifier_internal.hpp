#pragma once

#include "type_identity.hpp"
#include "zir_verifier.hpp"

namespace zir::verifier_detail {

bool isAssignable(
    const std::shared_ptr<Type>& actual,
    const std::shared_ptr<Type>& expected,
    TypeInterner& typeInterner
);
bool isTerminator(OpCode opcode);
bool isStringType(const std::shared_ptr<Type>& type);
std::shared_ptr<Value> instructionResult(const Instruction& instruction);
std::string typeName(const std::shared_ptr<Type>& type);

void verifyDefinedFunction(
    const Module& module,
    const Function& function,
    std::vector<VerificationError>& errors,
    TypeInterner& typeInterner,
    bool includeOwnershipObligations
);

} // namespace zir::verifier_detail
