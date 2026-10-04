#pragma once

#include "control_flow_graph.hpp"
#include "zir_verifier.hpp"

namespace zir::verifier_detail {

std::vector<VerificationError> verifyBorrowContracts(
    const Module& module,
    const Function& function,
    const ControlFlowGraph& cfg
);

} // namespace zir::verifier_detail
