#pragma once

#include "function.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zir {

class Module;

enum class OwnershipFlowState : unsigned char {
    Unavailable = 1 << 0,
    Live = 1 << 1,
    Moved = 1 << 2,
    Destroyed = 1 << 3,
    Mixed = Unavailable | Live | Moved | Destroyed,
};

std::string formatOwnershipFlowState(OwnershipFlowState state);

struct OwnershipTransferViolation {
    const BasicBlock* block;
    size_t instructionIndex;
    std::shared_ptr<Value> value;
    std::string operation;
    OwnershipFlowState priorState;
};

struct OwnershipExitObligation {
    const BasicBlock* block;
    size_t instructionIndex;
    const Value* value;
    OwnershipFlowState state;
};

struct OwnershipDefinitionSite {
    const BasicBlock* block;
    std::optional<size_t> instructionIndex;
};

enum class OwnershipDestroyPlacementKind {
    BeforeReturn,
    OnEdge,
};

struct OwnershipDestroyPlacement {
    OwnershipDestroyPlacementKind kind;
    const BasicBlock* source;
    const BasicBlock* destination;
    std::optional<size_t> instructionIndex;
    bool requiresEdgeSplit;
};

struct OwnershipClosurePlan {
    const Value* value;
    OwnershipDefinitionSite definition;
    std::vector<OwnershipExitObligation> liveExits;
    std::vector<OwnershipDestroyPlacement> destroyPlacements;
};

class OwnershipFlowAnalysis {
public:
    using BlockEdges = std::unordered_map<const BasicBlock*, std::vector<const BasicBlock*>>;

    OwnershipFlowAnalysis(
        const Module& module,
        const Function& function,
        const BlockEdges& predecessors,
        const BlockEdges& successors,
        const std::unordered_set<const BasicBlock*>& reachable
    );

    std::vector<OwnershipTransferViolation> analyze();
    std::vector<OwnershipExitObligation> analyzeExitObligations();
    std::vector<OwnershipClosurePlan> analyzeOwnershipClosurePlans();
    OwnershipFlowState stateOnEdge(
        const BasicBlock& source,
        const BasicBlock& destination,
        const std::shared_ptr<Value>& value
    ) const;
    OwnershipFlowState stateOnEdge(
        const BasicBlock& source,
        const BasicBlock& destination,
        const Value* value
    ) const;

private:
    using OwnershipStates = std::unordered_map<const Value*, unsigned char>;
    using OwnershipEdgeStates = std::
        unordered_map<const BasicBlock*, std::unordered_map<const BasicBlock*, OwnershipStates>>;
    using ReturnStates =
        std::unordered_map<const BasicBlock*, std::unordered_map<size_t, OwnershipStates>>;

    const Module& module_;
    const Function& function_;
    const BlockEdges& predecessors_;
    const BlockEdges& successors_;
    std::unordered_set<const BasicBlock*> reachable_;
    OwnershipEdgeStates edgeStates_;
    ReturnStates returnStates_;
    std::vector<OwnershipTransferViolation> violations_;
    bool analyzed_ = false;
};

} // namespace zir
