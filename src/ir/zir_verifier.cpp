#include "zir_verifier_internal.hpp"

#include "control_flow_graph.hpp"
#include "ownership_flow.hpp"

#include <sstream>
#include <unordered_set>

namespace zir {

namespace {

std::string formatDestroyPlacement(const OwnershipDestroyPlacement& placement) {
    if (placement.kind == OwnershipDestroyPlacementKind::BeforeReturn) {
        return "suggested destroy before return";
    }
    std::string result = "suggested destroy on edge %" + placement.source->label + " -> %"
        + placement.destination->label;
    if (placement.requiresEdgeSplit) {
        result += " (split critical edge)";
    }
    return result;
}

} // namespace

std::string VerificationResult::format() const {
    std::ostringstream output;
    for (size_t i = 0; i < errors_.size(); ++i) {
        const auto& error = errors_[i];
        if (i) {
            output << '\n';
        }
        output << "in function @" << error.function;
        if (!error.block.empty()) {
            output << ", block %" << error.block;
        }
        if (error.instructionIndex) {
            output << ", instruction " << *error.instructionIndex;
        }
        output << ": " << error.message;
    }
    return output.str();
}

namespace {

std::vector<VerificationError> verifyModule(
    const Module& module,
    bool includeOwnershipObligations
) {
    std::vector<VerificationError> errors;
    std::unordered_set<std::string> symbols;
    TypeInterner typeInterner;
    auto registerFunction = [&](const std::unique_ptr<Function>& function, bool external) {
        if (!function) {
            errors.push_back(
                {VerificationErrorCode::NullNode,
                    {},
                    {},
                    std::nullopt,
                    "module contains a null function"}
            );
            return;
        }
        if (!symbols.insert(function->name).second) {
            errors.push_back(
                {VerificationErrorCode::DuplicateSymbol,
                    function->name,
                    {},
                    std::nullopt,
                    "duplicate function symbol " + function->name}
            );
        }
        if (external) {
            if (!function->getBlocks().empty()) {
                errors.push_back(
                    {VerificationErrorCode::InvalidResult,
                        function->name,
                        {},
                        std::nullopt,
                        "external function must not have a body"}
                );
            }
            return;
        }
        verifier_detail::verifyDefinedFunction(
            module,
            *function,
            errors,
            typeInterner,
            includeOwnershipObligations
        );
    };

    for (const auto& function : module.getExternalFunctions()) {
        registerFunction(function, true);
    }
    for (const auto& function : module.getFunctions()) {
        registerFunction(function, false);
    }
    return errors;
}

} // namespace

VerificationResult ZirVerifier::verify(const Module& module) const {
    VerificationResult result;
    result.errors_ = verifyModule(module, false);
    return result;
}

VerificationResult ZirVerifier::verifyForCodegen(const Module& module) const {
    VerificationResult result;
    result.errors_ = verifyModule(module, true);
    return result;
}

VerificationResult ZirVerifier::verifyOwnershipObligations(const Module& module) const {
    VerificationResult result;
    for (const auto& function : module.getFunctions()) {
        if (!function) {
            continue;
        }
        ControlFlowGraph cfg(*function);
        OwnershipFlowAnalysis
            analysis(module, *function, cfg.predecessors(), cfg.successors(), cfg.reachable());
        for (const auto& plan : analysis.analyzeOwnershipClosurePlans()) {
            std::string definition = "function argument";
            if (plan.definition.block) {
                definition = "block %" + plan.definition.block->label;
                if (plan.definition.instructionIndex) {
                    definition +=
                        " instruction " + std::to_string(*plan.definition.instructionIndex);
                }
            }
            for (const auto& obligation : plan.liveExits) {
                std::string placement = "no safe destroy placement determined";
                for (const auto& candidate : plan.destroyPlacements) {
                    if (candidate.destination != obligation.block
                        || (candidate.kind == OwnershipDestroyPlacementKind::BeforeReturn
                            && candidate.instructionIndex != obligation.instructionIndex)) {
                        continue;
                    }
                    placement = formatDestroyPlacement(candidate);
                    break;
                }
                result.errors_.push_back(
                    {VerificationErrorCode::OwnershipViolation,
                        function->name,
                        obligation.block ? obligation.block->label : std::string{},
                        obligation.instructionIndex,
                        "owned value may remain live at function exit: " + plan.value->getName()
                            + " (defined in " + definition + "; " + "state: "
                            + formatOwnershipFlowState(obligation.state) + "; " + placement + ")"}
                );
            }
        }
    }
    return result;
}

} // namespace zir
