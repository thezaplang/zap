#include "zir_verifier_internal.hpp"

#include "borrow_verifier.hpp"
#include "call_contract.hpp"
#include "control_flow_graph.hpp"
#include "ownership_flow.hpp"
#include "ownership_liveness.hpp"
#include "string_type.hpp"

#include <algorithm>
#include <iterator>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zir {
namespace {

using verifier_detail::instructionResult;
using verifier_detail::isAssignable;
using verifier_detail::isStringType;
using verifier_detail::isTerminator;
using verifier_detail::typeName;

class FunctionVerifier {
public:
  FunctionVerifier(const Module &module, const Function &function,
                   std::vector<VerificationError> &errors,
                   TypeInterner &typeInterner, bool includeOwnershipObligations)
      : module_(module), function_(function), errors_(errors),
        typeInterner_(typeInterner),
        includeOwnershipObligations_(includeOwnershipObligations),
        cfg_(function) {}

  void verify() {
    verifySignature();
    if (function_.getBlocks().empty()) {
      error(VerificationErrorCode::MissingBody, nullptr, std::nullopt,
            "defined function has no basic blocks");
      return;
    }

    collectBlocks();
    collectDefinitionsAndEdges();
    verifyInstructions();
    auto borrowErrors =
        verifier_detail::verifyBorrowContracts(module_, function_, cfg_);
    errors_.insert(errors_.end(), std::make_move_iterator(borrowErrors.begin()),
                   std::make_move_iterator(borrowErrors.end()));
    verifyOwnershipTransfers();
  }

private:
  struct DefinitionSite {
    const BasicBlock *block;
    size_t instructionIndex;
  };

  const Module &module_;
  const Function &function_;
  std::vector<VerificationError> &errors_;
  TypeInterner &typeInterner_;
  bool includeOwnershipObligations_;
  std::unordered_map<const Value *, DefinitionSite> definitions_;
  std::unordered_map<std::string, const Value *> valueNames_;
  ControlFlowGraph cfg_;

  void error(VerificationErrorCode code, const BasicBlock *block,
             std::optional<size_t> instructionIndex, std::string message) {
    errors_.push_back({code, function_.name,
                       block ? block->label : std::string{}, instructionIndex,
                       std::move(message)});
  }

  void verifySignature() {
    if (!function_.getReturnType()) {
      error(VerificationErrorCode::NullNode, nullptr, std::nullopt,
            "function has no return type");
    }
    for (const auto &argument : function_.getArguments()) {
      if (!argument || !argument->getType()) {
        error(VerificationErrorCode::NullNode, nullptr, std::nullopt,
              "function has a null argument or argument type");
        continue;
      }
      if (!valueNames_.emplace(argument->getName(), argument.get()).second) {
        error(VerificationErrorCode::DuplicateValue, nullptr, std::nullopt,
              "duplicate argument name " + argument->getName());
      }
      if (argument->getParameterEscape() == ParameterEscape::NoEscape) {
        if (argument->getParameterOwnership() != ParameterOwnership::Borrow) {
          error(VerificationErrorCode::InvalidOperand, nullptr, std::nullopt,
                "noescape parameter must use borrowed ownership: " +
                    argument->getName());
        }
        if (argument->getType()->getIntrinsicKind() !=
            IntrinsicTypeKind::StringView) {
          error(VerificationErrorCode::InvalidOperand, nullptr, std::nullopt,
                "noescape currently requires a StringView parameter: " +
                    argument->getName());
        }
      }
    }
  }

  void collectBlocks() {
    for (const auto &block : function_.getBlocks()) {
      if (!block) {
        error(VerificationErrorCode::NullNode, nullptr, std::nullopt,
              "function contains a null basic block");
        continue;
      }
      if (cfg_.findBlock(block->label) != block.get()) {
        error(VerificationErrorCode::DuplicateBlock, block.get(), std::nullopt,
              "duplicate basic block label " + block->label);
      }
    }
  }

  void collectDefinitionsAndEdges() {
    for (const auto &blockOwner : function_.getBlocks()) {
      if (!blockOwner) {
        continue;
      }
      const auto &block = *blockOwner;
      bool sawTerminator = false;
      for (size_t i = 0; i < block.getInstructions().size(); ++i) {
        const auto &instruction = block.getInstructions()[i];
        if (!instruction) {
          error(VerificationErrorCode::NullNode, &block, i,
                "basic block contains a null instruction");
          continue;
        }
        if (sawTerminator) {
          error(VerificationErrorCode::InstructionAfterTerminator, &block, i,
                "instruction appears after a terminator");
        }
        sawTerminator = sawTerminator || isTerminator(instruction->getOpCode());

        if (auto result = instructionResult(*instruction)) {
          if (!result->getType()) {
            error(VerificationErrorCode::NullNode, &block, i,
                  "instruction result has no type");
          }
          if (!definitions_.emplace(result.get(), DefinitionSite{&block, i})
                   .second) {
            error(VerificationErrorCode::DuplicateValue, &block, i,
                  "value is defined more than once: " + result->getName());
          }
          auto [_, inserted] =
              valueNames_.emplace(result->getName(), result.get());
          if (!inserted) {
            error(VerificationErrorCode::DuplicateValue, &block, i,
                  "duplicate value name " + result->getName());
          }
        }

        if (instruction->getOpCode() == OpCode::Br) {
          const auto &branch = static_cast<const BranchInst &>(*instruction);
          if (!cfg_.findBlock(branch.getTarget())) {
            error(VerificationErrorCode::InvalidBranchTarget, &block, i,
                  "branch targets unknown block " + branch.getTarget());
          }
        } else if (instruction->getOpCode() == OpCode::CondBr) {
          const auto &branch =
              static_cast<const CondBranchInst &>(*instruction);
          for (const auto &label :
               {branch.getTrueLabel(), branch.getFalseLabel()}) {
            if (!cfg_.findBlock(label)) {
              error(VerificationErrorCode::InvalidBranchTarget, &block, i,
                    "branch targets unknown block " + label);
            }
          }
        }
      }
      if (!sawTerminator) {
        error(VerificationErrorCode::MissingTerminator, &block, std::nullopt,
              "basic block has no terminator");
      }
    }
  }

  bool verifyValue(const std::shared_ptr<Value> &value,
                   const BasicBlock &useBlock, size_t useIndex,
                   const BasicBlock *edgePredecessor = nullptr) {
    if (!value || !value->getType()) {
      error(VerificationErrorCode::NullNode, &useBlock, useIndex,
            "instruction contains a null value or value type");
      return false;
    }
    if (value->getKind() != ValueKind::Register) {
      return true;
    }

    auto definition = definitions_.find(value.get());
    if (definition == definitions_.end()) {
      error(VerificationErrorCode::UndefinedValue, &useBlock, useIndex,
            "use of undefined value " + value->getName());
      return false;
    }

    const auto *effectiveBlock = edgePredecessor ? edgePredecessor : &useBlock;
    if (definition->second.block == effectiveBlock) {
      const size_t effectiveIndex =
          edgePredecessor ? effectiveBlock->getInstructions().size() : useIndex;
      if (definition->second.instructionIndex >= effectiveIndex) {
        error(VerificationErrorCode::UseBeforeDefinition, &useBlock, useIndex,
              "value is used before its definition: " + value->getName());
        return false;
      }
      return true;
    }

    if (cfg_.isReachable(*effectiveBlock) &&
        !cfg_.dominates(*definition->second.block, *effectiveBlock)) {
      error(VerificationErrorCode::DominanceViolation, &useBlock, useIndex,
            "definition of " + value->getName() + " does not dominate its use");
      return false;
    }
    return true;
  }

  void expectSameType(const std::shared_ptr<Type> &actual,
                      const std::shared_ptr<Type> &expected,
                      const BasicBlock &block, size_t index,
                      const std::string &context) {
    if (!typeInterner_.same(actual, expected)) {
      error(VerificationErrorCode::TypeMismatch, &block, index,
            context + ": expected " + typeName(expected) + ", got " +
                typeName(actual));
    }
  }

  void expectAssignable(const std::shared_ptr<Type> &actual,
                        const std::shared_ptr<Type> &expected,
                        const BasicBlock &block, size_t index,
                        const std::string &context) {
    if (!isAssignable(actual, expected, typeInterner_)) {
      error(VerificationErrorCode::TypeMismatch, &block, index,
            context + ": expected " + typeName(expected) + ", got " +
                typeName(actual));
    }
  }

  void verifyInstructions() {
    for (const auto &blockOwner : function_.getBlocks()) {
      if (!blockOwner) {
        continue;
      }
      const auto &block = *blockOwner;
      for (size_t i = 0; i < block.getInstructions().size(); ++i) {
        const auto &instruction = block.getInstructions()[i];
        if (instruction) {
          verifyInstruction(*instruction, block, i);
        }
      }
    }
  }

  static bool ownsManagedValue(const std::shared_ptr<Value> &value) {
    return value && isOwned(value->getOwnership()) &&
           containsManagedValues(value->getType());
  }

  bool isBorrowedMethodSelf(const CallInst &call, size_t argumentIndex) const {
    if (call.isIndirect() || argumentIndex != 0) {
      return false;
    }
    const auto *callee = module_.findFunction(call.getFunctionName());
    return callee &&
           (!callee->ownerTypeCodegenName.empty() ||
            callee->hasExtensionReceiver) &&
           !callee->getArguments().empty() && callee->getArguments().front() &&
           callee->getArguments().front()->getRawName() == "self";
  }

  void verifyOwnershipTransfers() {
    OwnershipFlowAnalysis analysis(module_, function_, cfg_.predecessors(),
                                   cfg_.successors(), cfg_.reachable());
    for (const auto &violation : analysis.analyze()) {
      error(VerificationErrorCode::OwnershipViolation, violation.block,
            violation.instructionIndex,
            "owned value is not live for " + violation.operation + ": " +
                violation.value->getName() + " (state: " +
                formatOwnershipFlowState(violation.priorState) + ")");
    }
    if (includeOwnershipObligations_) {
      verifyOwnershipClosurePlans(analysis);
    }

    const auto liveness = analyzeOwnershipLiveness(module_, function_, cfg_);
    for (const auto &blockOwner : function_.getBlocks()) {
      if (!blockOwner) {
        continue;
      }
      const auto &block = *blockOwner;
      for (size_t i = 0; i < block.getInstructions().size(); ++i) {
        const auto &instruction = block.getInstructions()[i];
        if (!instruction || instruction->getOpCode() != OpCode::Destroy) {
          continue;
        }
        const auto &value =
            static_cast<const DestroyInst &>(*instruction).getValue();
        if (liveness.isLiveAfter(block, i, value)) {
          error(VerificationErrorCode::OwnershipViolation, &block, i,
                "cannot destroy a String owner while one of its borrowed "
                "StringViews is still live");
        }
      }
    }
  }

  void verifyOwnershipClosurePlans(OwnershipFlowAnalysis &analysis) {
    for (const auto &plan : analysis.analyzeOwnershipClosurePlans()) {
      std::string definition = "function argument";
      if (plan.definition.block) {
        definition = "block %" + plan.definition.block->label;
        if (plan.definition.instructionIndex) {
          definition += " instruction " +
                        std::to_string(*plan.definition.instructionIndex);
        }
      }
      for (const auto &obligation : plan.liveExits) {
        std::string placement = "no safe destroy placement determined";
        for (const auto &candidate : plan.destroyPlacements) {
          if (candidate.destination != obligation.block ||
              (candidate.kind == OwnershipDestroyPlacementKind::BeforeReturn &&
               candidate.instructionIndex != obligation.instructionIndex)) {
            continue;
          }
          if (candidate.kind == OwnershipDestroyPlacementKind::BeforeReturn) {
            placement = "suggested destroy before return";
          } else {
            placement = "suggested destroy on edge %" +
                        candidate.source->label + " -> %" +
                        candidate.destination->label;
            if (candidate.requiresEdgeSplit) {
              placement += " (split critical edge)";
            }
          }
          break;
        }
        error(VerificationErrorCode::OwnershipViolation, obligation.block,
              obligation.instructionIndex,
              "owned value may remain live at function exit: " +
                  plan.value->getName() + " (defined in " + definition +
                  "; state: " + formatOwnershipFlowState(obligation.state) +
                  "; " + placement + ")");
      }
    }
  }

  void verifyInstruction(const Instruction &instruction,
                         const BasicBlock &block, size_t index) {
    switch (instruction.getOpCode()) {
    case OpCode::Alloca: {
      const auto &alloca = static_cast<const AllocaInst &>(instruction);
      if (!alloca.getResult() || !alloca.getAllocatedType()) {
        error(VerificationErrorCode::InvalidResult, &block, index,
              "alloca requires a result and allocated type");
        return;
      }
      auto pointer =
          std::dynamic_pointer_cast<PointerType>(alloca.getResult()->getType());
      if (!pointer || !typeInterner_.same(pointer->getBaseType(),
                                          alloca.getAllocatedType())) {
        error(VerificationErrorCode::TypeMismatch, &block, index,
              "alloca result must point to the allocated type");
      }
      return;
    }
    case OpCode::Load: {
      const auto &load = static_cast<const LoadInst &>(instruction);
      verifyValue(load.getSource(), block, index);
      auto pointer = load.getSource() ? std::dynamic_pointer_cast<PointerType>(
                                            load.getSource()->getType())
                                      : nullptr;
      if (!pointer || !load.getResult()) {
        error(VerificationErrorCode::InvalidOperand, &block, index,
              "load requires a pointer source and result");
      } else {
        expectSameType(load.getResult()->getType(), pointer->getBaseType(),
                       block, index, "load result type");
      }
      return;
    }
    case OpCode::Store: {
      const auto &store = static_cast<const StoreInst &>(instruction);
      verifyValue(store.getSource(), block, index);
      verifyValue(store.getDestination(), block, index);
      auto pointer = store.getDestination()
                         ? std::dynamic_pointer_cast<PointerType>(
                               store.getDestination()->getType())
                         : nullptr;
      if (!pointer || !store.getSource()) {
        error(VerificationErrorCode::InvalidOperand, &block, index,
              "store requires a value and pointer destination");
      } else {
        expectAssignable(store.getSource()->getType(), pointer->getBaseType(),
                         block, index, "stored value type");
        if (store.getMode() == StoreMode::Initialize &&
            containsManagedValues(store.getSource()->getType()) &&
            !isOwned(store.getSource()->getOwnership())) {
          error(VerificationErrorCode::InvalidOperand, &block, index,
                "managed initialization must transfer ownership");
        }
      }
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
      const auto &binary = static_cast<const BinaryInst &>(instruction);
      verifyValue(binary.getLhs(), block, index);
      verifyValue(binary.getRhs(), block, index);
      if (!binary.getResult() || !binary.getLhs() || !binary.getRhs()) {
        error(VerificationErrorCode::InvalidOperand, &block, index,
              "binary instruction requires two operands and a result");
        return;
      }

      const auto lhsType = binary.getLhs()->getType();
      const auto rhsType = binary.getRhs()->getType();
      const bool isAdd = instruction.getOpCode() == OpCode::Add;
      const bool isSubtract = instruction.getOpCode() == OpCode::Sub;
      const bool isStringConcat =
          isAdd && (isStringType(lhsType) || isStringType(rhsType) ||
                    lhsType->getKind() == TypeKind::Char ||
                    rhsType->getKind() == TypeKind::Char);
      const bool lhsPointer = lhsType->getKind() == TypeKind::Pointer;
      const bool rhsPointer = rhsType->getKind() == TypeKind::Pointer;

      if (isStringConcat) {
        if ((!isStringType(lhsType) && lhsType->getKind() != TypeKind::Char) ||
            (!isStringType(rhsType) && rhsType->getKind() != TypeKind::Char) ||
            binary.getResult()->getType()->getIntrinsicKind() !=
                IntrinsicTypeKind::String) {
          error(VerificationErrorCode::TypeMismatch, &block, index,
                "string concatenation requires String/StringView/Char "
                "operands and a String result");
        }
      } else if (isAdd && lhsPointer != rhsPointer) {
        const auto offsetType = lhsPointer ? rhsType : lhsType;
        const auto pointerType = lhsPointer ? lhsType : rhsType;
        if (!offsetType->isInteger()) {
          error(VerificationErrorCode::TypeMismatch, &block, index,
                "pointer offset must be an integer");
        }
        expectSameType(binary.getResult()->getType(), pointerType, block, index,
                       "pointer addition result type");
      } else if (isSubtract && lhsPointer && rhsPointer) {
        expectSameType(lhsType, rhsType, block, index,
                       "pointer subtraction operand type");
        if (!binary.getResult()->getType()->isInteger()) {
          error(VerificationErrorCode::TypeMismatch, &block, index,
                "pointer subtraction result must be an integer");
        }
      } else if (isSubtract && lhsPointer && !rhsPointer) {
        if (!rhsType->isInteger()) {
          error(VerificationErrorCode::TypeMismatch, &block, index,
                "pointer offset must be an integer");
        }
        expectSameType(binary.getResult()->getType(), lhsType, block, index,
                       "pointer subtraction result type");
      } else {
        expectSameType(rhsType, lhsType, block, index, "binary operand type");
        expectSameType(binary.getResult()->getType(), lhsType, block, index,
                       "binary result type");
      }
      return;
    }
    case OpCode::Cmp: {
      const auto &compare = static_cast<const CmpInst &>(instruction);
      verifyValue(compare.getLhs(), block, index);
      verifyValue(compare.getRhs(), block, index);
      if (compare.getLhs() && compare.getRhs()) {
        expectSameType(compare.getRhs()->getType(), compare.getLhs()->getType(),
                       block, index, "comparison operand type");
      }
      if (!compare.getResult() ||
          compare.getResult()->getType()->getKind() != TypeKind::Bool) {
        error(VerificationErrorCode::InvalidResult, &block, index,
              "comparison result must be Bool");
      }
      return;
    }
    case OpCode::Br:
      return;
    case OpCode::CondBr: {
      const auto &branch = static_cast<const CondBranchInst &>(instruction);
      verifyValue(branch.getCondition(), block, index);
      if (!branch.getCondition() ||
          branch.getCondition()->getType()->getKind() != TypeKind::Bool) {
        error(VerificationErrorCode::InvalidConditionType, &block, index,
              "conditional branch requires a Bool condition");
      }
      return;
    }
    case OpCode::Ret: {
      const auto &returnInstruction =
          static_cast<const ReturnInst &>(instruction);
      if (returnInstruction.getValue()) {
        verifyValue(returnInstruction.getValue(), block, index);
      }
      const bool returnsVoid =
          function_.getReturnType() &&
          function_.getReturnType()->getKind() == TypeKind::Void;
      if (returnsVoid && returnInstruction.getValue()) {
        error(VerificationErrorCode::InvalidReturn, &block, index,
              "void function returns a value");
      } else if (!returnsVoid && !returnInstruction.getValue()) {
        error(VerificationErrorCode::InvalidReturn, &block, index,
              "non-void function returns no value");
      } else if (returnInstruction.getValue()) {
        auto expectedReturnType = function_.getReturnType();
        if (function_.returnsRef) {
          expectedReturnType =
              std::make_shared<PointerType>(expectedReturnType);
        }
        expectAssignable(returnInstruction.getValue()->getType(),
                         expectedReturnType, block, index, "return value type");
        if (function_.returnsRef &&
            isOwned(returnInstruction.getValue()->getOwnership())) {
          error(VerificationErrorCode::InvalidReturn, &block, index,
                "ref return cannot transfer ownership");
        }
      }
      return;
    }
    case OpCode::Call:
      verifyCall(static_cast<const CallInst &>(instruction), block, index);
      return;
    case OpCode::Copy: {
      const auto &copy = static_cast<const CopyInst &>(instruction);
      verifyValue(copy.getSource(), block, index);
      if (!copy.getResult() || !copy.getSource() ||
          !containsManagedValues(copy.getSource()->getType()) ||
          copy.getResult() == copy.getSource() ||
          copy.getResult()->getOwnership() !=
              ownedForType(copy.getSource()->getType())) {
        error(VerificationErrorCode::InvalidOperand, &block, index,
              "copy requires a distinct owned result and managed source");
      } else {
        expectSameType(copy.getResult()->getType(), copy.getSource()->getType(),
                       block, index, "copy result type");
      }
      return;
    }
    case OpCode::Move: {
      const auto &move = static_cast<const MoveInst &>(instruction);
      verifyValue(move.getSource(), block, index);
      if (!move.getResult() || !ownsManagedValue(move.getSource()) ||
          move.getResult() == move.getSource() ||
          move.getResult()->getOwnership() !=
              move.getSource()->getOwnership()) {
        error(VerificationErrorCode::InvalidOperand, &block, index,
              "move requires a distinct owned result and owned managed source");
      } else {
        expectSameType(move.getResult()->getType(), move.getSource()->getType(),
                       block, index, "move result type");
      }
      return;
    }
    case OpCode::Borrow: {
      const auto &borrow = static_cast<const BorrowInst &>(instruction);
      verifyValue(borrow.getOwner(), block, index);
      if (!borrow.getResult() || !borrow.getOwner() ||
          borrow.getOwner()->getType()->getIntrinsicKind() !=
              IntrinsicTypeKind::String ||
          borrow.getResult()->getType()->getIntrinsicKind() !=
              IntrinsicTypeKind::StringView ||
          borrow.getResult()->getOwnership() != ValueOwnership::Borrowed) {
        error(
            VerificationErrorCode::InvalidOperand, &block, index,
            "borrow requires a String owner and a borrowed StringView result");
      }
      return;
    }
    case OpCode::Destroy: {
      const auto &destroy = static_cast<const DestroyInst &>(instruction);
      verifyValue(destroy.getValue(), block, index);
      if (!ownsManagedValue(destroy.getValue())) {
        error(VerificationErrorCode::InvalidOperand, &block, index,
              "destroy requires an owned managed value");
      }
      return;
    }
    case OpCode::Alloc: {
      const auto &alloc = static_cast<const AllocInst &>(instruction);
      if (!alloc.getResult() || !alloc.getAllocatedType()) {
        error(VerificationErrorCode::InvalidResult, &block, index,
              "alloc requires a result and allocated type");
      } else {
        expectSameType(alloc.getResult()->getType(), alloc.getAllocatedType(),
                       block, index, "alloc result type");
        if (alloc.getResult()->getOwnership() != ValueOwnership::OwnedStrong) {
          error(VerificationErrorCode::InvalidResult, &block, index,
                "alloc result must be owned");
        }
      }
      return;
    }
    case OpCode::GetElementPtr:
      verifyGetElementPtr(static_cast<const GetElementPtrInst &>(instruction),
                          block, index);
      return;
    case OpCode::Phi:
      verifyPhi(static_cast<const PhiInst &>(instruction), block, index);
      return;
    case OpCode::Cast: {
      const auto &cast = static_cast<const CastInst &>(instruction);
      verifyValue(cast.getSource(), block, index);
      if (!cast.getResult() || !cast.getTargetType() || !cast.getSource()) {
        error(VerificationErrorCode::InvalidResult, &block, index,
              "cast requires a source, result, and target type");
      } else {
        expectSameType(cast.getResult()->getType(), cast.getTargetType(), block,
                       index, "cast result type");
        const auto expectedOwnership =
            cast.getTargetType()->getIntrinsicKind() ==
                    IntrinsicTypeKind::String
                ? ValueOwnership::OwnedStrong
            : containsManagedValues(cast.getTargetType()) &&
                    isOwned(cast.getSource()->getOwnership())
                ? cast.getSource()->getOwnership()
                : ValueOwnership::Borrowed;
        if (cast.getResult()->getOwnership() != expectedOwnership) {
          error(VerificationErrorCode::InvalidResult, &block, index,
                "cast result ownership does not match source and target");
        }
      }
      return;
    }
    case OpCode::WeakLock: {
      const auto &weakLock = static_cast<const WeakLockInst &>(instruction);
      verifyValue(weakLock.getWeakValue(), block, index);
      if (!weakLock.getResult() || !weakLock.getWeakValue() ||
          weakLock.getResult()->getType()->getKind() != TypeKind::Class ||
          weakLock.getWeakValue()->getType()->getKind() != TypeKind::Class) {
        error(VerificationErrorCode::InvalidOperand, &block, index,
              "weak.lock requires class operands");
      } else {
        const auto weakType = std::static_pointer_cast<ClassType>(
            weakLock.getWeakValue()->getType());
        const auto resultType = std::static_pointer_cast<ClassType>(
            weakLock.getResult()->getType());
        if (!weakType->isWeak() || resultType->isWeak()) {
          error(VerificationErrorCode::InvalidOperand, &block, index,
                "weak.lock requires a weak source and strong result");
        } else {
          auto expectedResultType = std::make_shared<ClassType>(*weakType);
          expectedResultType->setWeak(false);
          expectSameType(weakLock.getResult()->getType(), expectedResultType,
                         block, index, "weak.lock result type");
        }
        if (weakLock.getResult()->getOwnership() !=
            ValueOwnership::OwnedStrong) {
          error(VerificationErrorCode::InvalidResult, &block, index,
                "weak.lock result must be owned");
        }
      }
      return;
    }
    case OpCode::WeakAlive: {
      const auto &weakAlive = static_cast<const WeakAliveInst &>(instruction);
      verifyValue(weakAlive.getWeakValue(), block, index);
      if (!weakAlive.getResult() ||
          weakAlive.getResult()->getType()->getKind() != TypeKind::Bool) {
        error(VerificationErrorCode::InvalidResult, &block, index,
              "weak.alive result must be Bool");
      }
      return;
    }
    case OpCode::ClassIs: {
      const auto &classIs = static_cast<const ClassIsInst &>(instruction);
      verifyValue(classIs.getObject(), block, index);
      if (!classIs.getResult() || !classIs.getObject() ||
          !classIs.getTargetType() ||
          classIs.getResult()->getType()->getKind() != TypeKind::Bool ||
          classIs.getObject()->getType()->getKind() != TypeKind::Class) {
        error(VerificationErrorCode::InvalidOperand, &block, index,
              "classis requires a class operand and Bool result");
      }
      return;
    }
    case OpCode::InlineAsm: {
      const auto &assembly = static_cast<const InlineAsmInst &>(instruction);
      for (const auto &operand : assembly.getOutputs()) {
        verifyValue(operand.value, block, index);
      }
      for (const auto &operand : assembly.getInputs()) {
        verifyValue(operand.value, block, index);
      }
      return;
    }
    }
  }

  void verifyCall(const CallInst &call, const BasicBlock &block, size_t index) {
    for (const auto &argument : call.getArguments()) {
      verifyValue(argument, block, index);
    }
    if (call.getVariadicPack()) {
      verifyValue(call.getVariadicPack(), block, index);
    }

    std::vector<std::shared_ptr<Type>> parameterTypes;
    std::vector<ParameterOwnership> parameterOwnership;
    ResultBorrowContract resultBorrow;
    std::shared_ptr<Type> returnType;
    const bool returnsRef = callReturnsRef(module_, call);
    bool variadic = false;
    if (call.isIndirect()) {
      verifyValue(call.getCalleeValue(), block, index);
      auto functionType = call.getCalleeValue()
                              ? std::dynamic_pointer_cast<FunctionPointerType>(
                                    call.getCalleeValue()->getType())
                              : nullptr;
      if (!functionType) {
        error(VerificationErrorCode::InvalidCall, &block, index,
              "indirect call requires a function pointer");
        return;
      }
      parameterTypes = functionType->getParams();
      parameterOwnership = functionType->getParameterOwnership();
      resultBorrow = functionType->getResultBorrow();
      returnType = functionType->getReturnType();
    } else {
      const auto *callee = module_.findFunction(call.getFunctionName());
      if (!callee) {
        error(VerificationErrorCode::InvalidCall, &block, index,
              "call targets unknown function " + call.getFunctionName());
        return;
      }
      for (const auto &argument : callee->getArguments()) {
        if (argument && !argument->isVariadicPack()) {
          parameterTypes.push_back(argument->getType());
          parameterOwnership.push_back(argument->getParameterOwnership());
        }
      }
      returnType = callee->getReturnType();
      resultBorrow = callee->resultBorrow;
      variadic = callee->isCVariadic ||
                 (!callee->getArguments().empty() &&
                  callee->getArguments().back()->isVariadicPack());
    }

    if ((!variadic && call.getArguments().size() != parameterTypes.size()) ||
        (variadic && call.getArguments().size() < parameterTypes.size())) {
      error(VerificationErrorCode::InvalidCall, &block, index,
            "call argument count does not match the callee signature");
    }
    const size_t checkedArguments =
        std::min(call.getArguments().size(), parameterTypes.size());
    for (size_t i = 0; i < checkedArguments; ++i) {
      if (call.getArguments()[i]) {
        expectAssignable(call.getArguments()[i]->getType(), parameterTypes[i],
                         block, index,
                         "call argument " + std::to_string(i) + " type");
      }
    }
    if (call.getResult() && returnType) {
      auto expectedReturnType = returnType;
      if (returnsRef) {
        expectedReturnType = std::make_shared<PointerType>(returnType);
      }
      expectSameType(call.getResult()->getType(), expectedReturnType, block,
                     index, "call result type");
      const auto expectedOwnership =
          !returnsRef && containsManagedValues(returnType)
              ? ownedForType(returnType)
              : ValueOwnership::Borrowed;
      if (call.getResult()->getOwnership() != expectedOwnership) {
        error(VerificationErrorCode::InvalidResult, &block, index,
              "call result ownership does not match its return type");
      }
    }
    if (!call.getArgumentIsRef().empty() &&
        call.getArgumentIsRef().size() != call.getArguments().size()) {
      error(VerificationErrorCode::InvalidCall, &block, index,
            "call ref-argument metadata has the wrong size");
    }
    if (resultBorrow.hasSource() &&
        *resultBorrow.sourceParameter() >= call.getArguments().size()) {
      error(VerificationErrorCode::InvalidCall, &block, index,
            "call result borrow source argument is out of range");
    }
    for (size_t i = 0; i < call.getArguments().size(); ++i) {
      const auto &argument = call.getArguments()[i];
      const bool transfers = i < parameterOwnership.size() &&
                             transfersOwnership(parameterOwnership[i]);
      if (transfers && !ownsManagedValue(argument)) {
        error(VerificationErrorCode::InvalidCall, &block, index,
              "call transfer argument must be owned: " + std::to_string(i));
      }
      if (i < call.getArgumentIsRef().size() && call.getArgumentIsRef()[i] &&
          transfers) {
        error(VerificationErrorCode::InvalidCall, &block, index,
              "ref call argument cannot transfer ownership");
      }
      if (transfers && isBorrowedMethodSelf(call, i)) {
        error(VerificationErrorCode::InvalidCall, &block, index,
              "method self argument cannot transfer ownership");
      }
    }
  }

  void verifyGetElementPtr(const GetElementPtrInst &gep,
                           const BasicBlock &block, size_t index) {
    verifyValue(gep.getPointer(), block, index);
    if (gep.getIndexValue()) {
      verifyValue(gep.getIndexValue(), block, index);
      if (!gep.getIndexValue()->getType()->isInteger()) {
        error(VerificationErrorCode::InvalidOperand, &block, index,
              "dynamic getelementptr index must be an integer");
      }
    }
    if (!gep.getResult() || !gep.getPointer()) {
      error(VerificationErrorCode::InvalidResult, &block, index,
            "getelementptr requires pointer and result operands");
      return;
    }
    if (gep.getResult()->getType()->getKind() != TypeKind::Pointer) {
      error(VerificationErrorCode::TypeMismatch, &block, index,
            "getelementptr result must be a pointer");
    }
  }

  void verifyPhi(const PhiInst &phi, const BasicBlock &block, size_t index) {
    if (!phi.getResult()) {
      error(VerificationErrorCode::InvalidResult, &block, index,
            "phi requires a result");
      return;
    }
    std::unordered_set<const BasicBlock *> incomingBlocks;
    for (const auto &[label, value] : phi.getIncoming()) {
      const auto *incomingBlock = cfg_.findBlock(label);
      const auto &predecessors = cfg_.predecessors().at(&block);
      if (!incomingBlock || std::find(predecessors.begin(), predecessors.end(),
                                      incomingBlock) == predecessors.end()) {
        error(VerificationErrorCode::InvalidPhi, &block, index,
              "phi incoming label is not a predecessor: " + label);
        continue;
      }
      if (!incomingBlocks.insert(incomingBlock).second) {
        error(VerificationErrorCode::InvalidPhi, &block, index,
              "phi contains duplicate incoming block " + label);
      }
      if (value) {
        verifyValue(value, block, index, incomingBlock);
        expectSameType(value->getType(), phi.getResult()->getType(), block,
                       index, "phi incoming value type");
      }
    }
    std::unordered_set<const BasicBlock *> expectedPredecessors(
        cfg_.predecessors().at(&block).begin(),
        cfg_.predecessors().at(&block).end());
    if (incomingBlocks != expectedPredecessors) {
      error(VerificationErrorCode::InvalidPhi, &block, index,
            "phi must have one incoming value for every predecessor");
    }

    ValueOwnership expectedOwnership = ValueOwnership::Borrowed;
    if (containsManagedValues(phi.getResult()->getType()) &&
        !phi.getIncoming().empty() && phi.getIncoming().front().second) {
      const auto incomingOwnership =
          phi.getIncoming().front().second->getOwnership();
      const bool allIncomingHaveSameOwnership = std::all_of(
          phi.getIncoming().begin(), phi.getIncoming().end(),
          [incomingOwnership](const auto &incoming) {
            return incoming.second &&
                   incoming.second->getOwnership() == incomingOwnership;
          });
      if (!allIncomingHaveSameOwnership) {
        error(VerificationErrorCode::InvalidResult, &block, index,
              "phi incoming values must have matching ownership");
        return;
      }
      expectedOwnership = incomingOwnership;
    }
    if (phi.getResult()->getOwnership() != expectedOwnership) {
      error(VerificationErrorCode::InvalidResult, &block, index,
            "phi result ownership does not match incoming values");
    }
  }
};

} // namespace

void verifier_detail::verifyDefinedFunction(
    const Module &module, const Function &function,
    std::vector<VerificationError> &errors, TypeInterner &typeInterner,
    bool includeOwnershipObligations) {
  FunctionVerifier(module, function, errors, typeInterner,
                   includeOwnershipObligations)
      .verify();
}

} // namespace zir
