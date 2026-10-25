#include "control_flow_graph.hpp"

#include <algorithm>
#include <deque>

namespace zir {

ControlFlowGraph::ControlFlowGraph(const Function& function) {
    for (const auto& blockOwner : function.getBlocks()) {
        if (!blockOwner) {
            continue;
        }
        blocks_.emplace(blockOwner->label, blockOwner.get());
        predecessors_.try_emplace(blockOwner.get());
        successors_.try_emplace(blockOwner.get());
    }
    for (const auto& blockOwner : function.getBlocks()) {
        if (!blockOwner) {
            continue;
        }
        const auto& block = *blockOwner;
        auto addEdge = [&](const std::string& label) {
            const auto* target = findBlock(label);
            if (!target) {
                return;
            }
            successors_[&block].push_back(target);
            predecessors_[target].push_back(&block);
        };
        for (const auto& instruction : block.getInstructions()) {
            if (!instruction) {
                continue;
            }
            if (instruction->getOpCode() == OpCode::Br) {
                addEdge(static_cast<const BranchInst&>(*instruction).getTarget());
            } else if (instruction->getOpCode() == OpCode::CondBr) {
                const auto& branch = static_cast<const CondBranchInst&>(*instruction);
                addEdge(branch.getTrueLabel());
                addEdge(branch.getFalseLabel());
            }
        }
    }
    if (function.getBlocks().empty() || !function.getBlocks().front()) {
        return;
    }
    std::deque<const BasicBlock*> worklist{function.getBlocks().front().get()};
    reachable_.insert(worklist.front());
    while (!worklist.empty()) {
        const auto* block = worklist.front();
        worklist.pop_front();
        for (const auto* successor : successors_.at(block)) {
            if (reachable_.insert(successor).second) {
                worklist.push_back(successor);
            }
        }
    }
    const auto* entry = function.getBlocks().front().get();
    for (const auto& blockOwner : function.getBlocks()) {
        if (blockOwner && isReachable(*blockOwner)) {
            reachableBlockIndices_.emplace(blockOwner.get(), reachableBlockIndices_.size());
        }
    }

    const size_t blockCount = reachableBlockIndices_.size();
    const size_t wordCount = (blockCount + 63) / 64;
    const std::vector<uint64_t> allDominators(wordCount, ~uint64_t{0});
    dominators_.assign(blockCount, allDominators);

    const auto entryIndex = reachableBlockIndices_.at(entry);
    std::fill(dominators_[entryIndex].begin(), dominators_[entryIndex].end(), uint64_t{0});
    dominators_[entryIndex][entryIndex / 64] |= uint64_t{1} << (entryIndex % 64);

    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& blockOwner : function.getBlocks()) {
            if (!blockOwner || !isReachable(*blockOwner)) {
                continue;
            }
            const auto* block = blockOwner.get();
            if (block == entry) {
                continue;
            }
            std::vector<uint64_t> dominators(wordCount);
            bool firstPredecessor = true;
            for (const auto* predecessor : predecessors_.at(block)) {
                if (!isReachable(*predecessor)) {
                    continue;
                }
                const auto predecessorIndex = reachableBlockIndices_.at(predecessor);
                if (firstPredecessor) {
                    dominators = dominators_[predecessorIndex];
                    firstPredecessor = false;
                    continue;
                }
                for (size_t word = 0; word < wordCount; ++word) {
                    dominators[word] &= dominators_[predecessorIndex][word];
                }
            }
            const auto blockIndex = reachableBlockIndices_.at(block);
            dominators[blockIndex / 64] |= uint64_t{1} << (blockIndex % 64);
            if (dominators_[blockIndex] != dominators) {
                dominators_[blockIndex] = std::move(dominators);
                changed = true;
            }
        }
    }
}

const BasicBlock* ControlFlowGraph::findBlock(const std::string& label) const {
    const auto block = blocks_.find(label);
    return block == blocks_.end() ? nullptr : block->second;
}

bool ControlFlowGraph::isReachable(const BasicBlock& block) const {
    return reachable_.count(&block) != 0;
}

bool ControlFlowGraph::dominates(const BasicBlock& dominator, const BasicBlock& block) const {
    const auto dominatorIndex = reachableBlockIndices_.find(&dominator);
    const auto blockIndex = reachableBlockIndices_.find(&block);
    if (dominatorIndex == reachableBlockIndices_.end()
        || blockIndex == reachableBlockIndices_.end()) {
        return false;
    }
    const auto index = dominatorIndex->second;
    return (dominators_[blockIndex->second][index / 64] & (uint64_t{1} << (index % 64))) != 0;
}

} // namespace zir
