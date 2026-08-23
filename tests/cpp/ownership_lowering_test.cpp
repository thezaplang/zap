#include "ir/ownership_lowering.hpp"
#include "ir/zir_verifier.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using zir::BasicBlock;
using zir::BranchInst;
using zir::ClassType;
using zir::CmpInst;
using zir::CondBranchInst;
using zir::Constant;
using zir::DestroyInst;
using zir::Function;
using zir::Module;
using zir::OpCode;
using zir::PhiInst;
using zir::PrimitiveType;
using zir::Register;
using zir::ReturnInst;
using zir::Type;
using zir::TypeKind;
using zir::ValueOwnership;
using zir::VerificationErrorCode;
using zir::ZirVerifier;

std::shared_ptr<Type> primitive(TypeKind kind) {
  return std::make_shared<PrimitiveType>(kind);
}

std::shared_ptr<Register> reg(const std::string &name,
                              const std::shared_ptr<Type> &type) {
  return std::make_shared<Register>(name, type);
}

bool expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
  }
  return condition;
}

bool hasError(const zir::VerificationResult &result,
              VerificationErrorCode code) {
  for (const auto &error : result.errors()) {
    if (error.code == code) {
      return true;
    }
  }
  return false;
}

std::shared_ptr<zir::Argument>
addOwnedArgument(Function &function, const std::shared_ptr<Type> &type) {
  auto value = std::make_shared<zir::Argument>("value", type);
  value->setOwnership(ValueOwnership::Owned);
  function.arguments.push_back(value);
  return value;
}

bool hasDestroyBeforeReturn(const BasicBlock &block,
                            const std::shared_ptr<zir::Value> &value) {
  const auto &instructions = block.getInstructions();
  return instructions.size() == 2 &&
         instructions.front()->getOpCode() == OpCode::Destroy &&
         static_cast<const DestroyInst &>(*instructions.front()).getValue() ==
             value &&
         instructions.back()->getOpCode() == OpCode::Ret;
}

bool hasDestroyEdgeTo(const Function &function, const std::string &destination,
                      const std::shared_ptr<zir::Value> &value) {
  for (const auto &block : function.getBlocks()) {
    if (!block) {
      continue;
    }
    const auto &instructions = block->getInstructions();
    if (instructions.size() != 2 ||
        instructions.front()->getOpCode() != OpCode::Destroy ||
        static_cast<const DestroyInst &>(*instructions.front()).getValue() !=
            value ||
        instructions.back()->getOpCode() != OpCode::Br) {
      continue;
    }
    if (static_cast<const BranchInst &>(*instructions.back()).getTarget() ==
        destination) {
      return true;
    }
  }
  return false;
}

bool testLoopExitClosesOwnershipForZeroOneOrManyIterations() {
  Module module("ownership-loop-exit");
  auto classType = std::make_shared<ClassType>("Node");
  auto boolean = primitive(TypeKind::Bool);
  auto function =
      std::make_unique<Function>("valid", primitive(TypeKind::Void));
  auto value = addOwnedArgument(*function, classType);

  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<BranchInst>("header"));
  auto header = std::make_unique<BasicBlock>("header");
  header->addInstruction(std::make_unique<CondBranchInst>(
      std::make_shared<Constant>("true", boolean), "body", "exit"));
  auto body = std::make_unique<BasicBlock>("body");
  auto comparison = reg("comparison", boolean);
  body->addInstruction(std::make_unique<CmpInst>(
      "eq", comparison, value, std::make_shared<Constant>("null", classType)));
  body->addInstruction(std::make_unique<BranchInst>("header"));
  auto exit = std::make_unique<BasicBlock>("exit");
  exit->addInstruction(std::make_unique<ReturnInst>());

  function->addBlock(std::move(entry));
  function->addBlock(std::move(header));
  function->addBlock(std::move(body));
  function->addBlock(std::move(exit));
  module.addFunction(std::move(function));

  zir::lowerDeadOwnedResults(module);
  auto *lowered = module.getFunctions().front().get();
  return expect(hasDestroyBeforeReturn(*lowered->findBlock("exit"), value),
                "loop exit did not close the owned argument") &&
         expect(ZirVerifier().verify(module).ok(),
                "loop ownership lowering produced invalid ZIR") &&
         expect(ZirVerifier().verifyOwnershipObligations(module).ok(),
                "loop ownership lowering left an obligation");
}

bool testLoopBackEdgePhiTransfersOwnership() {
  Module module("ownership-loop-phi");
  auto classType = std::make_shared<ClassType>("Node");
  auto boolean = primitive(TypeKind::Bool);
  auto function =
      std::make_unique<Function>("valid", primitive(TypeKind::Void));
  auto initial = addOwnedArgument(*function, classType);

  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<BranchInst>("header"));
  auto header = std::make_unique<BasicBlock>("header");
  auto current = reg("current", classType);
  current->setOwnership(ValueOwnership::Owned);
  header->addInstruction(std::make_unique<PhiInst>(
      current, std::vector<std::pair<std::string, std::shared_ptr<zir::Value>>>{
                   {"entry", initial}, {"body", current}}));
  header->addInstruction(std::make_unique<CondBranchInst>(
      std::make_shared<Constant>("true", boolean), "body", "exit"));
  auto body = std::make_unique<BasicBlock>("body");
  auto comparison = reg("comparison", boolean);
  body->addInstruction(
      std::make_unique<CmpInst>("eq", comparison, current,
                                std::make_shared<Constant>("null", classType)));
  body->addInstruction(std::make_unique<BranchInst>("header"));
  auto exit = std::make_unique<BasicBlock>("exit");
  exit->addInstruction(std::make_unique<ReturnInst>());

  function->addBlock(std::move(entry));
  function->addBlock(std::move(header));
  function->addBlock(std::move(body));
  function->addBlock(std::move(exit));
  module.addFunction(std::move(function));

  zir::lowerDeadOwnedResults(module);
  auto *lowered = module.getFunctions().front().get();
  return expect(hasDestroyEdgeTo(*lowered, "exit", current),
                "loop phi ownership was not closed on the exit edge") &&
         expect(ZirVerifier().verify(module).ok(),
                "back-edge phi ownership lowering produced invalid ZIR") &&
         expect(ZirVerifier().verifyOwnershipObligations(module).ok(),
                "back-edge phi ownership lowering left an obligation");
}

bool testBreakAndContinuePathsShareOneOwnershipClosure() {
  Module module("ownership-break-continue");
  auto classType = std::make_shared<ClassType>("Node");
  auto boolean = primitive(TypeKind::Bool);
  auto function =
      std::make_unique<Function>("valid", primitive(TypeKind::Void));
  auto value = addOwnedArgument(*function, classType);

  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<BranchInst>("header"));
  auto header = std::make_unique<BasicBlock>("header");
  header->addInstruction(std::make_unique<CondBranchInst>(
      std::make_shared<Constant>("true", boolean), "body", "exit"));
  auto body = std::make_unique<BasicBlock>("body");
  body->addInstruction(std::make_unique<CondBranchInst>(
      std::make_shared<Constant>("true", boolean), "continue", "break"));
  auto continueBlock = std::make_unique<BasicBlock>("continue");
  continueBlock->addInstruction(std::make_unique<BranchInst>("header"));
  auto breakBlock = std::make_unique<BasicBlock>("break");
  breakBlock->addInstruction(std::make_unique<BranchInst>("exit"));
  auto exit = std::make_unique<BasicBlock>("exit");
  exit->addInstruction(std::make_unique<ReturnInst>());

  function->addBlock(std::move(entry));
  function->addBlock(std::move(header));
  function->addBlock(std::move(body));
  function->addBlock(std::move(continueBlock));
  function->addBlock(std::move(breakBlock));
  function->addBlock(std::move(exit));
  module.addFunction(std::move(function));

  zir::lowerDeadOwnedResults(module);
  auto *lowered = module.getFunctions().front().get();
  return expect(hasDestroyBeforeReturn(*lowered->findBlock("exit"), value),
                "break and continue paths did not share the exit cleanup") &&
         expect(ZirVerifier().verify(module).ok(),
                "break and continue ownership lowering produced invalid ZIR") &&
         expect(ZirVerifier().verifyOwnershipObligations(module).ok(),
                "break and continue ownership lowering left an obligation");
}

bool testUnreachableOwnershipDoesNotAffectReachableCleanup() {
  Module module("ownership-unreachable");
  auto classType = std::make_shared<ClassType>("Node");
  auto function =
      std::make_unique<Function>("valid", primitive(TypeKind::Void));

  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<ReturnInst>());
  auto unreachable = std::make_unique<BasicBlock>("unreachable");
  auto value = reg("value", classType);
  value->setOwnership(ValueOwnership::Owned);
  unreachable->addInstruction(
      std::make_unique<zir::AllocInst>(value, classType));
  unreachable->addInstruction(std::make_unique<ReturnInst>());

  function->addBlock(std::move(entry));
  function->addBlock(std::move(unreachable));
  module.addFunction(std::move(function));

  zir::lowerDeadOwnedResults(module);
  auto *lowered = module.getFunctions().front().get();
  const auto &entryInstructions =
      lowered->findBlock("entry")->getInstructions();
  return expect(entryInstructions.size() == 1 &&
                    entryInstructions.front()->getOpCode() == OpCode::Ret,
                "unreachable ownership changed reachable cleanup") &&
         expect(ZirVerifier().verify(module).ok(),
                "unreachable ownership produced invalid ZIR") &&
         expect(ZirVerifier().verifyOwnershipObligations(module).ok(),
                "unreachable ownership produced a reachable obligation");
}

bool testNestedDiamondClosesOwnershipExactlyOnce() {
  Module module("ownership-nested-diamond");
  auto classType = std::make_shared<ClassType>("Node");
  auto boolean = primitive(TypeKind::Bool);
  auto function =
      std::make_unique<Function>("valid", primitive(TypeKind::Void));
  auto value = addOwnedArgument(*function, classType);

  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<CondBranchInst>(
      std::make_shared<Constant>("true", boolean), "outer.left",
      "outer.right"));
  auto outerLeft = std::make_unique<BasicBlock>("outer.left");
  outerLeft->addInstruction(std::make_unique<CondBranchInst>(
      std::make_shared<Constant>("true", boolean), "inner.left",
      "inner.right"));
  auto innerLeft = std::make_unique<BasicBlock>("inner.left");
  innerLeft->addInstruction(std::make_unique<BranchInst>("merge"));
  auto innerRight = std::make_unique<BasicBlock>("inner.right");
  innerRight->addInstruction(std::make_unique<BranchInst>("merge"));
  auto outerRight = std::make_unique<BasicBlock>("outer.right");
  outerRight->addInstruction(std::make_unique<BranchInst>("merge"));
  auto merge = std::make_unique<BasicBlock>("merge");
  merge->addInstruction(std::make_unique<ReturnInst>());

  function->addBlock(std::move(entry));
  function->addBlock(std::move(outerLeft));
  function->addBlock(std::move(innerLeft));
  function->addBlock(std::move(innerRight));
  function->addBlock(std::move(outerRight));
  function->addBlock(std::move(merge));
  module.addFunction(std::move(function));

  zir::lowerDeadOwnedResults(module);
  auto *lowered = module.getFunctions().front().get();
  return expect(hasDestroyBeforeReturn(*lowered->findBlock("merge"), value),
                "nested diamond did not close ownership once at the merge") &&
         expect(ZirVerifier().verify(module).ok(),
                "nested diamond ownership lowering produced invalid ZIR") &&
         expect(ZirVerifier().verifyOwnershipObligations(module).ok(),
                "nested diamond ownership lowering left an obligation");
}

bool testUsedMixedOwnershipPhiIsRejected() {
  Module module("ownership-mixed-phi");
  auto classType = std::make_shared<ClassType>("Node");
  auto boolean = primitive(TypeKind::Bool);
  auto function =
      std::make_unique<Function>("broken", primitive(TypeKind::Void));
  auto owned = std::make_shared<zir::Argument>("owned", classType);
  owned->setOwnership(ValueOwnership::Owned);
  auto borrowed = std::make_shared<zir::Argument>("borrowed", classType);
  function->arguments.push_back(owned);
  function->arguments.push_back(borrowed);

  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<CondBranchInst>(
      std::make_shared<Constant>("true", boolean), "owned.path",
      "borrowed.path"));
  auto ownedPath = std::make_unique<BasicBlock>("owned.path");
  ownedPath->addInstruction(std::make_unique<BranchInst>("merge"));
  auto borrowedPath = std::make_unique<BasicBlock>("borrowed.path");
  borrowedPath->addInstruction(std::make_unique<BranchInst>("merge"));
  auto merge = std::make_unique<BasicBlock>("merge");
  auto result = reg("result", classType);
  merge->addInstruction(std::make_unique<PhiInst>(
      result, std::vector<std::pair<std::string, std::shared_ptr<zir::Value>>>{
                  {"owned.path", owned}, {"borrowed.path", borrowed}}));
  auto comparison = reg("comparison", boolean);
  merge->addInstruction(std::make_unique<CmpInst>(
      "eq", comparison, result, std::make_shared<Constant>("null", classType)));
  merge->addInstruction(std::make_unique<ReturnInst>());

  function->addBlock(std::move(entry));
  function->addBlock(std::move(ownedPath));
  function->addBlock(std::move(borrowedPath));
  function->addBlock(std::move(merge));
  module.addFunction(std::move(function));

  const auto verification = ZirVerifier().verify(module);
  return expect(
      hasError(verification, VerificationErrorCode::InvalidResult) &&
          verification.format().find("matching ownership") != std::string::npos,
      "used phi with mixed ownership incoming values was not rejected");
}

} // namespace

int main() {
  bool ok = true;
  ok = testLoopExitClosesOwnershipForZeroOneOrManyIterations() && ok;
  ok = testLoopBackEdgePhiTransfersOwnership() && ok;
  ok = testBreakAndContinuePathsShareOneOwnershipClosure() && ok;
  ok = testUnreachableOwnershipDoesNotAffectReachableCleanup() && ok;
  ok = testNestedDiamondClosesOwnershipExactlyOnce() && ok;
  ok = testUsedMixedOwnershipPhiIsRejected() && ok;
  return ok ? 0 : 1;
}
