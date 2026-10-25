#include "ownership_liveness.hpp"

#include "borrow_provenance.hpp"
#include "control_flow_graph.hpp"

#include <algorithm>
#include <unordered_map>
#include <utility>
#include <vector>

namespace zir {
namespace {

using ValueSet = std::unordered_set<const Value*>;

bool tracksOwnership(const std::shared_ptr<Value>& value) {
    return value && isOwned(value->getOwnership()) && containsManagedValues(value->getType());
}

void addUse(
    ValueSet& values,
    const std::shared_ptr<Value>& value,
    const BorrowProvenance& provenance
) {
    if (tracksOwnership(value)) {
        values.insert(value.get());
    }
    const auto& owners = provenance.ownersOf(value);
    values.insert(owners.begin(), owners.end());
}

void addInstructionUses(
    ValueSet& values,
    const Instruction& instruction,
    const BorrowProvenance& provenance
) {
    switch (instruction.getOpCode()) {
        case OpCode::Alloca:
        case OpCode::Alloc:
        case OpCode::Br:
        case OpCode::Phi:
            return;
        case OpCode::Load:
            addUse(values, static_cast<const LoadInst&>(instruction).getSource(), provenance);
            return;
        case OpCode::Store: {
            const auto& store = static_cast<const StoreInst&>(instruction);
            addUse(values, store.getSource(), provenance);
            addUse(values, store.getDestination(), provenance);
            return;
        }
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
        case OpCode::BitXor: {
            const auto& binary = static_cast<const BinaryInst&>(instruction);
            addUse(values, binary.getLhs(), provenance);
            addUse(values, binary.getRhs(), provenance);
            return;
        }
        case OpCode::Cmp: {
            const auto& comparison = static_cast<const CmpInst&>(instruction);
            addUse(values, comparison.getLhs(), provenance);
            addUse(values, comparison.getRhs(), provenance);
            return;
        }
        case OpCode::CondBr:
            addUse(
                values,
                static_cast<const CondBranchInst&>(instruction).getCondition(),
                provenance
            );
            return;
        case OpCode::Ret:
            addUse(values, static_cast<const ReturnInst&>(instruction).getValue(), provenance);
            return;
        case OpCode::Call: {
            const auto& call = static_cast<const CallInst&>(instruction);
            addUse(values, call.getCalleeValue(), provenance);
            for (const auto& argument : call.getArguments()) {
                addUse(values, argument, provenance);
            }
            addUse(values, call.getVariadicPack(), provenance);
            return;
        }
        case OpCode::Copy:
            addUse(values, static_cast<const CopyInst&>(instruction).getSource(), provenance);
            return;
        case OpCode::Move:
            addUse(values, static_cast<const MoveInst&>(instruction).getSource(), provenance);
            return;
        case OpCode::Borrow:
            addUse(values, static_cast<const BorrowInst&>(instruction).getOwner(), provenance);
            return;
        case OpCode::Destroy:
            addUse(values, static_cast<const DestroyInst&>(instruction).getValue(), provenance);
            return;
        case OpCode::GetElementPtr: {
            const auto& gep = static_cast<const GetElementPtrInst&>(instruction);
            addUse(values, gep.getPointer(), provenance);
            addUse(values, gep.getIndexValue(), provenance);
            return;
        }
        case OpCode::Cast:
            addUse(values, static_cast<const CastInst&>(instruction).getSource(), provenance);
            return;
        case OpCode::WeakLock:
            addUse(
                values,
                static_cast<const WeakLockInst&>(instruction).getWeakValue(),
                provenance
            );
            return;
        case OpCode::WeakAlive:
            addUse(
                values,
                static_cast<const WeakAliveInst&>(instruction).getWeakValue(),
                provenance
            );
            return;
        case OpCode::ClassIs:
            addUse(values, static_cast<const ClassIsInst&>(instruction).getObject(), provenance);
            return;
        case OpCode::InlineAsm: {
            const auto& inlineAsm = static_cast<const InlineAsmInst&>(instruction);
            for (const auto& operand : inlineAsm.getOutputs()) {
                addUse(values, operand.value, provenance);
            }
            for (const auto& operand : inlineAsm.getInputs()) {
                addUse(values, operand.value, provenance);
            }
            return;
        }
    }
}

bool instructionUsesValue(
    const Instruction& instruction,
    const Value* value,
    const BorrowProvenance& provenance
) {
    if (!value) {
        return false;
    }
    ValueSet uses;
    addInstructionUses(uses, instruction, provenance);
    return uses.count(value) != 0;
}

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

void unionInto(ValueSet& destination, const ValueSet& source) {
    destination.insert(source.begin(), source.end());
}

void replacePhiResultWithIncoming(
    ValueSet& live,
    const std::shared_ptr<Value>& result,
    const std::shared_ptr<Value>& incoming,
    const BorrowProvenance& provenance
) {
    if (!result) {
        return;
    }

    if (tracksOwnership(result)) {
        live.erase(result.get());
    }
    for (const auto* owner : provenance.ownersOf(result)) {
        live.erase(owner);
    }
    addUse(live, incoming, provenance);
}

void replaceLoadResultWithStorage(
    ValueSet& live,
    const LoadInst& load,
    const BasicBlock& source,
    const BasicBlock& destination,
    const BorrowProvenance& provenance
) {
    if (!load.getResult() || !load.getSource()) {
        return;
    }
    for (const auto* owner : provenance.ownersOf(load.getResult())) {
        live.erase(owner);
    }
    const auto actualOwners = provenance.ownersOnEdge(load.getResult(), source, destination);
    live.insert(actualOwners.begin(), actualOwners.end());
}

} // namespace

bool OwnershipLiveness::isLiveAtBlockEntry(
    const BasicBlock& block,
    const std::shared_ptr<Value>& value
) const {
    const auto states = entryStates_.find(&block);
    return value && states != entryStates_.end() && states->second.count(value.get()) != 0;
}

bool OwnershipLiveness::isLiveAfter(
    const BasicBlock& block,
    size_t instructionIndex,
    const std::shared_ptr<Value>& value
) const {
    const auto blockStates = afterStates_.find(&block);
    if (!value || blockStates == afterStates_.end()) {
        return false;
    }
    const auto states = blockStates->second.find(instructionIndex);
    return states != blockStates->second.end() && states->second.count(value.get()) != 0;
}

bool OwnershipLiveness::isLastUse(
    const BasicBlock& block,
    size_t instructionIndex,
    const std::shared_ptr<Value>& value
) const {
    if (!value || instructionIndex >= block.getInstructions().size()
        || !block.getInstructions()[instructionIndex]) {
        return false;
    }
    return instructionUsesValue(
               *block.getInstructions()[instructionIndex],
               value.get(),
               borrowProvenance_
           )
        && !isLiveAfter(block, instructionIndex, value);
}

bool OwnershipLiveness::isLiveOnEdge(
    const BasicBlock& source,
    const BasicBlock& destination,
    const std::shared_ptr<Value>& value
) const {
    const auto sourceStates = edgeStates_.find(&source);
    if (!value || sourceStates == edgeStates_.end()) {
        return false;
    }
    const auto states = sourceStates->second.find(&destination);
    return states != sourceStates->second.end() && states->second.count(value.get()) != 0;
}

OwnershipLiveness analyzeOwnershipLiveness(const Module& module, const Function& function) {
    const ControlFlowGraph cfg(function);
    return analyzeOwnershipLiveness(module, function, cfg);
}

OwnershipLiveness analyzeOwnershipLiveness(
    const Module& module,
    const Function& function,
    const ControlFlowGraph& cfg
) {
    OwnershipLiveness result;
    result.borrowProvenance_ = analyzeBorrowProvenance(module, function, cfg);

    bool changed = true;
    while (changed) {
        changed = false;
        for (auto blockIt = function.getBlocks().rbegin(); blockIt != function.getBlocks().rend();
            ++blockIt) {
            if (!*blockIt) {
                continue;
            }
            const auto& block = **blockIt;
            ValueSet liveOut;
            for (const auto* successor : cfg.successors().at(&block)) {
                ValueSet edgeState = result.entryStates_[successor];
                for (const auto& instruction : successor->getInstructions()) {
                    if (!instruction || instruction->getOpCode() != OpCode::Phi) {
                        continue;
                    }
                    const auto& phi = static_cast<const PhiInst&>(*instruction);
                    for (const auto& [label, value] : phi.getIncoming()) {
                        if (label == block.label) {
                            replacePhiResultWithIncoming(
                                edgeState,
                                phi.getResult(),
                                value,
                                result.borrowProvenance_
                            );
                            break;
                        }
                    }
                }
                for (const auto& instruction : successor->getInstructions()) {
                    if (!instruction || instruction->getOpCode() != OpCode::Load) {
                        continue;
                    }
                    const auto& load = static_cast<const LoadInst&>(*instruction);
                    if (result.borrowProvenance_.isEntryLoad(*successor, load.getResult())) {
                        replaceLoadResultWithStorage(
                            edgeState,
                            load,
                            block,
                            *successor,
                            result.borrowProvenance_
                        );
                    }
                }
                if (result.edgeStates_[&block][successor] != edgeState) {
                    result.edgeStates_[&block][successor] = edgeState;
                    changed = true;
                }
                unionInto(liveOut, edgeState);
            }

            ValueSet live = std::move(liveOut);
            OwnershipLiveness::InstructionStates afterStates;
            for (size_t i = block.getInstructions().size(); i-- > 0;) {
                const auto& instruction = block.getInstructions()[i];
                if (!instruction) {
                    continue;
                }
                afterStates.emplace(i, live);
                if (const auto value = instructionResult(*instruction); tracksOwnership(value)) {
                    live.erase(value.get());
                }
                addInstructionUses(live, *instruction, result.borrowProvenance_);
            }
            if (result.entryStates_[&block] != live) {
                result.entryStates_[&block] = std::move(live);
                changed = true;
            }
            result.afterStates_[&block] = std::move(afterStates);
        }
    }
    return result;
}

} // namespace zir
