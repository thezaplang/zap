#pragma once

#include "function.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zir {

class ControlFlowGraph {
public:
    using BlockEdges = std::unordered_map<const BasicBlock*, std::vector<const BasicBlock*>>;

    explicit ControlFlowGraph(const Function& function);

    const BasicBlock* findBlock(const std::string& label) const;

    const BlockEdges& predecessors() const { return predecessors_; }

    const BlockEdges& successors() const { return successors_; }

    const std::unordered_set<const BasicBlock*>& reachable() const { return reachable_; }

    bool isReachable(const BasicBlock& block) const;
    bool dominates(const BasicBlock& dominator, const BasicBlock& block) const;

private:
    std::unordered_map<std::string, const BasicBlock*> blocks_;
    BlockEdges predecessors_;
    BlockEdges successors_;
    std::unordered_set<const BasicBlock*> reachable_;
    std::unordered_map<const BasicBlock*, size_t> reachableBlockIndices_;
    std::vector<std::vector<uint64_t>> dominators_;
};

} // namespace zir
