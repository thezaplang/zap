#include "dead_phi_elimination.hpp"

#include <algorithm>
#include <unordered_set>

namespace zir {
namespace {

template <typename Visitor> void visitValue(const std::shared_ptr<Value>& value, Visitor& visitor) {
    if (value) {
        visitor(value.get());
    }
}

template <typename Visitor>
void visitInstructionOperands(const Instruction& instruction, Visitor& visitor) {
    switch (instruction.getOpCode()) {
        case OpCode::Alloca:
        case OpCode::Alloc:
        case OpCode::Br:
            return;
        case OpCode::Load:
            visitValue(static_cast<const LoadInst&>(instruction).getSource(), visitor);
            return;
        case OpCode::Store: {
            const auto& store = static_cast<const StoreInst&>(instruction);
            visitValue(store.getSource(), visitor);
            visitValue(store.getDestination(), visitor);
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
            visitValue(binary.getLhs(), visitor);
            visitValue(binary.getRhs(), visitor);
            return;
        }
        case OpCode::Cmp: {
            const auto& comparison = static_cast<const CmpInst&>(instruction);
            visitValue(comparison.getLhs(), visitor);
            visitValue(comparison.getRhs(), visitor);
            return;
        }
        case OpCode::CondBr:
            visitValue(static_cast<const CondBranchInst&>(instruction).getCondition(), visitor);
            return;
        case OpCode::Ret:
            visitValue(static_cast<const ReturnInst&>(instruction).getValue(), visitor);
            return;
        case OpCode::Call: {
            const auto& call = static_cast<const CallInst&>(instruction);
            visitValue(call.getCalleeValue(), visitor);
            visitValue(call.getVariadicPack(), visitor);
            for (const auto& argument : call.getArguments()) {
                visitValue(argument, visitor);
            }
            return;
        }
        case OpCode::Copy:
            visitValue(static_cast<const CopyInst&>(instruction).getSource(), visitor);
            return;
        case OpCode::Move:
            visitValue(static_cast<const MoveInst&>(instruction).getSource(), visitor);
            return;
        case OpCode::Borrow:
            visitValue(static_cast<const BorrowInst&>(instruction).getOwner(), visitor);
            return;
        case OpCode::Destroy:
            visitValue(static_cast<const DestroyInst&>(instruction).getValue(), visitor);
            return;
        case OpCode::GetElementPtr: {
            const auto& gep = static_cast<const GetElementPtrInst&>(instruction);
            visitValue(gep.getPointer(), visitor);
            visitValue(gep.getIndexValue(), visitor);
            return;
        }
        case OpCode::Phi: {
            const auto& phi = static_cast<const PhiInst&>(instruction);
            for (const auto& incoming : phi.getIncoming()) {
                visitValue(incoming.second, visitor);
            }
            return;
        }
        case OpCode::Cast:
            visitValue(static_cast<const CastInst&>(instruction).getSource(), visitor);
            return;
        case OpCode::WeakLock:
            visitValue(static_cast<const WeakLockInst&>(instruction).getWeakValue(), visitor);
            return;
        case OpCode::WeakAlive:
            visitValue(static_cast<const WeakAliveInst&>(instruction).getWeakValue(), visitor);
            return;
        case OpCode::ClassIs:
            visitValue(static_cast<const ClassIsInst&>(instruction).getObject(), visitor);
            return;
        case OpCode::InlineAsm: {
            const auto& inlineAsm = static_cast<const InlineAsmInst&>(instruction);
            for (const auto& output : inlineAsm.getOutputs()) {
                visitValue(output.value, visitor);
            }
            for (const auto& input : inlineAsm.getInputs()) {
                visitValue(input.value, visitor);
            }
            return;
        }
    }
}

std::unordered_set<const Value*> collectUsedValues(const Function& function) {
    std::unordered_set<const Value*> phiResults;
    for (const auto& block : function.getBlocks()) {
        if (!block) {
            continue;
        }
        for (const auto& instruction : block->getInstructions()) {
            if (!instruction || instruction->getOpCode() != OpCode::Phi) {
                continue;
            }
            const auto& result = static_cast<const PhiInst&>(*instruction).getResult();
            if (result) {
                phiResults.insert(result.get());
            }
        }
    }

    std::unordered_set<const Value*> used;
    for (const auto& block : function.getBlocks()) {
        if (!block) {
            continue;
        }
        for (const auto& candidate : block->getInstructions()) {
            if (!candidate) {
                continue;
            }
            auto recordPhiUse = [&](const Value* operand) {
                if (phiResults.count(operand) != 0) {
                    used.insert(operand);
                }
            };
            visitInstructionOperands(*candidate, recordPhiUse);
        }
    }
    return used;
}

} // namespace

void removeDeadPhiInstructions(Function& function) {
    bool changed = true;
    while (changed) {
        changed = false;
        const auto used = collectUsedValues(function);
        for (const auto& block : function.getBlocks()) {
            if (!block) {
                continue;
            }
            auto& instructions = block->instructions;
            const auto newEnd = std::remove_if(
                instructions.begin(),
                instructions.end(),
                [&](const std::unique_ptr<Instruction>& instruction) {
                    if (!instruction || instruction->getOpCode() != OpCode::Phi) {
                        return false;
                    }
                    const auto& result = static_cast<const PhiInst&>(*instruction).getResult();
                    const bool dead = result && used.count(result.get()) == 0;
                    changed = changed || dead;
                    return dead;
                }
            );
            instructions.erase(newEnd, instructions.end());
        }
    }
}

} // namespace zir
