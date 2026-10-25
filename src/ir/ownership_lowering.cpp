#include "ownership_lowering.hpp"

#include "call_contract.hpp"
#include "control_flow_graph.hpp"
#include "dead_phi_elimination.hpp"
#include "ownership_flow.hpp"
#include "ownership_liveness.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zir {
namespace {

std::shared_ptr<Value> instructionResult(const Instruction& instruction) {
    switch (instruction.getOpCode()) {
        case OpCode::Alloca:
            return static_cast<const AllocaInst&>(instruction).getResult();
        case OpCode::Load:
            return static_cast<const LoadInst&>(instruction).getResult();
        case OpCode::Add:
        case OpCode::Sub:
        case OpCode::Mul:
        case OpCode::SDiv:
        case OpCode::UDiv:
        case OpCode::SRem:
        case OpCode::URem:
        case OpCode::Shl:
        case OpCode::LShr:
        case OpCode::AShr:
        case OpCode::BitAnd:
        case OpCode::BitOr:
        case OpCode::BitXor:
            return static_cast<const BinaryInst&>(instruction).getResult();
        case OpCode::Cmp:
            return static_cast<const CmpInst&>(instruction).getResult();
        case OpCode::Call:
            return static_cast<const CallInst&>(instruction).getResult();
        case OpCode::Alloc:
            return static_cast<const AllocInst&>(instruction).getResult();
        case OpCode::GetElementPtr:
            return static_cast<const GetElementPtrInst&>(instruction).getResult();
        case OpCode::Phi:
            return static_cast<const PhiInst&>(instruction).getResult();
        case OpCode::Cast:
            return static_cast<const CastInst&>(instruction).getResult();
        case OpCode::Copy:
            return static_cast<const CopyInst&>(instruction).getResult();
        case OpCode::Move:
            return static_cast<const MoveInst&>(instruction).getResult();
        case OpCode::Borrow:
            return static_cast<const BorrowInst&>(instruction).getResult();
        case OpCode::WeakLock:
            return static_cast<const WeakLockInst&>(instruction).getResult();
        case OpCode::WeakAlive:
            return static_cast<const WeakAliveInst&>(instruction).getResult();
        case OpCode::ClassIs:
            return static_cast<const ClassIsInst&>(instruction).getResult();
        case OpCode::Store:
        case OpCode::Br:
        case OpCode::CondBr:
        case OpCode::Ret:
        case OpCode::Destroy:
        case OpCode::InlineAsm:
            return nullptr;
    }
    return nullptr;
}

bool ownsManagedValue(const std::shared_ptr<Value>& value) {
    return value && isOwned(value->getOwnership()) && containsManagedValues(value->getType());
}

size_t destroyInsertionIndex(const BasicBlock& block, size_t resultIndex) {
    if (block.getInstructions()[resultIndex]->getOpCode() != OpCode::Phi) {
        return resultIndex + 1;
    }
    size_t index = resultIndex + 1;
    while (index < block.getInstructions().size() && block.getInstructions()[index]
        && block.getInstructions()[index]->getOpCode() == OpCode::Phi) {
        ++index;
    }
    return index;
}

bool transfersOwnership(
    const Module& module,
    const Instruction& instruction,
    const std::shared_ptr<Value>& value
) {
    switch (instruction.getOpCode()) {
        case OpCode::Store: {
            const auto& store = static_cast<const StoreInst&>(instruction);
            return store.getSource() == value && isOwned(store.getSource()->getOwnership());
        }
        case OpCode::Ret: {
            const auto& ret = static_cast<const ReturnInst&>(instruction);
            return ret.getValue() == value && isOwned(ret.getValue()->getOwnership());
        }
        case OpCode::Cast: {
            const auto& cast = static_cast<const CastInst&>(instruction);
            return cast.getSource() == value && cast.getResult()
                && isOwned(cast.getResult()->getOwnership());
        }
        case OpCode::Call: {
            const auto& call = static_cast<const CallInst&>(instruction);
            for (size_t i = 0; i < call.getArguments().size(); ++i) {
                if (call.getArguments()[i] == value && callTransfersOwnership(module, call, i)) {
                    return true;
                }
            }
            return false;
        }
        case OpCode::Move:
            return static_cast<const MoveInst&>(instruction).getSource() == value;
        case OpCode::Destroy:
            return static_cast<const DestroyInst&>(instruction).getValue() == value;
        default:
            return false;
    }
}

void collectTransferredValues(
    const Module& module,
    const Instruction& instruction,
    std::unordered_set<const Value*>& values
) {
    switch (instruction.getOpCode()) {
        case OpCode::Store: {
            const auto& store = static_cast<const StoreInst&>(instruction);
            if (store.getSource() && isOwned(store.getSource()->getOwnership())) {
                values.insert(store.getSource().get());
            }
            return;
        }
        case OpCode::Ret: {
            const auto& ret = static_cast<const ReturnInst&>(instruction);
            if (ret.getValue() && isOwned(ret.getValue()->getOwnership())) {
                values.insert(ret.getValue().get());
            }
            return;
        }
        case OpCode::Cast: {
            const auto& cast = static_cast<const CastInst&>(instruction);
            if (cast.getSource() && cast.getResult() && isOwned(cast.getResult()->getOwnership())) {
                values.insert(cast.getSource().get());
            }
            return;
        }
        case OpCode::Call: {
            const auto& call = static_cast<const CallInst&>(instruction);
            for (size_t i = 0; i < call.getArguments().size(); ++i) {
                if (call.getArguments()[i] && callTransfersOwnership(module, call, i)) {
                    values.insert(call.getArguments()[i].get());
                }
            }
            return;
        }
        case OpCode::Move:
            values.insert(static_cast<const MoveInst&>(instruction).getSource().get());
            return;
        case OpCode::Destroy:
            values.insert(static_cast<const DestroyInst&>(instruction).getValue().get());
            return;
        default:
            return;
    }
}

struct PendingEdgeClosure {
    BasicBlock* source;
    BasicBlock* destination;
    std::vector<std::shared_ptr<Value>> values;
};

bool splitEdgeWithDestroys(
    BasicBlock& source,
    BasicBlock& destination,
    const std::vector<std::shared_ptr<Value>>& values,
    const char* labelPrefix,
    std::unordered_set<std::string>& labels,
    size_t& edgeIndex,
    std::vector<std::unique_ptr<BasicBlock>>& edgeBlocks
) {
    if (values.empty() || source.getInstructions().empty()) {
        return false;
    }
    const auto& terminator = source.getInstructions().back();
    if (!terminator) {
        return false;
    }
    if (terminator->getOpCode() == OpCode::Br) {
        if (static_cast<const BranchInst&>(*terminator).getTarget() != destination.label) {
            return false;
        }
    } else if (terminator->getOpCode() == OpCode::CondBr) {
        const auto& branch = static_cast<const CondBranchInst&>(*terminator);
        if (branch.getTrueLabel() != destination.label
            && branch.getFalseLabel() != destination.label) {
            return false;
        }
    } else {
        return false;
    }

    std::string edgeLabel;
    do {
        edgeLabel = std::string(labelPrefix) + std::to_string(edgeIndex++);
    } while (!labels.insert(edgeLabel).second);
    auto edge = std::make_unique<BasicBlock>(edgeLabel);
    for (const auto& value : values) {
        edge->addInstruction(std::make_unique<DestroyInst>(value));
    }
    edge->addInstruction(std::make_unique<BranchInst>(destination.label));
    if (terminator->getOpCode() == OpCode::Br) {
        static_cast<BranchInst&>(*terminator).setTarget(edgeLabel);
    } else {
        static_cast<CondBranchInst&>(*terminator).replaceTarget(destination.label, edgeLabel);
    }
    for (const auto& instruction : destination.getInstructions()) {
        if (instruction && instruction->getOpCode() == OpCode::Phi) {
            static_cast<PhiInst&>(*instruction).replaceIncomingLabel(source.label, edgeLabel);
        }
    }
    edgeBlocks.push_back(std::move(edge));
    return true;
}

void lowerUnambiguousOwnershipClosures(Module& module, Function& function) {
    std::unordered_map<const Value*, std::shared_ptr<Value>> values;
    for (const auto& argument : function.getArguments()) {
        if (argument) {
            values.emplace(argument.get(), argument);
        }
    }
    for (const auto& blockOwner : function.getBlocks()) {
        if (!blockOwner) {
            continue;
        }
        for (const auto& instruction : blockOwner->getInstructions()) {
            if (const auto value = instruction ? instructionResult(*instruction) : nullptr; value) {
                values.emplace(value.get(), value);
            }
        }
    }

    ControlFlowGraph cfg(function);
    OwnershipFlowAnalysis
        analysis(module, function, cfg.predecessors(), cfg.successors(), cfg.reachable());
    std::unordered_map<BasicBlock*, std::vector<std::pair<size_t, std::shared_ptr<Value>>>>
        destroys;
    std::vector<PendingEdgeClosure> criticalEdgeClosures;
    for (const auto& plan : analysis.analyzeOwnershipClosurePlans()) {
        for (const auto& placement : plan.destroyPlacements) {
            const auto value = values.find(plan.value);
            if (value == values.end()) {
                continue;
            }

            BasicBlock* block = nullptr;
            size_t insertionIndex = 0;
            if (placement.kind == OwnershipDestroyPlacementKind::BeforeReturn) {
                if (!placement.destination || !placement.instructionIndex) {
                    continue;
                }
                block = function.findBlock(placement.destination->label);
                insertionIndex = *placement.instructionIndex;
                const auto* returnInstruction =
                    block && insertionIndex < block->getInstructions().size()
                    ? block->getInstructions()[insertionIndex].get()
                    : nullptr;
                if (!returnInstruction || returnInstruction->getOpCode() != OpCode::Ret) {
                    continue;
                }
            } else if (placement.kind == OwnershipDestroyPlacementKind::OnEdge) {
                if (!placement.source || !placement.destination) {
                    continue;
                }
                block = function.findBlock(placement.source->label);
                const auto successors =
                    block ? cfg.successors().find(block) : cfg.successors().end();
                if (placement.requiresEdgeSplit) {
                    auto* destination = function.findBlock(placement.destination->label);
                    if (!block || !destination || successors == cfg.successors().end()
                        || successors->second.size() <= 1) {
                        continue;
                    }
                    auto closure = std::find_if(
                        criticalEdgeClosures.begin(),
                        criticalEdgeClosures.end(),
                        [&](const PendingEdgeClosure& candidate) {
                            return candidate.source == block
                                && candidate.destination == destination;
                        }
                    );
                    if (closure == criticalEdgeClosures.end()) {
                        criticalEdgeClosures.push_back({block, destination, {value->second}});
                    } else {
                        closure->values.push_back(value->second);
                    }
                    continue;
                }
                if (!block || successors == cfg.successors().end() || successors->second.size() != 1
                    || successors->second.front() != placement.destination
                    || block->getInstructions().empty()) {
                    continue;
                }
                insertionIndex = block->getInstructions().size() - 1;
                const auto* terminator = block->getInstructions().back().get();
                if (!terminator || terminator->getOpCode() != OpCode::Br) {
                    continue;
                }
            } else {
                continue;
            }
            destroys[block].emplace_back(insertionIndex, value->second);
        }
    }

    for (auto& [block, blockDestroys] : destroys) {
        std::sort(blockDestroys.begin(), blockDestroys.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.first < rhs.first;
        });
        auto& instructions = block->instructions;
        for (auto destroy = blockDestroys.rbegin(); destroy != blockDestroys.rend(); ++destroy) {
            instructions.insert(
                instructions.begin() + static_cast<std::ptrdiff_t>(destroy->first),
                std::make_unique<DestroyInst>(destroy->second)
            );
        }
    }

    std::unordered_set<std::string> labels;
    for (const auto& blockOwner : function.getBlocks()) {
        if (blockOwner) {
            labels.insert(blockOwner->label);
        }
    }
    size_t edgeIndex = 0;
    std::vector<std::unique_ptr<BasicBlock>> edgeBlocks;
    for (const auto& closure : criticalEdgeClosures) {
        splitEdgeWithDestroys(
            *closure.source,
            *closure.destination,
            closure.values,
            "ownership.close.",
            labels,
            edgeIndex,
            edgeBlocks
        );
    }
    for (auto& edge : edgeBlocks) {
        function.addBlock(std::move(edge));
    }
}

} // namespace

void lowerDeadOwnedResults(Module& module) {
    for (const auto& function : module.getFunctions()) {
        if (!function) {
            continue;
        }
        removeDeadPhiInstructions(*function);
        const auto liveness = analyzeOwnershipLiveness(module, *function);
        for (const auto& blockOwner : function->getBlocks()) {
            if (!blockOwner) {
                continue;
            }
            auto& instructions = blockOwner->instructions;
            std::vector<std::pair<size_t, std::shared_ptr<Value>>> destroys;
            std::vector<std::shared_ptr<Value>> ownedResults;
            std::unordered_set<const Value*> seenResults;
            std::unordered_set<const Value*> transferredBefore;
            for (size_t i = 0; i < instructions.size(); ++i) {
                if (!instructions[i]) {
                    continue;
                }
                const auto result = instructionResult(*instructions[i]);
                if (!ownsManagedValue(result)) {
                    continue;
                }
                if (seenResults.insert(result.get()).second) {
                    ownedResults.push_back(result);
                }
                if (!liveness.isLiveAfter(*blockOwner, i, result)) {
                    destroys.emplace_back(destroyInsertionIndex(*blockOwner, i), result);
                }
            }
            for (size_t i = 0; i < instructions.size(); ++i) {
                if (!instructions[i]) {
                    continue;
                }
                for (const auto& value : ownedResults) {
                    if (liveness.isLastUse(*blockOwner, i, value)
                        && !transfersOwnership(module, *instructions[i], value)
                        && transferredBefore.count(value.get()) == 0) {
                        destroys.emplace_back(destroyInsertionIndex(*blockOwner, i), value);
                    }
                }
                collectTransferredValues(module, *instructions[i], transferredBefore);
            }
            std::sort(destroys.begin(), destroys.end(), [](const auto& lhs, const auto& rhs) {
                return lhs.first < rhs.first;
            });
            for (auto destroy = destroys.rbegin(); destroy != destroys.rend(); ++destroy) {
                instructions.insert(
                    instructions.begin() + static_cast<std::ptrdiff_t>(destroy->first),
                    std::make_unique<DestroyInst>(destroy->second)
                );
            }
        }

        std::unordered_set<std::string> labels;
        for (const auto& blockOwner : function->getBlocks()) {
            if (blockOwner) {
                labels.insert(blockOwner->label);
            }
        }
        size_t edgeIndex = 0;
        std::vector<std::unique_ptr<BasicBlock>> edgeBlocks;
        for (const auto& sourceOwner : function->getBlocks()) {
            if (!sourceOwner || sourceOwner->getInstructions().empty()) {
                continue;
            }
            auto& source = *sourceOwner;
            const auto& terminator = source.getInstructions().back();
            if (!terminator
                || (terminator->getOpCode() != OpCode::Br
                    && terminator->getOpCode() != OpCode::CondBr)) {
                continue;
            }
            std::vector<std::string> targets;
            if (terminator->getOpCode() == OpCode::Br) {
                targets.push_back(static_cast<const BranchInst&>(*terminator).getTarget());
            } else {
                const auto& branch = static_cast<const CondBranchInst&>(*terminator);
                targets.push_back(branch.getTrueLabel());
                if (branch.getFalseLabel() != branch.getTrueLabel()) {
                    targets.push_back(branch.getFalseLabel());
                }
            }
            for (const auto& targetLabel : targets) {
                auto* destination = function->findBlock(targetLabel);
                if (!destination) {
                    continue;
                }
                std::vector<std::shared_ptr<Value>> destroys;
                std::unordered_set<const Value*> transferredValues;
                for (const auto& use : source.getInstructions()) {
                    if (use) {
                        collectTransferredValues(module, *use, transferredValues);
                    }
                }
                for (const auto& instruction : source.getInstructions()) {
                    if (!instruction) {
                        continue;
                    }
                    const auto value = instructionResult(*instruction);
                    if (!ownsManagedValue(value)
                        || liveness.isLiveOnEdge(source, *destination, value)) {
                        continue;
                    }
                    if (transferredValues.count(value.get()) == 0) {
                        destroys.push_back(value);
                    }
                }
                if (destroys.empty()) {
                    continue;
                }
                splitEdgeWithDestroys(
                    source,
                    *destination,
                    destroys,
                    "ownership.destroy.",
                    labels,
                    edgeIndex,
                    edgeBlocks
                );
            }
        }
        for (auto& edge : edgeBlocks) {
            function->addBlock(std::move(edge));
        }
        lowerUnambiguousOwnershipClosures(module, *function);
    }
}

} // namespace zir
