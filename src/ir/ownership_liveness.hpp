#pragma once

#include "borrow_provenance.hpp"
#include "function.hpp"

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace zir {

class Module;
class ControlFlowGraph;

class OwnershipLiveness {
public:
    bool isLiveAtBlockEntry(const BasicBlock& block, const std::shared_ptr<Value>& value) const;
    bool isLiveAfter(
        const BasicBlock& block,
        size_t instructionIndex,
        const std::shared_ptr<Value>& value
    ) const;
    bool isLastUse(
        const BasicBlock& block,
        size_t instructionIndex,
        const std::shared_ptr<Value>& value
    ) const;
    bool isLiveOnEdge(
        const BasicBlock& source,
        const BasicBlock& destination,
        const std::shared_ptr<Value>& value
    ) const;

private:
    using ValueSet = std::unordered_set<const Value*>;
    using InstructionStates = std::unordered_map<size_t, ValueSet>;
    using EdgeStates =
        std::unordered_map<const BasicBlock*, std::unordered_map<const BasicBlock*, ValueSet>>;

    std::unordered_map<const BasicBlock*, ValueSet> entryStates_;
    std::unordered_map<const BasicBlock*, InstructionStates> afterStates_;
    EdgeStates edgeStates_;
    BorrowProvenance borrowProvenance_;

    friend OwnershipLiveness analyzeOwnershipLiveness(
        const Module& module,
        const Function& function
    );
    friend OwnershipLiveness analyzeOwnershipLiveness(
        const Module& module,
        const Function& function,
        const ControlFlowGraph& cfg
    );
};

OwnershipLiveness analyzeOwnershipLiveness(const Module& module, const Function& function);
OwnershipLiveness analyzeOwnershipLiveness(
    const Module& module,
    const Function& function,
    const ControlFlowGraph& cfg
);

} // namespace zir
