#include "ir/borrow_provenance.hpp"
#include "ir/ownership_liveness.hpp"
#include "ir/string_type.hpp"
#include "ir/zir_verifier.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using zir::BasicBlock;
using zir::BorrowInst;
using zir::BranchInst;
using zir::CallInst;
using zir::CastInst;
using zir::CondBranchInst;
using zir::Constant;
using zir::ControlFlowGraph;
using zir::Function;
using zir::LoadInst;
using zir::Module;
using zir::ParameterEscape;
using zir::PhiInst;
using zir::PointerType;
using zir::PrimitiveType;
using zir::Register;
using zir::ResultBorrowContract;
using zir::ReturnInst;
using zir::StoreInst;
using zir::StoreMode;
using zir::Type;
using zir::TypeKind;
using zir::ValueOwnership;
using zir::VerificationErrorCode;
using zir::VerificationResult;
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

bool hasError(const VerificationResult &result, VerificationErrorCode code,
              const std::string &messageFragment = {}) {
  for (const auto &error : result.errors()) {
    if (error.code == code &&
        (messageFragment.empty() ||
         error.message.find(messageFragment) != std::string::npos)) {
      return true;
    }
  }
  return false;
}

void addOwnedStringAndView(BasicBlock &block, const std::string &name,
                           const std::shared_ptr<zir::Value> &slot,
                           StoreMode storeMode,
                           std::shared_ptr<Register> &owner) {
  const auto stringType = zir::makeStringType();
  owner = reg(name + ".owner", stringType);
  owner->setOwnership(ValueOwnership::OwnedStrong);
  auto view = reg(name + ".view", zir::makeStringViewType());
  block.addInstruction(std::make_unique<CallInst>(
      owner, "make", std::vector<std::shared_ptr<zir::Value>>{},
      std::vector<bool>{}, nullptr));
  block.addInstruction(std::make_unique<BorrowInst>(view, owner));
  block.addInstruction(std::make_unique<StoreInst>(view, slot, storeMode));
}

bool testStorageProvenanceCrossesPassThroughBlocks() {
  const auto stringViewType = zir::makeStringViewType();
  Module module("storage-provenance");
  auto function =
      std::make_unique<Function>("diamond", primitive(TypeKind::Void));

  auto entry = std::make_unique<BasicBlock>("entry");
  auto slot = reg("slot", std::make_shared<PointerType>(stringViewType));
  entry->addInstruction(
      std::make_unique<zir::AllocaInst>(slot, stringViewType));
  entry->addInstruction(std::make_unique<CondBranchInst>(
      std::make_shared<Constant>("true", primitive(TypeKind::Bool)), "left",
      "right"));

  auto left = std::make_unique<BasicBlock>("left");
  std::shared_ptr<Register> leftOwner;
  addOwnedStringAndView(*left, "left", slot, StoreMode::Initialize, leftOwner);
  left->addInstruction(std::make_unique<BranchInst>("left.forward"));

  auto leftForward = std::make_unique<BasicBlock>("left.forward");
  leftForward->addInstruction(std::make_unique<BranchInst>("merge"));

  auto right = std::make_unique<BasicBlock>("right");
  std::shared_ptr<Register> rightOwner;
  addOwnedStringAndView(*right, "right", slot, StoreMode::Assign, rightOwner);
  right->addInstruction(std::make_unique<BranchInst>("right.forward"));

  auto rightForward = std::make_unique<BasicBlock>("right.forward");
  rightForward->addInstruction(std::make_unique<BranchInst>("merge"));

  auto merge = std::make_unique<BasicBlock>("merge");
  auto loaded = reg("loaded", stringViewType);
  merge->addInstruction(std::make_unique<LoadInst>(loaded, slot));
  merge->addInstruction(std::make_unique<CallInst>(
      nullptr, "consume", std::vector<std::shared_ptr<zir::Value>>{loaded}));
  merge->addInstruction(std::make_unique<ReturnInst>());

  const auto *leftForwardBlock = leftForward.get();
  const auto *rightForwardBlock = rightForward.get();
  const auto *mergeBlock = merge.get();
  function->addBlock(std::move(entry));
  function->addBlock(std::move(left));
  function->addBlock(std::move(leftForward));
  function->addBlock(std::move(right));
  function->addBlock(std::move(rightForward));
  function->addBlock(std::move(merge));

  const ControlFlowGraph cfg(*function);
  const auto provenance = zir::analyzeBorrowProvenance(module, *function, cfg);
  const auto leftOwners =
      provenance.ownersOnEdge(loaded, *leftForwardBlock, *mergeBlock);
  const auto rightOwners =
      provenance.ownersOnEdge(loaded, *rightForwardBlock, *mergeBlock);
  const auto liveness = zir::analyzeOwnershipLiveness(module, *function);
  return expect(
      leftOwners.count(leftOwner.get()) == 1 &&
          leftOwners.count(rightOwner.get()) == 0 &&
          rightOwners.count(rightOwner.get()) == 1 &&
          rightOwners.count(leftOwner.get()) == 0 &&
          liveness.isLiveOnEdge(*leftForwardBlock, *mergeBlock, leftOwner) &&
          !liveness.isLiveOnEdge(*leftForwardBlock, *mergeBlock, rightOwner) &&
          liveness.isLiveOnEdge(*rightForwardBlock, *mergeBlock, rightOwner) &&
          !liveness.isLiveOnEdge(*rightForwardBlock, *mergeBlock, leftOwner),
      "storage borrow provenance did not cross pass-through CFG blocks");
}

bool testPhiProvenanceSelectsIncomingOwner() {
  const auto stringType = zir::makeStringType();
  const auto stringViewType = zir::makeStringViewType();
  Module module("phi-provenance");
  auto function = std::make_unique<Function>("phi", primitive(TypeKind::Void));

  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<CondBranchInst>(
      std::make_shared<Constant>("true", primitive(TypeKind::Bool)), "left",
      "right"));

  auto left = std::make_unique<BasicBlock>("left");
  auto leftOwner = reg("left.owner", stringType);
  leftOwner->setOwnership(ValueOwnership::OwnedStrong);
  auto leftView = reg("left.view", stringViewType);
  left->addInstruction(std::make_unique<CallInst>(
      leftOwner, "make", std::vector<std::shared_ptr<zir::Value>>{},
      std::vector<bool>{}, nullptr));
  left->addInstruction(std::make_unique<BorrowInst>(leftView, leftOwner));
  left->addInstruction(std::make_unique<BranchInst>("merge"));

  auto right = std::make_unique<BasicBlock>("right");
  auto rightOwner = reg("right.owner", stringType);
  rightOwner->setOwnership(ValueOwnership::OwnedStrong);
  auto rightView = reg("right.view", stringViewType);
  right->addInstruction(std::make_unique<CallInst>(
      rightOwner, "make", std::vector<std::shared_ptr<zir::Value>>{},
      std::vector<bool>{}, nullptr));
  right->addInstruction(std::make_unique<BorrowInst>(rightView, rightOwner));
  right->addInstruction(std::make_unique<BranchInst>("merge"));

  auto merge = std::make_unique<BasicBlock>("merge");
  auto mergedView = reg("merged.view", stringViewType);
  auto derivedView = reg("derived.view", stringViewType);
  merge->addInstruction(std::make_unique<PhiInst>(
      mergedView,
      std::vector<std::pair<std::string, std::shared_ptr<zir::Value>>>{
          {"left", leftView}, {"right", rightView}}));
  merge->addInstruction(
      std::make_unique<CastInst>(derivedView, mergedView, stringViewType));
  merge->addInstruction(std::make_unique<ReturnInst>());

  const auto *leftBlock = left.get();
  const auto *rightBlock = right.get();
  const auto *mergeBlock = merge.get();
  function->addBlock(std::move(entry));
  function->addBlock(std::move(left));
  function->addBlock(std::move(right));
  function->addBlock(std::move(merge));

  const ControlFlowGraph cfg(*function);
  const auto provenance = zir::analyzeBorrowProvenance(module, *function, cfg);
  const auto &allOwners = provenance.ownersOf(mergedView);
  const auto leftOwners =
      provenance.ownersOnEdge(mergedView, *leftBlock, *mergeBlock);
  const auto rightOwners =
      provenance.ownersOnEdge(mergedView, *rightBlock, *mergeBlock);
  const auto derivedLeftOwners =
      provenance.ownersOnEdge(derivedView, *leftBlock, *mergeBlock);
  const auto derivedRightOwners =
      provenance.ownersOnEdge(derivedView, *rightBlock, *mergeBlock);
  return expect(allOwners.count(leftOwner.get()) == 1 &&
                    allOwners.count(rightOwner.get()) == 1 &&
                    leftOwners.count(leftOwner.get()) == 1 &&
                    leftOwners.count(rightOwner.get()) == 0 &&
                    rightOwners.count(rightOwner.get()) == 1 &&
                    rightOwners.count(leftOwner.get()) == 0 &&
                    derivedLeftOwners == leftOwners &&
                    derivedRightOwners == rightOwners,
                "phi borrow provenance did not select its incoming edge owner");
}

bool testStorageProvenanceReachesLoopBackEdge() {
  const auto stringViewType = zir::makeStringViewType();
  Module module("loop-storage-provenance");
  auto function = std::make_unique<Function>("loop", primitive(TypeKind::Void));

  auto entry = std::make_unique<BasicBlock>("entry");
  auto slot = reg("slot", std::make_shared<PointerType>(stringViewType));
  entry->addInstruction(
      std::make_unique<zir::AllocaInst>(slot, stringViewType));
  std::shared_ptr<Register> owner;
  addOwnedStringAndView(*entry, "loop", slot, StoreMode::Initialize, owner);
  entry->addInstruction(std::make_unique<BranchInst>("loop"));

  auto loop = std::make_unique<BasicBlock>("loop");
  auto loaded = reg("loaded", stringViewType);
  loop->addInstruction(std::make_unique<LoadInst>(loaded, slot));
  loop->addInstruction(std::make_unique<CallInst>(
      nullptr, "consume", std::vector<std::shared_ptr<zir::Value>>{loaded}));
  loop->addInstruction(std::make_unique<CondBranchInst>(
      std::make_shared<Constant>("true", primitive(TypeKind::Bool)), "loop",
      "exit"));

  auto exit = std::make_unique<BasicBlock>("exit");
  exit->addInstruction(std::make_unique<ReturnInst>());

  const auto *entryBlock = entry.get();
  const auto *loopBlock = loop.get();
  function->addBlock(std::move(entry));
  function->addBlock(std::move(loop));
  function->addBlock(std::move(exit));

  const ControlFlowGraph cfg(*function);
  const auto provenance = zir::analyzeBorrowProvenance(module, *function, cfg);
  const auto backEdgeOwners =
      provenance.ownersOnEdge(loaded, *loopBlock, *loopBlock);
  const auto liveness = zir::analyzeOwnershipLiveness(module, *function);
  return expect(backEdgeOwners.count(owner.get()) == 1 &&
                    liveness.isLiveOnEdge(*entryBlock, *loopBlock, owner) &&
                    liveness.isLiveOnEdge(*loopBlock, *loopBlock, owner),
                "storage borrow provenance did not reach the loop back-edge");
}

bool testVerifierReportsLocalOwnerThroughPhi() {
  const auto stringType = zir::makeStringType();
  const auto stringViewType = zir::makeStringViewType();
  Module module("borrow-phi-escape");
  module.addExternalFunction(std::make_unique<Function>("make", stringType));

  auto function = std::make_unique<Function>("broken", stringViewType);
  auto callerView =
      std::make_shared<zir::Argument>("caller.view", stringViewType);
  function->arguments.push_back(callerView);

  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<CondBranchInst>(
      std::make_shared<Constant>("true", primitive(TypeKind::Bool)), "local",
      "caller"));

  auto local = std::make_unique<BasicBlock>("local");
  auto localOwner = reg("local.owner", stringType);
  localOwner->setOwnership(ValueOwnership::OwnedStrong);
  auto localView = reg("local.view", stringViewType);
  local->addInstruction(std::make_unique<CallInst>(
      localOwner, "make", std::vector<std::shared_ptr<zir::Value>>{},
      std::vector<bool>{}, nullptr));
  local->addInstruction(std::make_unique<BorrowInst>(localView, localOwner));
  local->addInstruction(std::make_unique<BranchInst>("merge"));

  auto caller = std::make_unique<BasicBlock>("caller");
  caller->addInstruction(std::make_unique<BranchInst>("merge"));

  auto merge = std::make_unique<BasicBlock>("merge");
  auto mergedView = reg("merged.view", stringViewType);
  merge->addInstruction(std::make_unique<PhiInst>(
      mergedView,
      std::vector<std::pair<std::string, std::shared_ptr<zir::Value>>>{
          {"local", localView}, {"caller", callerView}}));
  merge->addInstruction(std::make_unique<ReturnInst>(mergedView));

  function->addBlock(std::move(entry));
  function->addBlock(std::move(local));
  function->addBlock(std::move(caller));
  function->addBlock(std::move(merge));
  module.addFunction(std::move(function));

  const auto verification = ZirVerifier().verify(module);
  return expect(hasError(verification, VerificationErrorCode::InvalidReturn,
                         "%local.owner"),
                "verifier did not identify the local owner of a phi escape");
}

bool testVerifierAllowsCallerBorrowPhiReturn() {
  const auto stringViewType = zir::makeStringViewType();
  Module module("borrowed-phi-return");
  auto function = std::make_unique<Function>("valid", stringViewType);
  auto leftView = std::make_shared<zir::Argument>("left", stringViewType);
  auto rightView = std::make_shared<zir::Argument>("right", stringViewType);
  function->arguments.push_back(leftView);
  function->arguments.push_back(rightView);

  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<CondBranchInst>(
      std::make_shared<Constant>("true", primitive(TypeKind::Bool)), "left",
      "right"));
  auto left = std::make_unique<BasicBlock>("left");
  left->addInstruction(std::make_unique<BranchInst>("merge"));
  auto right = std::make_unique<BasicBlock>("right");
  right->addInstruction(std::make_unique<BranchInst>("merge"));
  auto merge = std::make_unique<BasicBlock>("merge");
  auto mergedView = reg("merged.view", stringViewType);
  merge->addInstruction(std::make_unique<PhiInst>(
      mergedView,
      std::vector<std::pair<std::string, std::shared_ptr<zir::Value>>>{
          {"left", leftView}, {"right", rightView}}));
  merge->addInstruction(std::make_unique<ReturnInst>(mergedView));

  function->addBlock(std::move(entry));
  function->addBlock(std::move(left));
  function->addBlock(std::move(right));
  function->addBlock(std::move(merge));
  module.addFunction(std::move(function));

  const auto verification = ZirVerifier().verify(module);
  return expect(verification.ok(),
                "verifier rejected a phi containing only caller borrows:\n" +
                    verification.format());
}

bool testVerifierReportsOwnerThroughStorageAndDerivedView() {
  const auto stringType = zir::makeStringType();
  const auto stringViewType = zir::makeStringViewType();
  Module module("borrow-storage-derived-escape");
  module.addExternalFunction(std::make_unique<Function>("make", stringType));
  auto function = std::make_unique<Function>("broken", stringViewType);

  auto entry = std::make_unique<BasicBlock>("entry");
  auto owner = reg("stored.owner", stringType);
  owner->setOwnership(ValueOwnership::OwnedStrong);
  auto view = reg("stored.view", stringViewType);
  auto slot = reg("slot", std::make_shared<PointerType>(stringViewType));
  entry->addInstruction(std::make_unique<CallInst>(
      owner, "make", std::vector<std::shared_ptr<zir::Value>>{},
      std::vector<bool>{}, nullptr));
  entry->addInstruction(std::make_unique<BorrowInst>(view, owner));
  entry->addInstruction(
      std::make_unique<zir::AllocaInst>(slot, stringViewType));
  entry->addInstruction(
      std::make_unique<StoreInst>(view, slot, StoreMode::Initialize));
  entry->addInstruction(std::make_unique<BranchInst>("forward"));

  auto forward = std::make_unique<BasicBlock>("forward");
  forward->addInstruction(std::make_unique<BranchInst>("exit"));

  auto exit = std::make_unique<BasicBlock>("exit");
  auto loaded = reg("loaded.view", stringViewType);
  auto derived = reg("derived.view", stringViewType);
  exit->addInstruction(std::make_unique<LoadInst>(loaded, slot));
  exit->addInstruction(
      std::make_unique<CastInst>(derived, loaded, stringViewType));
  exit->addInstruction(std::make_unique<ReturnInst>(derived));

  function->addBlock(std::move(entry));
  function->addBlock(std::move(forward));
  function->addBlock(std::move(exit));
  module.addFunction(std::move(function));

  const auto verification = ZirVerifier().verify(module);
  return expect(
      hasError(verification, VerificationErrorCode::InvalidReturn,
               "%stored.owner"),
      "verifier lost the local owner through storage and a derived view");
}

bool testVerifierAllowsOverwrittenLocalBorrowReturn() {
  const auto stringType = zir::makeStringType();
  const auto stringViewType = zir::makeStringViewType();
  Module module("overwritten-local-borrow");
  module.addExternalFunction(std::make_unique<Function>("make", stringType));
  auto function = std::make_unique<Function>("valid", stringViewType);
  auto callerView =
      std::make_shared<zir::Argument>("caller.view", stringViewType);
  function->arguments.push_back(callerView);

  auto entry = std::make_unique<BasicBlock>("entry");
  auto owner = reg("local.owner", stringType);
  owner->setOwnership(ValueOwnership::OwnedStrong);
  auto localView = reg("local.view", stringViewType);
  auto slot = reg("slot", std::make_shared<PointerType>(stringViewType));
  entry->addInstruction(std::make_unique<CallInst>(
      owner, "make", std::vector<std::shared_ptr<zir::Value>>{},
      std::vector<bool>{}, nullptr));
  entry->addInstruction(std::make_unique<BorrowInst>(localView, owner));
  entry->addInstruction(
      std::make_unique<zir::AllocaInst>(slot, stringViewType));
  entry->addInstruction(
      std::make_unique<StoreInst>(localView, slot, StoreMode::Initialize));
  entry->addInstruction(std::make_unique<BranchInst>("overwrite"));

  auto overwrite = std::make_unique<BasicBlock>("overwrite");
  overwrite->addInstruction(
      std::make_unique<StoreInst>(callerView, slot, StoreMode::Assign));
  overwrite->addInstruction(std::make_unique<BranchInst>("exit"));

  auto exit = std::make_unique<BasicBlock>("exit");
  auto loaded = reg("loaded.view", stringViewType);
  exit->addInstruction(std::make_unique<LoadInst>(loaded, slot));
  exit->addInstruction(std::make_unique<ReturnInst>(loaded));

  function->addBlock(std::move(entry));
  function->addBlock(std::move(overwrite));
  function->addBlock(std::move(exit));
  module.addFunction(std::move(function));

  const auto verification = ZirVerifier().verify(module);
  return expect(
      verification.ok(),
      "verifier retained a stale owner after overwriting local storage:\n" +
          verification.format());
}

bool testVerifierRejectsNoEscapeParameterReturn() {
  const auto stringViewType = zir::makeStringViewType();
  Module module("noescape-return");
  auto function = std::make_unique<Function>("broken", stringViewType);
  auto view = std::make_shared<zir::Argument>(
      "view", stringViewType, false, false, nullptr,
      zir::ParameterOwnership::Borrow, ParameterEscape::NoEscape);
  function->arguments.push_back(view);
  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<ReturnInst>(view));
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));

  const auto verification = ZirVerifier().verify(module);
  return expect(
      hasError(verification, VerificationErrorCode::InvalidReturn, "%view"),
      "verifier allowed a noescape parameter to be returned");
}

bool testVerifierRejectsNoEscapeForwardingToUnspecifiedParameter() {
  const auto stringViewType = zir::makeStringViewType();
  Module module("noescape-forwarding");
  auto consume =
      std::make_unique<Function>("consume", primitive(TypeKind::Void));
  consume->arguments.push_back(
      std::make_shared<zir::Argument>("view", stringViewType));
  module.addExternalFunction(std::move(consume));

  auto function =
      std::make_unique<Function>("broken", primitive(TypeKind::Void));
  auto view = std::make_shared<zir::Argument>(
      "view", stringViewType, false, false, nullptr,
      zir::ParameterOwnership::Borrow, ParameterEscape::NoEscape);
  function->arguments.push_back(view);
  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<CallInst>(
      nullptr, "consume", std::vector<std::shared_ptr<zir::Value>>{view}));
  entry->addInstruction(std::make_unique<ReturnInst>());
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));

  const auto verification = ZirVerifier().verify(module);
  return expect(hasError(verification, VerificationErrorCode::InvalidCall,
                         "unspecified escape"),
                "verifier allowed noescape forwarding to an unspecified "
                "parameter");
}

void addStringFactory(Module &module) {
  module.addExternalFunction(
      std::make_unique<Function>("make", zir::makeStringType()));
}

bool testVerifierRejectsLocalBorrowForwardingToUnspecifiedParameter() {
  const auto stringViewType = zir::makeStringViewType();
  Module module("local-borrow-forwarding");
  addStringFactory(module);
  auto consume =
      std::make_unique<Function>("consume", primitive(TypeKind::Void));
  consume->arguments.push_back(
      std::make_shared<zir::Argument>("view", stringViewType));
  module.addExternalFunction(std::move(consume));

  auto function =
      std::make_unique<Function>("broken", primitive(TypeKind::Void));
  auto owner = reg("owner", zir::makeStringType());
  owner->setOwnership(ValueOwnership::OwnedStrong);
  auto view = reg("view", stringViewType);
  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<CallInst>(
      owner, "make", std::vector<std::shared_ptr<zir::Value>>{},
      std::vector<bool>{}, nullptr));
  entry->addInstruction(std::make_unique<BorrowInst>(view, owner));
  entry->addInstruction(std::make_unique<CallInst>(
      nullptr, "consume", std::vector<std::shared_ptr<zir::Value>>{view}));
  entry->addInstruction(std::make_unique<zir::DestroyInst>(owner));
  entry->addInstruction(std::make_unique<ReturnInst>());
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));

  const auto verification = ZirVerifier().verify(module);
  return expect(
      hasError(verification, VerificationErrorCode::InvalidCall,
               "tracked borrow source %owner"),
      "verifier allowed a local borrow to cross an unspecified call boundary");
}

bool testVerifierAllowsLocalBorrowForwardingToNoEscapeParameter() {
  const auto stringViewType = zir::makeStringViewType();
  Module module("local-borrow-noescape-forwarding");
  addStringFactory(module);
  auto consume =
      std::make_unique<Function>("consume", primitive(TypeKind::Void));
  consume->arguments.push_back(std::make_shared<zir::Argument>(
      "view", stringViewType, false, false, nullptr,
      zir::ParameterOwnership::Borrow, ParameterEscape::NoEscape));
  module.addExternalFunction(std::move(consume));

  auto function =
      std::make_unique<Function>("valid", primitive(TypeKind::Void));
  auto owner = reg("owner", zir::makeStringType());
  owner->setOwnership(ValueOwnership::OwnedStrong);
  auto view = reg("view", stringViewType);
  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<CallInst>(
      owner, "make", std::vector<std::shared_ptr<zir::Value>>{},
      std::vector<bool>{}, nullptr));
  entry->addInstruction(std::make_unique<BorrowInst>(view, owner));
  entry->addInstruction(std::make_unique<CallInst>(
      nullptr, "consume", std::vector<std::shared_ptr<zir::Value>>{view},
      std::vector<bool>{false}, nullptr));
  entry->addInstruction(std::make_unique<zir::DestroyInst>(owner));
  entry->addInstruction(std::make_unique<ReturnInst>());
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));

  const auto verification = ZirVerifier().verify(module);
  return expect(verification.ok(),
                "verifier rejected a local borrow passed to noescape:\n" +
                    verification.format());
}

bool testVerifierAllowsNoEscapeForwarding() {
  const auto stringViewType = zir::makeStringViewType();
  Module module("noescape-forwarding-valid");
  auto consume =
      std::make_unique<Function>("consume", primitive(TypeKind::Void));
  consume->arguments.push_back(std::make_shared<zir::Argument>(
      "view", stringViewType, false, false, nullptr,
      zir::ParameterOwnership::Borrow, ParameterEscape::NoEscape));
  module.addExternalFunction(std::move(consume));

  auto function =
      std::make_unique<Function>("valid", primitive(TypeKind::Void));
  auto view = std::make_shared<zir::Argument>(
      "view", stringViewType, false, false, nullptr,
      zir::ParameterOwnership::Borrow, ParameterEscape::NoEscape);
  function->arguments.push_back(view);
  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<CallInst>(
      nullptr, "consume", std::vector<std::shared_ptr<zir::Value>>{view},
      std::vector<bool>{false}, nullptr));
  entry->addInstruction(std::make_unique<ReturnInst>());
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));

  const auto verification = ZirVerifier().verify(module);
  return expect(verification.ok(),
                "verifier rejected valid noescape forwarding:\n" +
                    verification.format());
}

bool testVerifierDerivesNoEscapeFromCalleeContract() {
  const auto stringViewType = zir::makeStringViewType();
  Module module("noescape-call-metadata");
  auto consume =
      std::make_unique<Function>("consume", primitive(TypeKind::Void));
  consume->arguments.push_back(std::make_shared<zir::Argument>(
      "view", stringViewType, false, false, nullptr,
      zir::ParameterOwnership::Borrow, ParameterEscape::NoEscape));
  module.addExternalFunction(std::move(consume));

  auto function =
      std::make_unique<Function>("broken", primitive(TypeKind::Void));
  auto view = std::make_shared<zir::Argument>("view", stringViewType);
  function->arguments.push_back(view);
  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<CallInst>(
      nullptr, "consume", std::vector<std::shared_ptr<zir::Value>>{view}));
  entry->addInstruction(std::make_unique<ReturnInst>());
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));

  const auto verification = ZirVerifier().verify(module);
  return expect(verification.ok(),
                "verifier did not derive noescape from the callee contract:\n" +
                    verification.format());
}

std::unique_ptr<Function> makeBorrowingViewFunction(const std::string &name) {
  auto function = std::make_unique<Function>(name, zir::makeStringViewType());
  function->arguments.push_back(
      std::make_shared<zir::Argument>("source", zir::makeStringViewType()));
  function->resultBorrow = ResultBorrowContract::fromParameter(0);
  return function;
}

bool testVerifierTracksBorrowedCallResultToLocalOwner() {
  const auto stringViewType = zir::makeStringViewType();
  Module module("borrowed-call-local-owner");
  module.addExternalFunction(makeBorrowingViewFunction("tail"));

  auto function = std::make_unique<Function>("broken", stringViewType);
  auto entry = std::make_unique<BasicBlock>("entry");
  auto owner = reg("owner", zir::makeStringType());
  owner->setOwnership(ValueOwnership::OwnedStrong);
  auto source = reg("source", stringViewType);
  auto result = reg("result", stringViewType);
  entry->addInstruction(std::make_unique<CallInst>(
      owner, "make", std::vector<std::shared_ptr<zir::Value>>{},
      std::vector<bool>{}, nullptr));
  entry->addInstruction(std::make_unique<BorrowInst>(source, owner));
  entry->addInstruction(std::make_unique<CallInst>(
      result, "tail", std::vector<std::shared_ptr<zir::Value>>{source},
      std::vector<bool>{false}, nullptr));
  entry->addInstruction(std::make_unique<ReturnInst>(result));
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));

  const auto verification = ZirVerifier().verify(module);
  return expect(
      hasError(verification, VerificationErrorCode::InvalidReturn, "%owner"),
      "verifier lost the local owner behind a borrowed call result");
}

bool testIndirectBorrowedCallResultExtendsTemporaryOwnerLiveness() {
  const auto stringViewType = zir::makeStringViewType();
  Module module("borrowed-call-liveness");
  auto tailType = std::make_shared<zir::FunctionPointerType>(
      std::vector<std::shared_ptr<Type>>{stringViewType}, stringViewType,
      std::vector<zir::ParameterOwnership>{zir::ParameterOwnership::Borrow},
      std::vector<ParameterEscape>{ParameterEscape::Unspecified},
      ResultBorrowContract::fromParameter(0));
  auto tail = std::make_shared<zir::FunctionReference>("tail", tailType);
  auto function =
      std::make_unique<Function>("temporary", primitive(TypeKind::Void));
  auto entry = std::make_unique<BasicBlock>("entry");
  auto owner = reg("owner", zir::makeStringType());
  owner->setOwnership(ValueOwnership::OwnedStrong);
  auto source = reg("source", stringViewType);
  auto result = reg("result", stringViewType);
  entry->addInstruction(std::make_unique<CallInst>(
      owner, "make", std::vector<std::shared_ptr<zir::Value>>{},
      std::vector<bool>{}, nullptr));
  entry->addInstruction(std::make_unique<BorrowInst>(source, owner));
  entry->addInstruction(std::make_unique<CallInst>(
      result, tail, std::vector<std::shared_ptr<zir::Value>>{source}));
  entry->addInstruction(std::make_unique<CallInst>(
      nullptr, "consume", std::vector<std::shared_ptr<zir::Value>>{result}));
  entry->addInstruction(std::make_unique<ReturnInst>());
  const auto *entryBlock = entry.get();
  function->addBlock(std::move(entry));

  const auto liveness = zir::analyzeOwnershipLiveness(module, *function);
  return expect(
      liveness.isLiveAfter(*entryBlock, 2, owner) &&
          !liveness.isLiveAfter(*entryBlock, 3, owner) &&
          liveness.isLastUse(*entryBlock, 3, owner),
      "borrowed call result did not extend its temporary owner to the "
      "result's last use");
}

bool testVerifierAllowsForwardedBorrowedCallResult() {
  const auto stringViewType = zir::makeStringViewType();
  Module module("borrowed-call-forwarding");
  module.addExternalFunction(makeBorrowingViewFunction("tail"));

  auto function = makeBorrowingViewFunction("forward");
  const auto source = function->arguments.front();
  auto result = reg("result", stringViewType);
  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<CallInst>(
      result, "tail", std::vector<std::shared_ptr<zir::Value>>{source},
      std::vector<bool>{false}, nullptr));
  entry->addInstruction(std::make_unique<ReturnInst>(result));
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));

  const auto verification = ZirVerifier().verify(module);
  return expect(verification.ok(),
                "verifier rejected a forwarded borrowed call result:\n" +
                    verification.format());
}

bool testVerifierRejectsReturnedNoEscapeCallResult() {
  const auto stringViewType = zir::makeStringViewType();
  Module module("borrowed-call-noescape");
  module.addExternalFunction(makeBorrowingViewFunction("tail"));

  auto function = std::make_unique<Function>("broken", stringViewType);
  auto source = std::make_shared<zir::Argument>(
      "source", stringViewType, false, false, nullptr,
      zir::ParameterOwnership::Borrow, ParameterEscape::NoEscape);
  function->arguments.push_back(source);
  auto result = reg("result", stringViewType);
  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<CallInst>(
      result, "tail", std::vector<std::shared_ptr<zir::Value>>{source},
      std::vector<bool>{false}, nullptr));
  entry->addInstruction(std::make_unique<ReturnInst>(result));
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));

  const auto verification = ZirVerifier().verify(module);
  return expect(
      hasError(verification, VerificationErrorCode::InvalidReturn, "%source"),
      "verifier allowed a borrowed call result to escape its noescape source");
}

bool testVerifierDerivesResultBorrowFromCalleeContract() {
  const auto stringViewType = zir::makeStringViewType();
  Module module("borrowed-call-metadata");
  module.addExternalFunction(makeBorrowingViewFunction("tail"));

  auto function =
      std::make_unique<Function>("broken", primitive(TypeKind::Void));
  auto source = std::make_shared<zir::Argument>("source", stringViewType);
  function->arguments.push_back(source);
  auto result = reg("result", stringViewType);
  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<CallInst>(
      result, "tail", std::vector<std::shared_ptr<zir::Value>>{source}));
  entry->addInstruction(std::make_unique<ReturnInst>());
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));

  const auto verification = ZirVerifier().verify(module);
  return expect(verification.ok(),
                "verifier did not derive result borrow from the callee "
                "contract:\n" +
                    verification.format());
}

} // namespace

int main() {
  bool ok = true;
  ok = testStorageProvenanceCrossesPassThroughBlocks() && ok;
  ok = testPhiProvenanceSelectsIncomingOwner() && ok;
  ok = testStorageProvenanceReachesLoopBackEdge() && ok;
  ok = testVerifierReportsLocalOwnerThroughPhi() && ok;
  ok = testVerifierAllowsCallerBorrowPhiReturn() && ok;
  ok = testVerifierReportsOwnerThroughStorageAndDerivedView() && ok;
  ok = testVerifierAllowsOverwrittenLocalBorrowReturn() && ok;
  ok = testVerifierRejectsNoEscapeParameterReturn() && ok;
  ok = testVerifierRejectsNoEscapeForwardingToUnspecifiedParameter() && ok;
  ok = testVerifierRejectsLocalBorrowForwardingToUnspecifiedParameter() && ok;
  ok = testVerifierAllowsLocalBorrowForwardingToNoEscapeParameter() && ok;
  ok = testVerifierAllowsNoEscapeForwarding() && ok;
  ok = testVerifierDerivesNoEscapeFromCalleeContract() && ok;
  ok = testVerifierTracksBorrowedCallResultToLocalOwner() && ok;
  ok = testIndirectBorrowedCallResultExtendsTemporaryOwnerLiveness() && ok;
  ok = testVerifierAllowsForwardedBorrowedCallResult() && ok;
  ok = testVerifierRejectsReturnedNoEscapeCallResult() && ok;
  ok = testVerifierDerivesResultBorrowFromCalleeContract() && ok;
  return ok ? 0 : 1;
}
