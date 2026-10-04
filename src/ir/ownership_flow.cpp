#include "ownership_flow.hpp"

#include "call_contract.hpp"
#include "module.hpp"

#include <algorithm>
#include <functional>
#include <vector>

namespace zir {
namespace {

constexpr unsigned char live = static_cast<unsigned char>(OwnershipFlowState::Live);
constexpr unsigned char moved = static_cast<unsigned char>(OwnershipFlowState::Moved);
constexpr unsigned char destroyed = static_cast<unsigned char>(OwnershipFlowState::Destroyed);
constexpr unsigned char unavailable = static_cast<unsigned char>(OwnershipFlowState::Unavailable);

bool ownsManagedValue(const std::shared_ptr<Value>& value) {
    return value && isOwned(value->getOwnership()) && containsManagedValues(value->getType());
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

std::vector<const Value*> collectOwnedValues(const Function& function) {
    std::vector<const Value*> values;
    std::unordered_set<const Value*> seen;
    auto add = [&](const std::shared_ptr<Value>& value) {
        if (ownsManagedValue(value) && seen.insert(value.get()).second) {
            values.push_back(value.get());
        }
    };
    for (const auto& argument : function.getArguments()) {
        add(argument);
    }
    for (const auto& blockOwner : function.getBlocks()) {
        if (!blockOwner) {
            continue;
        }
        for (const auto& instruction : blockOwner->getInstructions()) {
            if (instruction) {
                add(instructionResult(*instruction));
            }
        }
    }
    return values;
}

bool transfersThroughCast(const CastInst& cast) {
    return ownsManagedValue(cast.getSource()) && cast.getTargetType() && cast.getResult()
        && isOwned(cast.getResult()->getOwnership());
}

} // namespace

std::string formatOwnershipFlowState(OwnershipFlowState state) {
    const auto bits = static_cast<unsigned char>(state);
    std::string result;
    auto append = [&](unsigned char flag, const char* name) {
        if ((bits & flag) == 0) {
            return;
        }
        if (!result.empty()) {
            result += "|";
        }
        result += name;
    };
    append(unavailable, "Unavailable");
    append(live, "Live");
    append(moved, "Moved");
    append(destroyed, "Destroyed");
    return result.empty() ? "None" : result;
}

OwnershipFlowAnalysis::OwnershipFlowAnalysis(
    const Module& module,
    const Function& function,
    const BlockEdges& predecessors,
    const BlockEdges& successors,
    const std::unordered_set<const BasicBlock*>& reachable
)
    : module_(module),
      function_(function),
      predecessors_(predecessors),
      successors_(successors),
      reachable_(reachable) {}

OwnershipFlowState OwnershipFlowAnalysis::stateOnEdge(
    const BasicBlock& source,
    const BasicBlock& destination,
    const std::shared_ptr<Value>& value
) const {
    return stateOnEdge(source, destination, value.get());
}

OwnershipFlowState OwnershipFlowAnalysis::stateOnEdge(
    const BasicBlock& source,
    const BasicBlock& destination,
    const Value* value
) const {
    const auto sourceStates = edgeStates_.find(&source);
    if (!value || sourceStates == edgeStates_.end()) {
        return OwnershipFlowState::Unavailable;
    }
    const auto destinationStates = sourceStates->second.find(&destination);
    if (destinationStates == sourceStates->second.end()) {
        return OwnershipFlowState::Unavailable;
    }
    const auto state = destinationStates->second.find(value);
    return state == destinationStates->second.end()
        ? OwnershipFlowState::Unavailable
        : static_cast<OwnershipFlowState>(state->second);
}

std::vector<OwnershipTransferViolation> OwnershipFlowAnalysis::analyze() {
    if (analyzed_) {
        return violations_;
    }

    edgeStates_.clear();
    returnStates_.clear();
    const auto ownedValues = collectOwnedValues(function_);
    OwnershipStates entryStates;
    for (const auto* value : ownedValues) {
        entryStates[value] = unavailable;
    }
    for (const auto& argument : function_.getArguments()) {
        if (ownsManagedValue(argument)) {
            entryStates[argument.get()] = live;
        }
    }

    std::vector<OwnershipTransferViolation> violations;
    std::unordered_set<std::string> reported;
    auto transition = [&](OwnershipStates& states,
                          const std::shared_ptr<Value>& value,
                          const BasicBlock& block,
                          size_t instructionIndex,
                          const char* operation,
                          unsigned char nextState) {
        if (!ownsManagedValue(value)) {
            return;
        }
        auto& state = states[value.get()];
        if (state != live) {
            const auto key =
                block.label + ":" + std::to_string(instructionIndex) + ":" + value->getName();
            if (reported.insert(key).second) {
                violations.push_back(
                    {&block,
                        instructionIndex,
                        value,
                        std::string(operation),
                        static_cast<OwnershipFlowState>(state)}
                );
            }
        }
        state = nextState;
    };
    auto requireLive = [&](const OwnershipStates& states,
                           const std::shared_ptr<Value>& value,
                           const BasicBlock& block,
                           size_t instructionIndex,
                           const char* operation) {
        if (!ownsManagedValue(value)) {
            return;
        }
        const auto state = states.find(value.get());
        const auto currentState = state == states.end() ? unavailable : state->second;
        if (currentState == live) {
            return;
        }
        const auto key =
            block.label + ":" + std::to_string(instructionIndex) + ":" + value->getName();
        if (reported.insert(key).second) {
            violations.push_back(
                {&block,
                    instructionIndex,
                    value,
                    std::string(operation),
                    static_cast<OwnershipFlowState>(currentState)}
            );
        }
    };

    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& blockOwner : function_.getBlocks()) {
            if (!blockOwner || reachable_.count(blockOwner.get()) == 0) {
                continue;
            }
            const auto& block = *blockOwner;
            OwnershipStates states;
            if (!function_.getBlocks().empty() && function_.getBlocks().front().get() == &block) {
                states = entryStates;
            } else {
                const auto predecessors = predecessors_.find(&block);
                if (predecessors != predecessors_.end()) {
                    for (const auto* predecessor : predecessors->second) {
                        if (reachable_.count(predecessor) == 0) {
                            continue;
                        }
                        const auto sourceStates = edgeStates_.find(predecessor);
                        if (sourceStates == edgeStates_.end()) {
                            continue;
                        }
                        const auto destinationStates = sourceStates->second.find(&block);
                        if (destinationStates == sourceStates->second.end()) {
                            continue;
                        }
                        for (const auto* value : ownedValues) {
                            states[value] |= destinationStates->second.at(value);
                        }
                    }
                }
            }
            for (size_t i = 0; i < block.getInstructions().size(); ++i) {
                const auto& instruction = block.getInstructions()[i];
                if (!instruction) {
                    continue;
                }
                switch (instruction->getOpCode()) {
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
                        const auto& binary = static_cast<const BinaryInst&>(*instruction);
                        requireLive(states, binary.getLhs(), block, i, "binary operation");
                        requireLive(states, binary.getRhs(), block, i, "binary operation");
                        break;
                    }
                    case OpCode::Cmp: {
                        const auto& comparison = static_cast<const CmpInst&>(*instruction);
                        requireLive(states, comparison.getLhs(), block, i, "comparison");
                        requireLive(states, comparison.getRhs(), block, i, "comparison");
                        break;
                    }
                    case OpCode::Store: {
                        const auto& store = static_cast<const StoreInst&>(*instruction);
                        if (store.getSource() && isOwned(store.getSource()->getOwnership())) {
                            transition(states, store.getSource(), block, i, "store", moved);
                        }
                        break;
                    }
                    case OpCode::Ret: {
                        const auto& ret = static_cast<const ReturnInst&>(*instruction);
                        if (ret.getValue() && isOwned(ret.getValue()->getOwnership())) {
                            transition(states, ret.getValue(), block, i, "return", moved);
                        }
                        break;
                    }
                    case OpCode::Cast: {
                        const auto& cast = static_cast<const CastInst&>(*instruction);
                        if (transfersThroughCast(cast)) {
                            transition(states, cast.getSource(), block, i, "cast", moved);
                        } else {
                            requireLive(states, cast.getSource(), block, i, "cast");
                        }
                        break;
                    }
                    case OpCode::Call: {
                        const auto& call = static_cast<const CallInst&>(*instruction);
                        for (size_t argumentIndex = 0; argumentIndex < call.getArguments().size();
                            ++argumentIndex) {
                            if (callTransfersOwnership(module_, call, argumentIndex)) {
                                transition(
                                    states,
                                    call.getArguments()[argumentIndex],
                                    block,
                                    i,
                                    "call",
                                    moved
                                );
                            } else {
                                requireLive(
                                    states,
                                    call.getArguments()[argumentIndex],
                                    block,
                                    i,
                                    "call"
                                );
                            }
                        }
                        break;
                    }
                    case OpCode::Copy:
                        requireLive(
                            states,
                            static_cast<const CopyInst&>(*instruction).getSource(),
                            block,
                            i,
                            "copy"
                        );
                        break;
                    case OpCode::Move:
                        transition(
                            states,
                            static_cast<const MoveInst&>(*instruction).getSource(),
                            block,
                            i,
                            "move",
                            moved
                        );
                        break;
                    case OpCode::Borrow:
                        requireLive(
                            states,
                            static_cast<const BorrowInst&>(*instruction).getOwner(),
                            block,
                            i,
                            "borrow"
                        );
                        break;
                    case OpCode::Destroy:
                        transition(
                            states,
                            static_cast<const DestroyInst&>(*instruction).getValue(),
                            block,
                            i,
                            "destroy",
                            destroyed
                        );
                        break;
                    default:
                        break;
                }
                if (const auto result = instructionResult(*instruction); ownsManagedValue(result)) {
                    states[result.get()] = live;
                }
                if (instruction->getOpCode() == OpCode::Ret) {
                    returnStates_[&block][i] = states;
                }
            }
            const auto successors = successors_.find(&block);
            if (successors == successors_.end()) {
                continue;
            }
            for (const auto* successor : successors->second) {
                auto edgeState = states;
                for (const auto* value : ownedValues) {
                    edgeState.try_emplace(value, unavailable);
                }
                for (size_t i = 0; i < successor->getInstructions().size(); ++i) {
                    const auto& instruction = successor->getInstructions()[i];
                    if (!instruction || instruction->getOpCode() != OpCode::Phi) {
                        continue;
                    }
                    const auto& phi = static_cast<const PhiInst&>(*instruction);
                    if (!ownsManagedValue(phi.getResult())) {
                        continue;
                    }
                    for (const auto& [label, value] : phi.getIncoming()) {
                        if (label == block.label) {
                            transition(edgeState, value, *successor, i, "phi", moved);
                            break;
                        }
                    }
                }
                auto& storedState = edgeStates_[&block][successor];
                if (storedState != edgeState) {
                    storedState = std::move(edgeState);
                    changed = true;
                }
            }
        }
    }
    violations_ = std::move(violations);
    analyzed_ = true;
    return violations_;
}

std::vector<OwnershipExitObligation> OwnershipFlowAnalysis::analyzeExitObligations() {
    analyze();

    std::vector<OwnershipExitObligation> obligations;
    for (const auto& [block, returns] : returnStates_) {
        for (const auto& [instructionIndex, states] : returns) {
            for (const auto& [value, state] : states) {
                if ((state & live) != 0) {
                    obligations.push_back(
                        {block, instructionIndex, value, static_cast<OwnershipFlowState>(state)}
                    );
                }
            }
        }
    }
    return obligations;
}

std::vector<OwnershipClosurePlan> OwnershipFlowAnalysis::analyzeOwnershipClosurePlans() {
    const auto obligations = analyzeExitObligations();
    const auto ownedValues = collectOwnedValues(function_);
    std::unordered_map<const Value*, OwnershipDefinitionSite> definitions;
    for (const auto& argument : function_.getArguments()) {
        if (ownsManagedValue(argument)) {
            definitions.emplace(argument.get(), OwnershipDefinitionSite{nullptr, std::nullopt});
        }
    }
    for (const auto& blockOwner : function_.getBlocks()) {
        if (!blockOwner) {
            continue;
        }
        for (size_t i = 0; i < blockOwner->getInstructions().size(); ++i) {
            const auto& instruction = blockOwner->getInstructions()[i];
            if (const auto value = instruction ? instructionResult(*instruction) : nullptr;
                ownsManagedValue(value)) {
                definitions.emplace(value.get(), OwnershipDefinitionSite{blockOwner.get(), i});
            }
        }
    }

    std::vector<OwnershipClosurePlan> plans;
    for (const auto* value : ownedValues) {
        OwnershipClosurePlan plan{value, definitions[value], {}, {}};
        for (const auto& obligation : obligations) {
            if (obligation.value == value) {
                plan.liveExits.push_back(obligation);
            }
        }
        if (!plan.liveExits.empty()) {
            auto addEdgePlacement = [&](const BasicBlock* source, const BasicBlock* destination) {
                const auto duplicate = std::find_if(
                    plan.destroyPlacements.begin(),
                    plan.destroyPlacements.end(),
                    [source, destination](const OwnershipDestroyPlacement& placement) {
                        return placement.kind == OwnershipDestroyPlacementKind::OnEdge
                            && placement.source == source && placement.destination == destination;
                    }
                );
                if (duplicate != plan.destroyPlacements.end()) {
                    return;
                }
                const auto successors = successors_.find(source);
                const bool requiresEdgeSplit =
                    successors != successors_.end() && successors->second.size() > 1;
                plan.destroyPlacements.push_back(
                    {OwnershipDestroyPlacementKind::OnEdge,
                        source,
                        destination,
                        std::nullopt,
                        requiresEdgeSplit}
                );
            };

            for (const auto& exit : plan.liveExits) {
                const auto exitState = static_cast<unsigned char>(exit.state);
                const bool definedBeforeReturn = plan.definition.block == exit.block
                    && plan.definition.instructionIndex
                    && *plan.definition.instructionIndex < exit.instructionIndex;
                if (exitState == live || definedBeforeReturn) {
                    plan.destroyPlacements.push_back(
                        {OwnershipDestroyPlacementKind::BeforeReturn,
                            nullptr,
                            exit.block,
                            exit.instructionIndex,
                            false}
                    );
                    continue;
                }

                std::unordered_set<const BasicBlock*> visited;
                std::function<void(const BasicBlock*)> traceLiveFrontier =
                    [&](const BasicBlock* block) {
                        if (!block || !visited.insert(block).second) {
                            return;
                        }
                        const auto predecessors = predecessors_.find(block);
                        if (predecessors == predecessors_.end()) {
                            return;
                        }
                        for (const auto* predecessor : predecessors->second) {
                            if (reachable_.count(predecessor) == 0) {
                                continue;
                            }
                            const auto edgeState = static_cast<unsigned char>(
                                stateOnEdge(*predecessor, *block, value)
                            );
                            if (edgeState == live) {
                                addEdgePlacement(predecessor, block);
                            } else if ((edgeState & live) != 0) {
                                traceLiveFrontier(predecessor);
                            }
                        }
                    };
                traceLiveFrontier(exit.block);
            }
            plans.push_back(std::move(plan));
        }
    }
    return plans;
}

} // namespace zir
