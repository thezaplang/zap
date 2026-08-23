#include "ir/control_flow_graph.hpp"
#include "ir/ownership_flow.hpp"
#include "ir/ownership_liveness.hpp"
#include "ir/ownership_lowering.hpp"
#include "ir/string_type.hpp"
#include "ir/zir_verifier.hpp"

#include <algorithm>
#include <iostream>
#include <memory>
#include <string>

namespace {

using zir::BasicBlock;
using zir::BorrowInst;
using zir::BranchInst;
using zir::CastInst;
using zir::ClassType;
using zir::CmpInst;
using zir::CondBranchInst;
using zir::Constant;
using zir::ControlFlowGraph;
using zir::CopyInst;
using zir::DestroyInst;
using zir::Function;
using zir::FunctionPointerType;
using zir::FunctionReference;
using zir::LoadInst;
using zir::Module;
using zir::MoveInst;
using zir::OpCode;
using zir::OwnershipDestroyPlacementKind;
using zir::OwnershipFlowAnalysis;
using zir::OwnershipFlowState;
using zir::PhiInst;
using zir::PointerType;
using zir::PrimitiveType;
using zir::Register;
using zir::ReturnInst;
using zir::StoreInst;
using zir::StoreMode;
using zir::Type;
using zir::TypeKind;
using zir::ValueOwnership;
using zir::VerificationErrorCode;
using zir::WeakLockInst;
using zir::ZirVerifier;

std::shared_ptr<Type> primitive(TypeKind kind) {
  return std::make_shared<PrimitiveType>(kind);
}

std::shared_ptr<Register> reg(const std::string &name,
                              const std::shared_ptr<Type> &type) {
  return std::make_shared<Register>(name, type);
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

std::unique_ptr<Function> validFunction() {
  auto i32 = primitive(TypeKind::Int32);
  auto boolean = primitive(TypeKind::Bool);
  auto function = std::make_unique<Function>("valid", i32);

  auto entry = std::make_unique<BasicBlock>("entry");
  auto slot = reg("slot", std::make_shared<PointerType>(i32));
  auto loaded = reg("loaded", i32);
  auto condition = reg("condition", boolean);
  entry->addInstruction(std::make_unique<zir::AllocaInst>(slot, i32));
  entry->addInstruction(std::make_unique<StoreInst>(
      std::make_shared<Constant>("4", i32), slot, zir::StoreMode::Assign));
  entry->addInstruction(std::make_unique<LoadInst>(loaded, slot));
  entry->addInstruction(std::make_unique<CmpInst>(
      "eq", condition, loaded, std::make_shared<Constant>("4", i32)));
  entry->addInstruction(
      std::make_unique<CondBranchInst>(condition, "left", "right"));

  auto left = std::make_unique<BasicBlock>("left");
  auto leftValue = reg("left.value", i32);
  left->addInstruction(std::make_unique<zir::BinaryInst>(
      OpCode::Add, leftValue, loaded, std::make_shared<Constant>("1", i32)));
  left->addInstruction(std::make_unique<BranchInst>("merge"));

  auto right = std::make_unique<BasicBlock>("right");
  auto rightValue = reg("right.value", i32);
  right->addInstruction(std::make_unique<zir::BinaryInst>(
      OpCode::Sub, rightValue, loaded, std::make_shared<Constant>("1", i32)));
  right->addInstruction(std::make_unique<BranchInst>("merge"));

  auto merge = std::make_unique<BasicBlock>("merge");
  auto result = reg("result", i32);
  merge->addInstruction(std::make_unique<PhiInst>(
      result, std::vector<std::pair<std::string, std::shared_ptr<zir::Value>>>{
                  {"left", leftValue}, {"right", rightValue}}));
  merge->addInstruction(std::make_unique<ReturnInst>(result));

  function->addBlock(std::move(entry));
  function->addBlock(std::move(left));
  function->addBlock(std::move(right));
  function->addBlock(std::move(merge));
  return function;
}

bool expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

bool testValidFunction() {
  Module module("valid-module");
  module.addFunction(validFunction());
  auto result = ZirVerifier().verify(module);
  return expect(result.ok(), "valid ZIR was rejected:\n" + result.format());
}

bool testMissingTerminator() {
  Module module("missing-terminator");
  auto function =
      std::make_unique<Function>("broken", primitive(TypeKind::Void));
  function->addBlock(std::make_unique<BasicBlock>("entry"));
  module.addFunction(std::move(function));
  auto result = ZirVerifier().verify(module);
  return expect(hasError(result, VerificationErrorCode::MissingTerminator),
                "missing terminator was not diagnosed");
}

bool testUnknownBranchTarget() {
  Module module("unknown-target");
  auto function =
      std::make_unique<Function>("broken", primitive(TypeKind::Void));
  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<BranchInst>("missing"));
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));
  auto result = ZirVerifier().verify(module);
  return expect(hasError(result, VerificationErrorCode::InvalidBranchTarget),
                "unknown branch target was not diagnosed");
}

bool testInstructionAfterTerminator() {
  Module module("instruction-after-terminator");
  auto i32 = primitive(TypeKind::Int32);
  auto function =
      std::make_unique<Function>("broken", primitive(TypeKind::Void));
  auto entry = std::make_unique<BasicBlock>("entry");
  auto dead = reg("dead", i32);
  entry->addInstruction(std::make_unique<ReturnInst>());
  entry->addInstruction(std::make_unique<zir::BinaryInst>(
      OpCode::Add, dead, std::make_shared<Constant>("1", i32),
      std::make_shared<Constant>("2", i32)));
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));
  auto result = ZirVerifier().verify(module);
  return expect(
      hasError(result, VerificationErrorCode::InstructionAfterTerminator),
      "instruction after terminator was not diagnosed");
}

bool testUseBeforeDefinition() {
  Module module("use-before-definition");
  auto i32 = primitive(TypeKind::Int32);
  auto function = std::make_unique<Function>("broken", i32);
  auto entry = std::make_unique<BasicBlock>("entry");
  auto value = reg("late", i32);
  auto result = reg("result", i32);
  entry->addInstruction(std::make_unique<zir::BinaryInst>(
      OpCode::Add, result, value, std::make_shared<Constant>("1", i32)));
  entry->addInstruction(std::make_unique<zir::BinaryInst>(
      OpCode::Add, value, std::make_shared<Constant>("1", i32),
      std::make_shared<Constant>("2", i32)));
  entry->addInstruction(std::make_unique<ReturnInst>(result));
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));
  auto verification = ZirVerifier().verify(module);
  return expect(
      hasError(verification, VerificationErrorCode::UseBeforeDefinition),
      "use before definition was not diagnosed");
}

bool testStoreTypeMismatch() {
  Module module("store-type-mismatch");
  auto i32 = primitive(TypeKind::Int32);
  auto i64 = primitive(TypeKind::Int64);
  auto function =
      std::make_unique<Function>("broken", primitive(TypeKind::Void));
  auto entry = std::make_unique<BasicBlock>("entry");
  auto slot = reg("slot", std::make_shared<PointerType>(i32));
  entry->addInstruction(std::make_unique<zir::AllocaInst>(slot, i32));
  entry->addInstruction(std::make_unique<StoreInst>(
      std::make_shared<Constant>("1", i64), slot, zir::StoreMode::Assign));
  entry->addInstruction(std::make_unique<ReturnInst>());
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));
  auto result = ZirVerifier().verify(module);
  return expect(hasError(result, VerificationErrorCode::TypeMismatch),
                "store type mismatch was not diagnosed");
}

bool testStoreModeRendering() {
  auto i32 = primitive(TypeKind::Int32);
  auto slot = reg("slot", std::make_shared<PointerType>(i32));
  auto value = std::make_shared<Constant>("1", i32);
  const std::vector<std::pair<StoreMode, std::string>> cases = {
      {StoreMode::Assign, "store.assign"},
      {StoreMode::Initialize, "store.initialize"},
      {StoreMode::RawAssign, "store.raw_assign"},
      {StoreMode::RawInitialize, "store.raw_initialize"},
  };
  for (const auto &[mode, prefix] : cases) {
    StoreInst store(value, slot, mode);
    const std::string rendered = store.toString();
    if (!expect(rendered.compare(0, prefix.size(), prefix) == 0,
                "store mode is missing from ZIR rendering")) {
      return false;
    }
  }
  return true;
}

bool testAllocRequiresOwnedResult() {
  Module module("alloc-ownership");
  auto classType = std::make_shared<zir::ClassType>("Node");
  auto function =
      std::make_unique<Function>("broken", primitive(TypeKind::Void));
  auto entry = std::make_unique<BasicBlock>("entry");
  auto result = reg("node", classType);
  entry->addInstruction(std::make_unique<zir::AllocInst>(result, classType));
  entry->addInstruction(std::make_unique<ReturnInst>());
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));
  auto verification = ZirVerifier().verify(module);
  return expect(hasError(verification, VerificationErrorCode::InvalidResult),
                "borrowed alloc result was not diagnosed");
}

bool testWeakLockRequiresOwnedStrongResult() {
  Module module("weak-lock-ownership");
  auto weakType = std::make_shared<ClassType>("Node");
  weakType->setWeak(true);
  auto strongType = std::make_shared<ClassType>(*weakType);
  strongType->setWeak(false);
  auto function =
      std::make_unique<Function>("broken", primitive(TypeKind::Void));
  auto weakValue = std::make_shared<zir::Argument>("weak", weakType);
  function->arguments.push_back(weakValue);
  auto entry = std::make_unique<BasicBlock>("entry");
  auto result = reg("locked", strongType);
  entry->addInstruction(std::make_unique<WeakLockInst>(result, weakValue));
  entry->addInstruction(std::make_unique<ReturnInst>());
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));

  auto verification = ZirVerifier().verify(module);
  return expect(hasError(verification, VerificationErrorCode::InvalidResult),
                "borrowed weak.lock result was not diagnosed");
}

bool testManagedCallRequiresOwnedResult() {
  Module module("managed-call-ownership");
  auto stringType = zir::makeStringType();
  auto functionType = std::make_shared<FunctionPointerType>(
      std::vector<std::shared_ptr<Type>>{}, stringType);
  auto callee =
      std::make_shared<FunctionReference>("make_string", functionType);
  auto function =
      std::make_unique<Function>("broken", primitive(TypeKind::Void));
  auto entry = std::make_unique<BasicBlock>("entry");
  auto result = reg("text", stringType);
  entry->addInstruction(std::make_unique<zir::CallInst>(
      result, callee, std::vector<std::shared_ptr<zir::Value>>{}));
  entry->addInstruction(std::make_unique<ReturnInst>());
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));

  auto verification = ZirVerifier().verify(module);
  return expect(hasError(verification, VerificationErrorCode::InvalidResult),
                "borrowed managed call result was not diagnosed");
}

bool testRefCallDerivesResultTypeFromSignature() {
  Module module("ref-call-signature");
  auto i32 = primitive(TypeKind::Int32);
  auto get = std::make_unique<Function>("get", i32);
  get->returnsRef = true;
  module.addExternalFunction(std::move(get));

  auto function =
      std::make_unique<Function>("valid", primitive(TypeKind::Void));
  auto directResult = reg("direct", std::make_shared<PointerType>(i32));
  auto indirectResult = reg("indirect", std::make_shared<PointerType>(i32));
  auto indirectType = std::make_shared<FunctionPointerType>(
      std::vector<std::shared_ptr<Type>>{}, i32,
      std::vector<zir::ParameterOwnership>{},
      std::vector<zir::ParameterEscape>{}, zir::ResultBorrowContract{}, true);
  auto indirect = std::make_shared<FunctionReference>("get", indirectType);
  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<zir::CallInst>(
      directResult, "get", std::vector<std::shared_ptr<zir::Value>>{}));
  entry->addInstruction(std::make_unique<zir::CallInst>(
      indirectResult, indirect, std::vector<std::shared_ptr<zir::Value>>{}));
  entry->addInstruction(std::make_unique<ReturnInst>());
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));

  return expect(ZirVerifier().verify(module).ok(),
                "ref call result type was not derived from its signature");
}

bool testManagedTypeClassification() {
  auto stringType = zir::makeStringType();
  auto stringViewType = zir::makeStringViewType();
  auto record = std::make_shared<zir::RecordType>("TextBox");
  record->addField("text", stringType);
  auto array = std::make_shared<zir::ArrayType>(record, 2);
  auto taggedUnion = std::make_shared<zir::TaggedUnionType>(
      "TextResult", std::vector<zir::TaggedUnionType::Variant>{
                        {"Empty", nullptr, 0}, {"Text", stringType, 1}});

  return expect(zir::containsManagedValues(stringType),
                "String was not classified as managed") &&
         expect(!zir::containsManagedValues(stringViewType),
                "StringView was classified as managed") &&
         expect(zir::containsManagedValues(record),
                "record containing String was not classified as managed") &&
         expect(zir::containsManagedValues(array),
                "array containing managed records was not classified as "
                "managed") &&
         expect(zir::containsManagedValues(taggedUnion),
                "tagged union containing String was not classified as managed");
}

bool testPhiRequiresOwnershipMatchingIncomingValues() {
  Module module("phi-ownership");
  auto stringType = zir::makeStringType();
  auto boolean = primitive(TypeKind::Bool);
  auto function =
      std::make_unique<Function>("broken", primitive(TypeKind::Void));
  auto leftValue = std::make_shared<zir::Argument>("left.value", stringType);
  auto rightValue = std::make_shared<zir::Argument>("right.value", stringType);
  leftValue->setOwnership(ValueOwnership::Owned);
  rightValue->setOwnership(ValueOwnership::Owned);
  function->arguments.push_back(leftValue);
  function->arguments.push_back(rightValue);

  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<CondBranchInst>(
      std::make_shared<Constant>("true", boolean), "left", "right"));
  auto left = std::make_unique<BasicBlock>("left");
  left->addInstruction(std::make_unique<BranchInst>("merge"));
  auto right = std::make_unique<BasicBlock>("right");
  right->addInstruction(std::make_unique<BranchInst>("merge"));
  auto merge = std::make_unique<BasicBlock>("merge");
  auto result = reg("result", stringType);
  merge->addInstruction(std::make_unique<PhiInst>(
      result, std::vector<std::pair<std::string, std::shared_ptr<zir::Value>>>{
                  {"left", leftValue}, {"right", rightValue}}));
  merge->addInstruction(std::make_unique<ReturnInst>());

  function->addBlock(std::move(entry));
  function->addBlock(std::move(left));
  function->addBlock(std::move(right));
  function->addBlock(std::move(merge));
  module.addFunction(std::move(function));

  auto verification = ZirVerifier().verify(module);
  return expect(
      hasError(verification, VerificationErrorCode::InvalidResult),
      "borrowed phi result with owned incoming values was not diagnosed");
}

bool testManagedInitializationRequiresOwnershipTransfer() {
  Module module("managed-initialization-ownership");
  auto stringType = zir::makeStringType();
  auto function =
      std::make_unique<Function>("broken", primitive(TypeKind::Void));
  auto value = std::make_shared<zir::Argument>("value", stringType);
  function->arguments.push_back(value);

  auto entry = std::make_unique<BasicBlock>("entry");
  auto slot = reg("slot", std::make_shared<PointerType>(stringType));
  entry->addInstruction(std::make_unique<zir::AllocaInst>(slot, stringType));
  entry->addInstruction(
      std::make_unique<StoreInst>(value, slot, StoreMode::Initialize));
  entry->addInstruction(std::make_unique<ReturnInst>());
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));

  return expect(hasError(ZirVerifier().verify(module),
                         VerificationErrorCode::InvalidOperand),
                "managed initialization accepted a borrowed source");
}

bool testCastRequiresOwnershipMatchingSourceAndTarget() {
  Module module("cast-ownership");
  auto strongType = std::make_shared<ClassType>("Node");
  auto weakType = std::make_shared<ClassType>(*strongType);
  weakType->setWeak(true);
  auto function =
      std::make_unique<Function>("broken", primitive(TypeKind::Void));
  auto source = std::make_shared<zir::Argument>("source", strongType);
  source->setOwnership(ValueOwnership::Owned);
  function->arguments.push_back(source);

  auto entry = std::make_unique<BasicBlock>("entry");
  auto result = reg("result", weakType);
  entry->addInstruction(std::make_unique<CastInst>(result, source, weakType));
  entry->addInstruction(std::make_unique<ReturnInst>());
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));

  auto verification = ZirVerifier().verify(module);
  return expect(hasError(verification, VerificationErrorCode::InvalidResult),
                "borrowed managed cast result was not diagnosed");
}

bool testCallRequiresOwnershipMatchingArguments() {
  Module module("call-ownership");
  auto stringType = zir::makeStringType();
  auto function =
      std::make_unique<Function>("broken", primitive(TypeKind::Void));
  auto argument = std::make_shared<zir::Argument>("argument", stringType);
  argument->setOwnership(ValueOwnership::Owned);
  function->arguments.push_back(argument);

  auto entry = std::make_unique<BasicBlock>("entry");
  auto calleeType = std::make_shared<FunctionPointerType>(
      std::vector<std::shared_ptr<Type>>{stringType}, primitive(TypeKind::Void),
      std::vector<zir::ParameterOwnership>{zir::ParameterOwnership::Transfer});
  auto callee = std::make_shared<FunctionReference>("callee", calleeType);
  argument->setOwnership(ValueOwnership::Borrowed);
  entry->addInstruction(std::make_unique<zir::CallInst>(
      nullptr, callee, std::vector<std::shared_ptr<zir::Value>>{argument}));
  entry->addInstruction(std::make_unique<ReturnInst>());
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));

  auto verification = ZirVerifier().verify(module);
  return expect(hasError(verification, VerificationErrorCode::InvalidCall),
                "transfer of a borrowed call argument was not diagnosed");
}

bool testIndirectCallDerivesNoEscapeFromFunctionType() {
  Module module("indirect-call-noescape");
  auto stringType = zir::makeStringType();
  auto stringViewType = zir::makeStringViewType();
  module.addExternalFunction(std::make_unique<Function>("make", stringType));

  auto function =
      std::make_unique<Function>("valid", primitive(TypeKind::Void));
  auto owner = reg("owner", stringType);
  owner->setOwnership(ValueOwnership::OwnedStrong);
  auto view = reg("view", stringViewType);
  auto calleeType = std::make_shared<FunctionPointerType>(
      std::vector<std::shared_ptr<Type>>{stringViewType},
      primitive(TypeKind::Void),
      std::vector<zir::ParameterOwnership>{zir::ParameterOwnership::Borrow},
      std::vector<zir::ParameterEscape>{zir::ParameterEscape::NoEscape});
  auto callee = std::make_shared<FunctionReference>("consume", calleeType);

  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<zir::CallInst>(
      owner, "make", std::vector<std::shared_ptr<zir::Value>>{},
      std::vector<bool>{}, nullptr));
  entry->addInstruction(std::make_unique<BorrowInst>(view, owner));
  entry->addInstruction(std::make_unique<zir::CallInst>(
      nullptr, callee, std::vector<std::shared_ptr<zir::Value>>{view}));
  entry->addInstruction(std::make_unique<DestroyInst>(owner));
  entry->addInstruction(std::make_unique<ReturnInst>());
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));

  return expect(ZirVerifier().verify(module).ok(),
                "indirect call did not derive noescape from function type");
}

bool testCallConsumesExplicitManagedCopy() {
  Module module("call-explicit-copy");
  auto stringType = zir::makeStringType();

  auto callee =
      std::make_unique<Function>("consume", primitive(TypeKind::Void));
  auto parameter = std::make_shared<zir::Argument>(
      "value", stringType, false, false, nullptr,
      zir::ParameterOwnership::Transfer);
  parameter->setOwnership(ValueOwnership::OwnedStrong);
  callee->arguments.push_back(parameter);
  auto calleeEntry = std::make_unique<BasicBlock>("entry");
  calleeEntry->addInstruction(std::make_unique<DestroyInst>(parameter));
  calleeEntry->addInstruction(std::make_unique<ReturnInst>());
  callee->addBlock(std::move(calleeEntry));
  module.addFunction(std::move(callee));

  auto caller = std::make_unique<Function>("caller", primitive(TypeKind::Void));
  auto source = std::make_shared<zir::Argument>("source", stringType);
  caller->arguments.push_back(source);
  auto copied = reg("copied", stringType);
  copied->setOwnership(ValueOwnership::OwnedStrong);
  auto callerEntry = std::make_unique<BasicBlock>("entry");
  callerEntry->addInstruction(std::make_unique<CopyInst>(copied, source));
  auto call = std::make_unique<zir::CallInst>(
      nullptr, "consume", std::vector<std::shared_ptr<zir::Value>>{copied},
      std::vector<bool>{false}, nullptr);
  const auto callText = call->toString();
  callerEntry->addInstruction(std::move(call));
  callerEntry->addInstruction(std::make_unique<ReturnInst>());
  caller->addBlock(std::move(callerEntry));
  module.addFunction(std::move(caller));

  return expect(callText.find("transfer ") == std::string::npos,
                "call retained ownership metadata") &&
         expect(ZirVerifier().verify(module).ok(),
                "call rejected an explicit managed copy transfer") &&
         expect(ZirVerifier().verifyOwnershipObligations(module).ok(),
                "call did not consume its explicit managed copy");
}

bool testOwnershipTransferAcrossControlFlow() {
  Module module("ownership-control-flow");
  auto classType = std::make_shared<ClassType>("Node");
  auto boolean = primitive(TypeKind::Bool);
  auto function = std::make_unique<Function>("broken", classType);
  auto value = std::make_shared<zir::Argument>("value", classType);
  value->setOwnership(ValueOwnership::Owned);
  function->arguments.push_back(value);

  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<CondBranchInst>(
      std::make_shared<Constant>("true", boolean), "consume", "skip"));

  auto consume = std::make_unique<BasicBlock>("consume");
  auto slot = reg("slot", std::make_shared<PointerType>(classType));
  consume->addInstruction(std::make_unique<zir::AllocaInst>(slot, classType));
  consume->addInstruction(
      std::make_unique<StoreInst>(value, slot, StoreMode::Assign));
  consume->addInstruction(std::make_unique<BranchInst>("merge"));

  auto skip = std::make_unique<BasicBlock>("skip");
  skip->addInstruction(std::make_unique<BranchInst>("merge"));

  auto merge = std::make_unique<BasicBlock>("merge");
  merge->addInstruction(std::make_unique<ReturnInst>(value));

  function->addBlock(std::move(entry));
  function->addBlock(std::move(consume));
  function->addBlock(std::move(skip));
  function->addBlock(std::move(merge));
  module.addFunction(std::move(function));

  auto verification = ZirVerifier().verify(module);
  return expect(
      hasError(verification, VerificationErrorCode::OwnershipViolation),
      "double ownership transfer across control flow was not diagnosed");
}

bool testPhiTransfersOwnershipOnIncomingEdge() {
  Module module("phi-ownership-transfer");
  auto classType = std::make_shared<ClassType>("Node");
  auto boolean = primitive(TypeKind::Bool);
  auto function = std::make_unique<Function>("broken", classType);
  auto value = std::make_shared<zir::Argument>("value", classType);
  value->setOwnership(ValueOwnership::Owned);
  function->arguments.push_back(value);

  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<CondBranchInst>(
      std::make_shared<Constant>("true", boolean), "left", "right"));

  auto left = std::make_unique<BasicBlock>("left");
  auto slot = reg("slot", std::make_shared<PointerType>(classType));
  left->addInstruction(std::make_unique<zir::AllocaInst>(slot, classType));
  left->addInstruction(
      std::make_unique<StoreInst>(value, slot, StoreMode::Assign));
  left->addInstruction(std::make_unique<BranchInst>("merge"));

  auto right = std::make_unique<BasicBlock>("right");
  right->addInstruction(std::make_unique<BranchInst>("merge"));

  auto merge = std::make_unique<BasicBlock>("merge");
  auto result = reg("result", classType);
  result->setOwnership(ValueOwnership::Owned);
  merge->addInstruction(std::make_unique<PhiInst>(
      result, std::vector<std::pair<std::string, std::shared_ptr<zir::Value>>>{
                  {"left", value}, {"right", value}}));
  merge->addInstruction(std::make_unique<ReturnInst>(result));

  function->addBlock(std::move(entry));
  function->addBlock(std::move(left));
  function->addBlock(std::move(right));
  function->addBlock(std::move(merge));
  module.addFunction(std::move(function));

  auto verification = ZirVerifier().verify(module);
  return expect(
      hasError(verification, VerificationErrorCode::OwnershipViolation),
      "phi ownership transfer on its incoming edge was not diagnosed");
}

bool testPhiAllowsSeparateAlternativeOwnershipTransfers() {
  Module module("phi-alternative-ownership-transfer");
  auto classType = std::make_shared<ClassType>("Node");
  auto boolean = primitive(TypeKind::Bool);
  auto function = std::make_unique<Function>("valid", classType);
  auto value = std::make_shared<zir::Argument>("value", classType);
  value->setOwnership(ValueOwnership::Owned);
  function->arguments.push_back(value);

  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<CondBranchInst>(
      std::make_shared<Constant>("true", boolean), "left", "right"));
  auto left = std::make_unique<BasicBlock>("left");
  left->addInstruction(std::make_unique<BranchInst>("merge"));
  auto right = std::make_unique<BasicBlock>("right");
  right->addInstruction(std::make_unique<BranchInst>("merge"));

  auto merge = std::make_unique<BasicBlock>("merge");
  auto result = reg("result", classType);
  result->setOwnership(ValueOwnership::Owned);
  merge->addInstruction(std::make_unique<PhiInst>(
      result, std::vector<std::pair<std::string, std::shared_ptr<zir::Value>>>{
                  {"left", value}, {"right", value}}));
  merge->addInstruction(std::make_unique<ReturnInst>(result));

  function->addBlock(std::move(entry));
  function->addBlock(std::move(left));
  function->addBlock(std::move(right));
  function->addBlock(std::move(merge));
  module.addFunction(std::move(function));

  auto verification = ZirVerifier().verify(module);
  return expect(verification.ok(),
                "separate phi edge ownership transfers were rejected:\n" +
                    verification.format());
}

bool testOwnershipLivenessTracksPhiEdges() {
  Module module("ownership-liveness-phi");
  auto classType = std::make_shared<ClassType>("Node");
  auto boolean = primitive(TypeKind::Bool);
  auto function = std::make_unique<Function>("liveness", classType);
  auto value = std::make_shared<zir::Argument>("value", classType);
  value->setOwnership(ValueOwnership::Owned);
  function->arguments.push_back(value);

  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<CondBranchInst>(
      std::make_shared<Constant>("true", boolean), "left", "right"));
  auto left = std::make_unique<BasicBlock>("left");
  left->addInstruction(std::make_unique<BranchInst>("merge"));
  auto right = std::make_unique<BasicBlock>("right");
  right->addInstruction(std::make_unique<BranchInst>("merge"));
  auto merge = std::make_unique<BasicBlock>("merge");
  auto result = reg("result", classType);
  result->setOwnership(ValueOwnership::Owned);
  merge->addInstruction(std::make_unique<PhiInst>(
      result, std::vector<std::pair<std::string, std::shared_ptr<zir::Value>>>{
                  {"left", value}, {"right", value}}));
  merge->addInstruction(std::make_unique<ReturnInst>(result));

  auto *leftBlock = left.get();
  auto *rightBlock = right.get();
  auto *mergeBlock = merge.get();
  function->addBlock(std::move(entry));
  function->addBlock(std::move(left));
  function->addBlock(std::move(right));
  function->addBlock(std::move(merge));

  const auto liveness = zir::analyzeOwnershipLiveness(module, *function);
  return expect(liveness.isLiveOnEdge(*leftBlock, *mergeBlock, value) &&
                    liveness.isLiveOnEdge(*rightBlock, *mergeBlock, value),
                "owned phi inputs were not live on their incoming edges") &&
         expect(!liveness.isLiveAtBlockEntry(*mergeBlock, value) &&
                    liveness.isLiveAfter(*mergeBlock, 0, result),
                "phi liveness did not transfer ownership to its result");
}

bool testOwnershipFlowTracksEdgesMergesAndLoops() {
  auto classType = std::make_shared<ClassType>("Node");
  auto boolean = primitive(TypeKind::Bool);
  Module module("ownership-flow");

  auto branchFunction =
      std::make_unique<Function>("branch", primitive(TypeKind::Void));
  auto value = std::make_shared<zir::Argument>("value", classType);
  value->setOwnership(ValueOwnership::Owned);
  branchFunction->arguments.push_back(value);
  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<CondBranchInst>(
      std::make_shared<Constant>("true", boolean), "left", "right"));
  auto left = std::make_unique<BasicBlock>("left");
  left->addInstruction(std::make_unique<DestroyInst>(value));
  left->addInstruction(std::make_unique<BranchInst>("exit"));
  auto right = std::make_unique<BasicBlock>("right");
  right->addInstruction(std::make_unique<BranchInst>("exit"));
  auto exit = std::make_unique<BasicBlock>("exit");
  exit->addInstruction(std::make_unique<ReturnInst>());
  auto *entryBlock = entry.get();
  auto *leftBlock = left.get();
  auto *rightBlock = right.get();
  auto *exitBlock = exit.get();
  branchFunction->addBlock(std::move(entry));
  branchFunction->addBlock(std::move(left));
  branchFunction->addBlock(std::move(right));
  branchFunction->addBlock(std::move(exit));
  OwnershipFlowAnalysis::BlockEdges predecessors{
      {entryBlock, {}},
      {leftBlock, {entryBlock}},
      {rightBlock, {entryBlock}},
      {exitBlock, {leftBlock, rightBlock}}};
  OwnershipFlowAnalysis::BlockEdges successors{
      {entryBlock, {leftBlock, rightBlock}},
      {leftBlock, {exitBlock}},
      {rightBlock, {exitBlock}},
      {exitBlock, {}}};
  OwnershipFlowAnalysis branchAnalysis(
      module, *branchFunction, predecessors, successors,
      {entryBlock, leftBlock, rightBlock, exitBlock});
  const auto branchViolations = branchAnalysis.analyze();

  auto loopFunction =
      std::make_unique<Function>("loop", primitive(TypeKind::Void));
  auto loopEntry = std::make_unique<BasicBlock>("entry");
  auto node = reg("node", classType);
  node->setOwnership(ValueOwnership::Owned);
  loopEntry->addInstruction(std::make_unique<zir::AllocInst>(node, classType));
  loopEntry->addInstruction(std::make_unique<BranchInst>("loop"));
  auto loop = std::make_unique<BasicBlock>("loop");
  loop->addInstruction(std::make_unique<CondBranchInst>(
      std::make_shared<Constant>("true", boolean), "loop", "exit"));
  auto loopExit = std::make_unique<BasicBlock>("exit");
  loopExit->addInstruction(std::make_unique<ReturnInst>());
  auto *loopEntryBlock = loopEntry.get();
  auto *loopBlock = loop.get();
  auto *loopExitBlock = loopExit.get();
  loopFunction->addBlock(std::move(loopEntry));
  loopFunction->addBlock(std::move(loop));
  loopFunction->addBlock(std::move(loopExit));
  OwnershipFlowAnalysis::BlockEdges loopPredecessors{
      {loopEntryBlock, {}},
      {loopBlock, {loopEntryBlock, loopBlock}},
      {loopExitBlock, {loopBlock}}};
  OwnershipFlowAnalysis::BlockEdges loopSuccessors{
      {loopEntryBlock, {loopBlock}},
      {loopBlock, {loopBlock, loopExitBlock}},
      {loopExitBlock, {}}};
  OwnershipFlowAnalysis loopAnalysis(
      module, *loopFunction, loopPredecessors, loopSuccessors,
      {loopEntryBlock, loopBlock, loopExitBlock});
  const auto loopViolations = loopAnalysis.analyze();

  return expect(branchViolations.empty(),
                "ownership flow reported a false branch transfer violation") &&
         expect(
             branchAnalysis.stateOnEdge(*leftBlock, *exitBlock, value) ==
                     OwnershipFlowState::Destroyed &&
                 branchAnalysis.stateOnEdge(*rightBlock, *exitBlock, value) ==
                     OwnershipFlowState::Live,
             "ownership flow did not preserve per-edge branch states") &&
         expect(loopViolations.empty(),
                "ownership flow reported a false loop transfer violation") &&
         expect(loopAnalysis.stateOnEdge(*loopBlock, *loopBlock, node) ==
                        OwnershipFlowState::Live &&
                    loopAnalysis.stateOnEdge(*loopBlock, *loopExitBlock,
                                             node) == OwnershipFlowState::Live,
                "ownership flow did not reach a stable loop state");
}

bool testOwnershipFlowRejectsTransferAfterPartialDefinition() {
  Module module("ownership-partial-definition");
  auto classType = std::make_shared<ClassType>("Node");
  auto boolean = primitive(TypeKind::Bool);
  auto function =
      std::make_unique<Function>("broken", primitive(TypeKind::Void));

  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<CondBranchInst>(
      std::make_shared<Constant>("true", boolean), "left", "right"));
  auto left = std::make_unique<BasicBlock>("left");
  auto node = reg("node", classType);
  node->setOwnership(ValueOwnership::Owned);
  left->addInstruction(std::make_unique<zir::AllocInst>(node, classType));
  left->addInstruction(std::make_unique<BranchInst>("merge"));
  auto right = std::make_unique<BasicBlock>("right");
  right->addInstruction(std::make_unique<BranchInst>("merge"));
  auto merge = std::make_unique<BasicBlock>("merge");
  merge->addInstruction(std::make_unique<DestroyInst>(node));
  merge->addInstruction(std::make_unique<ReturnInst>());

  auto *entryBlock = entry.get();
  auto *leftBlock = left.get();
  auto *rightBlock = right.get();
  auto *mergeBlock = merge.get();
  function->addBlock(std::move(entry));
  function->addBlock(std::move(left));
  function->addBlock(std::move(right));
  function->addBlock(std::move(merge));

  OwnershipFlowAnalysis::BlockEdges predecessors{
      {entryBlock, {}},
      {leftBlock, {entryBlock}},
      {rightBlock, {entryBlock}},
      {mergeBlock, {leftBlock, rightBlock}}};
  OwnershipFlowAnalysis::BlockEdges successors{
      {entryBlock, {leftBlock, rightBlock}},
      {leftBlock, {mergeBlock}},
      {rightBlock, {mergeBlock}},
      {mergeBlock, {}}};
  OwnershipFlowAnalysis analysis(
      module, *function, predecessors, successors,
      {entryBlock, leftBlock, rightBlock, mergeBlock});
  const auto violations = analysis.analyze();
  const auto expectedState = static_cast<OwnershipFlowState>(
      static_cast<unsigned char>(OwnershipFlowState::Unavailable) |
      static_cast<unsigned char>(OwnershipFlowState::Live));

  return expect(
      violations.size() == 1 &&
          violations.front().priorState == expectedState &&
          zir::formatOwnershipFlowState(violations.front().priorState) ==
              "Unavailable|Live",
      "ownership flow did not report the exact partial-definition merge "
      "state");
}

bool testControlFlowGraphBuildsEdgesAndReachability() {
  auto boolean = primitive(TypeKind::Bool);
  auto function = std::make_unique<Function>("cfg", primitive(TypeKind::Void));
  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<CondBranchInst>(
      std::make_shared<Constant>("true", boolean), "left", "right"));
  auto left = std::make_unique<BasicBlock>("left");
  left->addInstruction(std::make_unique<BranchInst>("merge"));
  auto right = std::make_unique<BasicBlock>("right");
  right->addInstruction(std::make_unique<BranchInst>("merge"));
  auto merge = std::make_unique<BasicBlock>("merge");
  merge->addInstruction(std::make_unique<ReturnInst>());
  auto dead = std::make_unique<BasicBlock>("dead");
  dead->addInstruction(std::make_unique<ReturnInst>());
  auto *entryBlock = entry.get();
  auto *leftBlock = left.get();
  auto *rightBlock = right.get();
  auto *mergeBlock = merge.get();
  auto *deadBlock = dead.get();
  function->addBlock(std::move(entry));
  function->addBlock(std::move(left));
  function->addBlock(std::move(right));
  function->addBlock(std::move(merge));
  function->addBlock(std::move(dead));

  const ControlFlowGraph cfg(*function);
  const auto &mergePredecessors = cfg.predecessors().at(mergeBlock);
  return expect(
             cfg.findBlock("merge") == mergeBlock &&
                 cfg.successors().at(entryBlock).size() == 2 &&
                 std::find(mergePredecessors.begin(), mergePredecessors.end(),
                           leftBlock) != mergePredecessors.end() &&
                 std::find(mergePredecessors.begin(), mergePredecessors.end(),
                           rightBlock) != mergePredecessors.end(),
             "control-flow graph did not preserve branch edges") &&
         expect(cfg.reachable().count(mergeBlock) != 0 &&
                    cfg.reachable().count(deadBlock) == 0,
                "control-flow graph did not calculate reachability") &&
         expect(cfg.dominates(*entryBlock, *mergeBlock) &&
                    !cfg.dominates(*leftBlock, *mergeBlock),
                "control-flow graph did not calculate dominators");
}

bool testDestroyConsumesOwnedValue() {
  Module module("release-ownership");
  auto stringType = zir::makeStringType();
  auto function =
      std::make_unique<Function>("broken", primitive(TypeKind::Void));
  auto value = std::make_shared<zir::Argument>("value", stringType);
  value->setOwnership(ValueOwnership::Owned);
  function->arguments.push_back(value);

  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<DestroyInst>(value));
  entry->addInstruction(std::make_unique<DestroyInst>(value));
  entry->addInstruction(std::make_unique<ReturnInst>());
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));

  const auto verification = ZirVerifier().verify(module);
  return expect(
      hasError(verification, VerificationErrorCode::OwnershipViolation),
      "double release of an owned value was not diagnosed");
}

bool testUseAfterDestroyIsRejected() {
  Module module("use-after-release");
  auto classType = std::make_shared<ClassType>("Node");
  auto boolean = primitive(TypeKind::Bool);
  auto function =
      std::make_unique<Function>("broken", primitive(TypeKind::Void));
  auto value = std::make_shared<zir::Argument>("value", classType);
  value->setOwnership(ValueOwnership::Owned);
  function->arguments.push_back(value);

  auto entry = std::make_unique<BasicBlock>("entry");
  auto comparison = reg("comparison", boolean);
  entry->addInstruction(std::make_unique<DestroyInst>(value));
  entry->addInstruction(std::make_unique<CmpInst>(
      "eq", comparison, value, std::make_shared<Constant>("null", classType)));
  entry->addInstruction(std::make_unique<ReturnInst>());
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));

  return expect(hasError(ZirVerifier().verify(module),
                         VerificationErrorCode::OwnershipViolation),
                "use of an owned value after release was not diagnosed");
}

bool testUseAfterMoveIsRejected() {
  Module module("use-after-move");
  auto classType = std::make_shared<ClassType>("Node");
  auto function = std::make_unique<Function>("broken", classType);
  auto entry = std::make_unique<BasicBlock>("entry");
  auto value = reg("value", classType);
  value->setOwnership(ValueOwnership::Owned);
  auto moved = reg("moved", classType);
  moved->setOwnership(ValueOwnership::Owned);
  entry->addInstruction(std::make_unique<zir::AllocInst>(value, classType));
  entry->addInstruction(std::make_unique<MoveInst>(moved, value));
  entry->addInstruction(std::make_unique<DestroyInst>(value));
  entry->addInstruction(std::make_unique<ReturnInst>(moved));
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));

  const auto verification = ZirVerifier().verify(module);
  return expect(
      hasError(verification, VerificationErrorCode::OwnershipViolation) &&
          verification.format().find("state: Moved") != std::string::npos,
      "use of an owned value after explicit move did not report Moved state");
}

bool testCopyCreatesIndependentOwnership() {
  Module module("copy-independent-ownership");
  auto classType = std::make_shared<ClassType>("Node");
  auto function =
      std::make_unique<Function>("valid", primitive(TypeKind::Void));
  auto source = std::make_shared<zir::Argument>("source", classType);
  source->setOwnership(ValueOwnership::Owned);
  function->arguments.push_back(source);
  auto copied = reg("copied", classType);
  copied->setOwnership(ValueOwnership::Owned);

  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<CopyInst>(copied, source));
  entry->addInstruction(std::make_unique<DestroyInst>(source));
  entry->addInstruction(std::make_unique<DestroyInst>(copied));
  entry->addInstruction(std::make_unique<ReturnInst>());
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));

  const auto &copy = module.getFunctions()
                         .front()
                         ->getBlocks()
                         .front()
                         ->getInstructions()
                         .front();
  return expect(copy->toString().rfind("%copied = copy ", 0) == 0,
                "copy instruction did not render its owned result") &&
         expect(ZirVerifier().verify(module).ok(),
                "copy with independently destroyed ownership was rejected") &&
         expect(ZirVerifier().verifyOwnershipObligations(module).ok(),
                "copy left an ownership obligation after both destroys");
}

bool testMoveTransfersOwnershipIntoInitialization() {
  Module module("move-initialization-ownership");
  auto classType = std::make_shared<ClassType>("Node");
  auto function =
      std::make_unique<Function>("valid", primitive(TypeKind::Void));
  auto slot = reg("slot", std::make_shared<PointerType>(classType));
  auto value = reg("value", classType);
  value->setOwnership(ValueOwnership::OwnedStrong);
  auto moved = reg("moved", classType);
  moved->setOwnership(ValueOwnership::OwnedStrong);

  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<zir::AllocaInst>(slot, classType));
  entry->addInstruction(std::make_unique<zir::AllocInst>(value, classType));
  entry->addInstruction(std::make_unique<MoveInst>(moved, value));
  entry->addInstruction(
      std::make_unique<StoreInst>(moved, slot, StoreMode::Initialize));
  entry->addInstruction(std::make_unique<ReturnInst>());
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));

  return expect(ZirVerifier().verify(module).ok(),
                "move into managed initialization was rejected") &&
         expect(ZirVerifier().verifyOwnershipObligations(module).ok(),
                "managed initialization did not consume the moved value");
}

bool testWeakCopyTransfersWeakOwnership() {
  Module module("weak-copy-ownership");
  auto weakType = std::make_shared<ClassType>("Node");
  weakType->setWeak(true);
  auto function =
      std::make_unique<Function>("valid", primitive(TypeKind::Void));
  auto source = std::make_shared<zir::Argument>("source", weakType);
  function->arguments.push_back(source);

  auto copied = reg("copied", weakType);
  copied->setOwnership(ValueOwnership::OwnedWeak);
  auto slot = reg("slot", std::make_shared<PointerType>(weakType));
  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<zir::AllocaInst>(slot, weakType));
  entry->addInstruction(std::make_unique<CopyInst>(copied, source));
  entry->addInstruction(
      std::make_unique<StoreInst>(copied, slot, StoreMode::Initialize));
  entry->addInstruction(std::make_unique<ReturnInst>());
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));

  return expect(ZirVerifier().verify(module).ok(),
                "weak copy with an owned weak result was rejected") &&
         expect(ZirVerifier().verifyOwnershipObligations(module).ok(),
                "weak store did not consume the owned weak token");
}

bool testWeakCopyRejectsStrongOwnership() {
  Module module("weak-copy-strong-ownership");
  auto weakType = std::make_shared<ClassType>("Node");
  weakType->setWeak(true);
  auto function =
      std::make_unique<Function>("broken", primitive(TypeKind::Void));
  auto source = std::make_shared<zir::Argument>("source", weakType);
  function->arguments.push_back(source);

  auto copied = reg("copied", weakType);
  copied->setOwnership(ValueOwnership::OwnedStrong);
  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<CopyInst>(copied, source));
  entry->addInstruction(std::make_unique<DestroyInst>(copied));
  entry->addInstruction(std::make_unique<ReturnInst>());
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));

  return expect(hasError(ZirVerifier().verify(module),
                         VerificationErrorCode::InvalidOperand),
                "weak copy accepted an owned strong result");
}

bool testOwnershipExitObligationsReportLiveValues() {
  Module module("ownership-exit-obligations");
  auto classType = std::make_shared<ClassType>("Node");
  auto function =
      std::make_unique<Function>("valid", primitive(TypeKind::Void));
  auto entry = std::make_unique<BasicBlock>("entry");
  auto value = reg("value", classType);
  value->setOwnership(ValueOwnership::Owned);
  entry->addInstruction(std::make_unique<zir::AllocInst>(value, classType));
  entry->addInstruction(std::make_unique<ReturnInst>());
  auto *entryBlock = entry.get();
  function->addBlock(std::move(entry));

  OwnershipFlowAnalysis::BlockEdges predecessors{{entryBlock, {}}};
  OwnershipFlowAnalysis::BlockEdges successors{{entryBlock, {}}};
  OwnershipFlowAnalysis analysis(module, *function, predecessors, successors,
                                 {entryBlock});
  const auto obligations = analysis.analyzeExitObligations();
  return expect(obligations.size() == 1 &&
                    obligations.front().block == entryBlock &&
                    obligations.front().instructionIndex == 1 &&
                    obligations.front().value == value.get() &&
                    obligations.front().state == OwnershipFlowState::Live,
                "exit obligations did not report a live owned value");
}

bool testOwnershipClosurePlanConnectsDefinitionAndExit() {
  Module module("ownership-closure-plan");
  auto classType = std::make_shared<ClassType>("Node");
  auto function =
      std::make_unique<Function>("valid", primitive(TypeKind::Void));
  auto entry = std::make_unique<BasicBlock>("entry");
  auto value = reg("value", classType);
  value->setOwnership(ValueOwnership::Owned);
  entry->addInstruction(std::make_unique<zir::AllocInst>(value, classType));
  entry->addInstruction(std::make_unique<ReturnInst>());
  auto *entryBlock = entry.get();
  function->addBlock(std::move(entry));

  OwnershipFlowAnalysis::BlockEdges predecessors{{entryBlock, {}}};
  OwnershipFlowAnalysis::BlockEdges successors{{entryBlock, {}}};
  OwnershipFlowAnalysis analysis(module, *function, predecessors, successors,
                                 {entryBlock});
  const auto plans = analysis.analyzeOwnershipClosurePlans();
  return expect(
      plans.size() == 1 && plans.front().value == value.get() &&
          plans.front().definition.block == entryBlock &&
          plans.front().definition.instructionIndex == 0 &&
          plans.front().liveExits.size() == 1 &&
          plans.front().liveExits.front().block == entryBlock &&
          plans.front().liveExits.front().instructionIndex == 1 &&
          plans.front().destroyPlacements.size() == 1 &&
          plans.front().destroyPlacements.front().kind ==
              OwnershipDestroyPlacementKind::BeforeReturn &&
          plans.front().destroyPlacements.front().destination == entryBlock &&
          plans.front().destroyPlacements.front().instructionIndex == 1,
      "closure plan did not connect an owned definition to its "
      "live exit");
}

bool testOwnershipClosurePlanUsesCriticalLiveEdge() {
  Module module("ownership-closure-critical-edge");
  auto classType = std::make_shared<ClassType>("Node");
  auto boolean = primitive(TypeKind::Bool);
  auto function =
      std::make_unique<Function>("valid", primitive(TypeKind::Void));
  auto value = std::make_shared<zir::Argument>("value", classType);
  value->setOwnership(ValueOwnership::Owned);
  function->arguments.push_back(value);

  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<CondBranchInst>(
      std::make_shared<Constant>("true", boolean), "exit", "closed"));
  auto closed = std::make_unique<BasicBlock>("closed");
  closed->addInstruction(std::make_unique<DestroyInst>(value));
  closed->addInstruction(std::make_unique<BranchInst>("exit"));
  auto exit = std::make_unique<BasicBlock>("exit");
  exit->addInstruction(std::make_unique<ReturnInst>());
  auto *entryBlock = entry.get();
  auto *closedBlock = closed.get();
  auto *exitBlock = exit.get();
  function->addBlock(std::move(entry));
  function->addBlock(std::move(closed));
  function->addBlock(std::move(exit));

  OwnershipFlowAnalysis::BlockEdges predecessors{
      {entryBlock, {}},
      {closedBlock, {entryBlock}},
      {exitBlock, {entryBlock, closedBlock}}};
  OwnershipFlowAnalysis::BlockEdges successors{
      {entryBlock, {exitBlock, closedBlock}},
      {closedBlock, {exitBlock}},
      {exitBlock, {}}};
  OwnershipFlowAnalysis analysis(module, *function, predecessors, successors,
                                 {entryBlock, closedBlock, exitBlock});
  const auto plans = analysis.analyzeOwnershipClosurePlans();
  return expect(
      plans.size() == 1 && plans.front().destroyPlacements.size() == 1 &&
          plans.front().destroyPlacements.front().kind ==
              OwnershipDestroyPlacementKind::OnEdge &&
          plans.front().destroyPlacements.front().source == entryBlock &&
          plans.front().destroyPlacements.front().destination == exitBlock &&
          plans.front().destroyPlacements.front().requiresEdgeSplit,
      "closure plan did not isolate the live critical edge");
}

bool testOwnershipExitObligationsAllowMovedReturn() {
  Module module("ownership-exit-moved-return");
  auto classType = std::make_shared<ClassType>("Node");
  auto function = std::make_unique<Function>("valid", classType);
  auto entry = std::make_unique<BasicBlock>("entry");
  auto value = reg("value", classType);
  value->setOwnership(ValueOwnership::Owned);
  auto moved = reg("moved", classType);
  moved->setOwnership(ValueOwnership::Owned);
  entry->addInstruction(std::make_unique<zir::AllocInst>(value, classType));
  entry->addInstruction(std::make_unique<MoveInst>(moved, value));
  entry->addInstruction(std::make_unique<ReturnInst>(moved));
  auto *entryBlock = entry.get();
  function->addBlock(std::move(entry));

  OwnershipFlowAnalysis::BlockEdges predecessors{{entryBlock, {}}};
  OwnershipFlowAnalysis::BlockEdges successors{{entryBlock, {}}};
  OwnershipFlowAnalysis analysis(module, *function, predecessors, successors,
                                 {entryBlock});
  const auto obligations = analysis.analyzeExitObligations();
  module.addFunction(std::move(function));
  return expect(obligations.empty(),
                "explicitly moved return produced an ownership obligation") &&
         expect(ZirVerifier().verify(module).ok(),
                "explicit move return was rejected by the verifier");
}

bool testOwnershipObligationVerifierReportsLiveValues() {
  Module module("ownership-obligation-verifier");
  auto classType = std::make_shared<ClassType>("Node");
  auto function =
      std::make_unique<Function>("broken", primitive(TypeKind::Void));
  auto entry = std::make_unique<BasicBlock>("entry");
  auto value = reg("value", classType);
  value->setOwnership(ValueOwnership::Owned);
  entry->addInstruction(std::make_unique<zir::AllocInst>(value, classType));
  entry->addInstruction(std::make_unique<ReturnInst>());
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));

  const auto verification = ZirVerifier().verifyOwnershipObligations(module);
  return expect(
      hasError(verification, VerificationErrorCode::OwnershipViolation) &&
          verification.format().find("state: Live") != std::string::npos,
      "ownership obligation verifier did not report the live exit state");
}

bool testOwnershipExitObligationsTrackPartialDefinitions() {
  Module module("ownership-exit-partial-definition");
  auto classType = std::make_shared<ClassType>("Node");
  auto boolean = primitive(TypeKind::Bool);
  auto function =
      std::make_unique<Function>("valid", primitive(TypeKind::Void));
  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<CondBranchInst>(
      std::make_shared<Constant>("true", boolean), "defines", "skips"));
  auto defines = std::make_unique<BasicBlock>("defines");
  auto value = reg("value", classType);
  value->setOwnership(ValueOwnership::Owned);
  defines->addInstruction(std::make_unique<zir::AllocInst>(value, classType));
  defines->addInstruction(std::make_unique<BranchInst>("exit"));
  auto skips = std::make_unique<BasicBlock>("skips");
  skips->addInstruction(std::make_unique<BranchInst>("exit"));
  auto exit = std::make_unique<BasicBlock>("exit");
  exit->addInstruction(std::make_unique<ReturnInst>());
  auto *entryBlock = entry.get();
  auto *definesBlock = defines.get();
  auto *skipsBlock = skips.get();
  auto *exitBlock = exit.get();
  function->addBlock(std::move(entry));
  function->addBlock(std::move(defines));
  function->addBlock(std::move(skips));
  function->addBlock(std::move(exit));

  OwnershipFlowAnalysis::BlockEdges predecessors{
      {entryBlock, {}},
      {definesBlock, {entryBlock}},
      {skipsBlock, {entryBlock}},
      {exitBlock, {definesBlock, skipsBlock}}};
  OwnershipFlowAnalysis::BlockEdges successors{
      {entryBlock, {definesBlock, skipsBlock}},
      {definesBlock, {exitBlock}},
      {skipsBlock, {exitBlock}},
      {exitBlock, {}}};
  OwnershipFlowAnalysis analysis(
      module, *function, predecessors, successors,
      {entryBlock, definesBlock, skipsBlock, exitBlock});
  const auto obligations = analysis.analyzeExitObligations();
  const auto expectedState = static_cast<OwnershipFlowState>(
      static_cast<unsigned char>(OwnershipFlowState::Unavailable) |
      static_cast<unsigned char>(OwnershipFlowState::Live));
  const auto plans = analysis.analyzeOwnershipClosurePlans();
  return expect(
      obligations.size() == 1 && obligations.front().value == value.get() &&
          obligations.front().state == expectedState && plans.size() == 1 &&
          plans.front().destroyPlacements.size() == 1 &&
          plans.front().destroyPlacements.front().kind ==
              OwnershipDestroyPlacementKind::OnEdge &&
          plans.front().destroyPlacements.front().source == definesBlock &&
          plans.front().destroyPlacements.front().destination == exitBlock &&
          !plans.front().destroyPlacements.front().requiresEdgeSplit,
      "partial definition did not preserve its live exit obligation or "
      "identify its live edge");
}

bool testOwnershipExitObligationsAllowPhiTransfer() {
  Module module("ownership-exit-phi-transfer");
  auto classType = std::make_shared<ClassType>("Node");
  auto boolean = primitive(TypeKind::Bool);
  auto function = std::make_unique<Function>("valid", classType);
  auto value = std::make_shared<zir::Argument>("value", classType);
  value->setOwnership(ValueOwnership::Owned);
  function->arguments.push_back(value);
  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<CondBranchInst>(
      std::make_shared<Constant>("true", boolean), "left", "right"));
  auto left = std::make_unique<BasicBlock>("left");
  left->addInstruction(std::make_unique<BranchInst>("merge"));
  auto right = std::make_unique<BasicBlock>("right");
  right->addInstruction(std::make_unique<BranchInst>("merge"));
  auto merge = std::make_unique<BasicBlock>("merge");
  auto result = reg("result", classType);
  result->setOwnership(ValueOwnership::Owned);
  merge->addInstruction(std::make_unique<PhiInst>(
      result, std::vector<std::pair<std::string, std::shared_ptr<zir::Value>>>{
                  {"left", value}, {"right", value}}));
  merge->addInstruction(std::make_unique<ReturnInst>(result));
  auto *entryBlock = entry.get();
  auto *leftBlock = left.get();
  auto *rightBlock = right.get();
  auto *mergeBlock = merge.get();
  function->addBlock(std::move(entry));
  function->addBlock(std::move(left));
  function->addBlock(std::move(right));
  function->addBlock(std::move(merge));

  OwnershipFlowAnalysis::BlockEdges predecessors{
      {entryBlock, {}},
      {leftBlock, {entryBlock}},
      {rightBlock, {entryBlock}},
      {mergeBlock, {leftBlock, rightBlock}}};
  OwnershipFlowAnalysis::BlockEdges successors{
      {entryBlock, {leftBlock, rightBlock}},
      {leftBlock, {mergeBlock}},
      {rightBlock, {mergeBlock}},
      {mergeBlock, {}}};
  OwnershipFlowAnalysis analysis(
      module, *function, predecessors, successors,
      {entryBlock, leftBlock, rightBlock, mergeBlock});
  return expect(analysis.analyzeExitObligations().empty(),
                "phi transfer produced an unclosed ownership obligation");
}

bool testOwnershipExitObligationsTrackLoops() {
  Module module("ownership-exit-loop");
  auto classType = std::make_shared<ClassType>("Node");
  auto boolean = primitive(TypeKind::Bool);
  auto function =
      std::make_unique<Function>("valid", primitive(TypeKind::Void));
  auto entry = std::make_unique<BasicBlock>("entry");
  auto value = reg("value", classType);
  value->setOwnership(ValueOwnership::Owned);
  entry->addInstruction(std::make_unique<zir::AllocInst>(value, classType));
  entry->addInstruction(std::make_unique<BranchInst>("loop"));
  auto loop = std::make_unique<BasicBlock>("loop");
  loop->addInstruction(std::make_unique<CondBranchInst>(
      std::make_shared<Constant>("true", boolean), "loop", "exit"));
  auto exit = std::make_unique<BasicBlock>("exit");
  exit->addInstruction(std::make_unique<ReturnInst>());
  auto *entryBlock = entry.get();
  auto *loopBlock = loop.get();
  auto *exitBlock = exit.get();
  function->addBlock(std::move(entry));
  function->addBlock(std::move(loop));
  function->addBlock(std::move(exit));

  OwnershipFlowAnalysis::BlockEdges predecessors{
      {entryBlock, {}},
      {loopBlock, {entryBlock, loopBlock}},
      {exitBlock, {loopBlock}}};
  OwnershipFlowAnalysis::BlockEdges successors{
      {entryBlock, {loopBlock}},
      {loopBlock, {loopBlock, exitBlock}},
      {exitBlock, {}}};
  OwnershipFlowAnalysis analysis(module, *function, predecessors, successors,
                                 {entryBlock, loopBlock, exitBlock});
  const auto obligations = analysis.analyzeExitObligations();
  const auto plans = analysis.analyzeOwnershipClosurePlans();
  return expect(
      obligations.size() == 1 && obligations.front().value == value.get() &&
          obligations.front().state == OwnershipFlowState::Live &&
          plans.size() == 1 && plans.front().destroyPlacements.size() == 1 &&
          plans.front().destroyPlacements.front().kind ==
              OwnershipDestroyPlacementKind::BeforeReturn &&
          plans.front().destroyPlacements.front().destination == exitBlock,
      "loop exit did not preserve its live obligation or identify its "
      "destroy placement");
}

bool testBorrowPreservesOwnedString() {
  Module module("borrow-ownership");
  auto stringType = zir::makeStringType();
  auto stringViewType = zir::makeStringViewType();
  auto function =
      std::make_unique<Function>("valid", primitive(TypeKind::Void));
  auto value = std::make_shared<zir::Argument>("value", stringType);
  value->setOwnership(ValueOwnership::Owned);
  function->arguments.push_back(value);

  auto entry = std::make_unique<BasicBlock>("entry");
  auto view = reg("view", stringViewType);
  entry->addInstruction(std::make_unique<BorrowInst>(view, value));
  entry->addInstruction(std::make_unique<DestroyInst>(value));
  entry->addInstruction(std::make_unique<ReturnInst>());
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));

  return expect(ZirVerifier().verify(module).ok(),
                "borrow incorrectly consumed its String owner");
}

bool testBorrowRequiresStringOwnerAndBorrowedStringView() {
  Module module("invalid-borrow");
  auto stringViewType = zir::makeStringViewType();
  auto function =
      std::make_unique<Function>("broken", primitive(TypeKind::Void));
  auto owner = std::make_shared<zir::Argument>("owner", stringViewType);
  function->arguments.push_back(owner);

  auto entry = std::make_unique<BasicBlock>("entry");
  auto result = reg("result", stringViewType);
  result->setOwnership(ValueOwnership::Owned);
  entry->addInstruction(std::make_unique<BorrowInst>(result, owner));
  entry->addInstruction(std::make_unique<ReturnInst>());
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));

  return expect(
      hasError(ZirVerifier().verify(module),
               VerificationErrorCode::InvalidOperand),
      "borrow accepted a non-String owner or an owned StringView result");
}

bool testDestroyRejectsLiveBorrowedStringView() {
  Module module("borrow-release-lifetime");
  auto stringType = zir::makeStringType();
  auto stringViewType = zir::makeStringViewType();
  auto consume =
      std::make_unique<Function>("consume", primitive(TypeKind::Void));
  consume->arguments.push_back(std::make_shared<zir::Argument>(
      "view", stringViewType, false, false, nullptr,
      zir::ParameterOwnership::Borrow, zir::ParameterEscape::NoEscape));
  module.addExternalFunction(std::move(consume));

  auto function =
      std::make_unique<Function>("broken", primitive(TypeKind::Void));
  auto text = std::make_shared<zir::Argument>("text", stringType);
  text->setOwnership(ValueOwnership::Owned);
  function->arguments.push_back(text);
  auto entry = std::make_unique<BasicBlock>("entry");
  auto view = reg("view", stringViewType);
  entry->addInstruction(std::make_unique<BorrowInst>(view, text));
  entry->addInstruction(std::make_unique<DestroyInst>(text));
  entry->addInstruction(std::make_unique<zir::CallInst>(
      nullptr, "consume", std::vector<std::shared_ptr<zir::Value>>{view},
      std::vector<bool>{false}, nullptr));
  entry->addInstruction(std::make_unique<ReturnInst>());
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));

  return expect(hasError(ZirVerifier().verify(module),
                         VerificationErrorCode::OwnershipViolation),
                "release before a borrowed StringView use was not diagnosed");
}

bool testOwnershipLivenessTracksBorrowPhiEdges() {
  Module module("borrow-phi-liveness");
  auto stringType = zir::makeStringType();
  auto stringViewType = zir::makeStringViewType();
  auto boolean = primitive(TypeKind::Bool);
  auto make = std::make_unique<Function>("make", stringType);
  module.addExternalFunction(std::move(make));
  auto consume =
      std::make_unique<Function>("consume", primitive(TypeKind::Void));
  consume->arguments.push_back(
      std::make_shared<zir::Argument>("view", stringViewType));
  module.addExternalFunction(std::move(consume));

  auto function =
      std::make_unique<Function>("valid", primitive(TypeKind::Void));
  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<CondBranchInst>(
      std::make_shared<Constant>("true", boolean), "left", "right"));

  auto left = std::make_unique<BasicBlock>("left");
  auto leftText = reg("left.text", stringType);
  leftText->setOwnership(ValueOwnership::Owned);
  auto leftView = reg("left.view", stringViewType);
  left->addInstruction(std::make_unique<zir::CallInst>(
      leftText, "make", std::vector<std::shared_ptr<zir::Value>>{},
      std::vector<bool>{}, nullptr));
  left->addInstruction(std::make_unique<BorrowInst>(leftView, leftText));
  left->addInstruction(std::make_unique<BranchInst>("merge"));

  auto right = std::make_unique<BasicBlock>("right");
  auto rightText = reg("right.text", stringType);
  rightText->setOwnership(ValueOwnership::Owned);
  auto rightView = reg("right.view", stringViewType);
  right->addInstruction(std::make_unique<zir::CallInst>(
      rightText, "make", std::vector<std::shared_ptr<zir::Value>>{},
      std::vector<bool>{}, nullptr));
  right->addInstruction(std::make_unique<BorrowInst>(rightView, rightText));
  right->addInstruction(std::make_unique<BranchInst>("merge"));

  auto merge = std::make_unique<BasicBlock>("merge");
  auto mergedView = reg("merged.view", stringViewType);
  merge->addInstruction(std::make_unique<PhiInst>(
      mergedView,
      std::vector<std::pair<std::string, std::shared_ptr<zir::Value>>>{
          {"left", leftView}, {"right", rightView}}));
  merge->addInstruction(std::make_unique<zir::CallInst>(
      nullptr, "consume",
      std::vector<std::shared_ptr<zir::Value>>{mergedView}));
  merge->addInstruction(std::make_unique<ReturnInst>());

  auto *leftBlock = left.get();
  auto *rightBlock = right.get();
  function->addBlock(std::move(entry));
  function->addBlock(std::move(left));
  function->addBlock(std::move(right));
  function->addBlock(std::move(merge));
  module.addFunction(std::move(function));

  const auto liveness =
      zir::analyzeOwnershipLiveness(module, *module.getFunctions().front());
  return expect(
      liveness.isLiveOnEdge(*leftBlock,
                            *module.getFunctions().front()->findBlock("merge"),
                            leftText) &&
          !liveness.isLiveOnEdge(
              *leftBlock, *module.getFunctions().front()->findBlock("merge"),
              rightText) &&
          liveness.isLiveOnEdge(
              *rightBlock, *module.getFunctions().front()->findBlock("merge"),
              rightText) &&
          !liveness.isLiveOnEdge(
              *rightBlock, *module.getFunctions().front()->findBlock("merge"),
              leftText),
      "borrow provenance did not stay on its incoming phi edge");
}

bool testOwnershipLivenessTracksBorrowedViewsThroughLocalStorage() {
  Module module("borrow-storage-liveness");
  auto stringType = zir::makeStringType();
  auto stringViewType = zir::makeStringViewType();
  auto stringViewPointer = std::make_shared<PointerType>(stringViewType);
  auto boolean = primitive(TypeKind::Bool);
  auto make = std::make_unique<Function>("make", stringType);
  module.addExternalFunction(std::move(make));
  auto consume =
      std::make_unique<Function>("consume", primitive(TypeKind::Void));
  consume->arguments.push_back(
      std::make_shared<zir::Argument>("view", stringViewType));
  module.addExternalFunction(std::move(consume));

  auto function =
      std::make_unique<Function>("valid", primitive(TypeKind::Void));
  auto entry = std::make_unique<BasicBlock>("entry");
  auto slot = reg("slot", stringViewPointer);
  entry->addInstruction(
      std::make_unique<zir::AllocaInst>(slot, stringViewType));
  entry->addInstruction(std::make_unique<CondBranchInst>(
      std::make_shared<Constant>("true", boolean), "left", "right"));

  auto left = std::make_unique<BasicBlock>("left");
  auto leftText = reg("left.text", stringType);
  leftText->setOwnership(ValueOwnership::Owned);
  auto leftView = reg("left.view", stringViewType);
  left->addInstruction(std::make_unique<zir::CallInst>(
      leftText, "make", std::vector<std::shared_ptr<zir::Value>>{},
      std::vector<bool>{}, nullptr));
  left->addInstruction(std::make_unique<BorrowInst>(leftView, leftText));
  left->addInstruction(
      std::make_unique<StoreInst>(leftView, slot, StoreMode::Initialize));
  left->addInstruction(std::make_unique<BranchInst>("merge"));

  auto right = std::make_unique<BasicBlock>("right");
  auto rightText = reg("right.text", stringType);
  rightText->setOwnership(ValueOwnership::Owned);
  auto rightView = reg("right.view", stringViewType);
  right->addInstruction(std::make_unique<zir::CallInst>(
      rightText, "make", std::vector<std::shared_ptr<zir::Value>>{},
      std::vector<bool>{}, nullptr));
  right->addInstruction(std::make_unique<BorrowInst>(rightView, rightText));
  right->addInstruction(
      std::make_unique<StoreInst>(rightView, slot, StoreMode::Assign));
  right->addInstruction(std::make_unique<BranchInst>("merge"));

  auto merge = std::make_unique<BasicBlock>("merge");
  auto loaded = reg("loaded", stringViewType);
  merge->addInstruction(std::make_unique<LoadInst>(loaded, slot));
  merge->addInstruction(std::make_unique<zir::CallInst>(
      nullptr, "consume", std::vector<std::shared_ptr<zir::Value>>{loaded}));
  merge->addInstruction(std::make_unique<ReturnInst>());

  auto *leftBlock = left.get();
  auto *rightBlock = right.get();
  function->addBlock(std::move(entry));
  function->addBlock(std::move(left));
  function->addBlock(std::move(right));
  function->addBlock(std::move(merge));
  module.addFunction(std::move(function));

  const auto *mergeBlock = module.getFunctions().front()->findBlock("merge");
  const auto liveness =
      zir::analyzeOwnershipLiveness(module, *module.getFunctions().front());
  return expect(
      liveness.isLiveOnEdge(*leftBlock, *mergeBlock, leftText) &&
          !liveness.isLiveOnEdge(*leftBlock, *mergeBlock, rightText) &&
          liveness.isLiveOnEdge(*rightBlock, *mergeBlock, rightText) &&
          !liveness.isLiveOnEdge(*rightBlock, *mergeBlock, leftText),
      "borrow provenance loaded from local storage did not stay on its "
      "incoming edge");
}

bool testReturnRejectsFunctionLocalStringView() {
  Module module("local-string-view-return");
  auto stringType = zir::makeStringType();
  auto stringViewType = zir::makeStringViewType();
  auto make = std::make_unique<Function>("make", stringType);
  module.addExternalFunction(std::move(make));

  auto function = std::make_unique<Function>("broken", stringViewType);
  auto entry = std::make_unique<BasicBlock>("entry");
  auto text = reg("text", stringType);
  text->setOwnership(ValueOwnership::Owned);
  auto view = reg("view", stringViewType);
  entry->addInstruction(std::make_unique<zir::CallInst>(
      text, "make", std::vector<std::shared_ptr<zir::Value>>{},
      std::vector<bool>{}, nullptr));
  entry->addInstruction(std::make_unique<BorrowInst>(view, text));
  entry->addInstruction(std::make_unique<ReturnInst>(view));
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));

  const auto verification = ZirVerifier().verify(module);
  return expect(hasError(verification, VerificationErrorCode::InvalidReturn),
                "returning a function-local StringView was not diagnosed");
}

bool testReturnAllowsBorrowedStringView() {
  Module module("borrowed-string-view-return");
  auto stringType = zir::makeStringType();
  auto stringViewType = zir::makeStringViewType();
  auto function = std::make_unique<Function>("valid", stringViewType);
  auto text = std::make_shared<zir::Argument>("text", stringType);
  auto view = reg("view", stringViewType);
  function->arguments.push_back(text);

  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<BorrowInst>(view, text));
  entry->addInstruction(std::make_unique<ReturnInst>(view));
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));

  return expect(ZirVerifier().verify(module).ok(),
                "returning a caller-borrowed StringView was rejected");
}

bool testReturnRejectsStringViewLoadedFromLocalStorage() {
  Module module("local-string-view-load-return");
  auto stringType = zir::makeStringType();
  auto stringViewType = zir::makeStringViewType();
  auto stringViewPointer = std::make_shared<PointerType>(stringViewType);
  auto make = std::make_unique<Function>("make", stringType);
  module.addExternalFunction(std::move(make));

  auto function = std::make_unique<Function>("broken", stringViewType);
  auto entry = std::make_unique<BasicBlock>("entry");
  auto text = reg("text", stringType);
  text->setOwnership(ValueOwnership::Owned);
  auto view = reg("view", stringViewType);
  auto slot = reg("slot", stringViewPointer);
  auto loaded = reg("loaded", stringViewType);
  entry->addInstruction(std::make_unique<zir::CallInst>(
      text, "make", std::vector<std::shared_ptr<zir::Value>>{},
      std::vector<bool>{}, nullptr));
  entry->addInstruction(std::make_unique<BorrowInst>(view, text));
  entry->addInstruction(
      std::make_unique<zir::AllocaInst>(slot, stringViewType));
  entry->addInstruction(
      std::make_unique<StoreInst>(view, slot, StoreMode::Initialize));
  entry->addInstruction(std::make_unique<LoadInst>(loaded, slot));
  entry->addInstruction(std::make_unique<ReturnInst>(loaded));
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));

  const auto verification = ZirVerifier().verify(module);
  return expect(hasError(verification, VerificationErrorCode::InvalidReturn),
                "returning a local StringView loaded from storage was not "
                "diagnosed");
}

bool testReturnRejectsCastFunctionLocalStringView() {
  Module module("local-string-view-cast-return");
  auto stringType = zir::makeStringType();
  auto stringViewType = zir::makeStringViewType();
  auto make = std::make_unique<Function>("make", stringType);
  module.addExternalFunction(std::move(make));

  auto function = std::make_unique<Function>("broken", stringViewType);
  auto entry = std::make_unique<BasicBlock>("entry");
  auto text = reg("text", stringType);
  text->setOwnership(ValueOwnership::Owned);
  auto view = reg("view", stringViewType);
  auto castView = reg("cast.view", stringViewType);
  entry->addInstruction(std::make_unique<zir::CallInst>(
      text, "make", std::vector<std::shared_ptr<zir::Value>>{},
      std::vector<bool>{}, nullptr));
  entry->addInstruction(std::make_unique<BorrowInst>(view, text));
  entry->addInstruction(
      std::make_unique<CastInst>(castView, view, stringViewType));
  entry->addInstruction(std::make_unique<ReturnInst>(castView));
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));

  return expect(hasError(ZirVerifier().verify(module),
                         VerificationErrorCode::InvalidReturn),
                "returning a cast function-local StringView was not "
                "diagnosed");
}

bool testStoreRejectsEscapingFunctionLocalStringView() {
  Module module("local-string-view-store");
  auto stringType = zir::makeStringType();
  auto stringViewType = zir::makeStringViewType();
  auto stringViewPointer = std::make_shared<PointerType>(stringViewType);
  auto make = std::make_unique<Function>("make", stringType);
  module.addExternalFunction(std::move(make));

  auto function =
      std::make_unique<Function>("broken", primitive(TypeKind::Void));
  auto destination =
      std::make_shared<zir::Argument>("destination", stringViewPointer);
  function->arguments.push_back(destination);
  auto entry = std::make_unique<BasicBlock>("entry");
  auto text = reg("text", stringType);
  text->setOwnership(ValueOwnership::Owned);
  auto view = reg("view", stringViewType);
  entry->addInstruction(std::make_unique<zir::CallInst>(
      text, "make", std::vector<std::shared_ptr<zir::Value>>{},
      std::vector<bool>{}, nullptr));
  entry->addInstruction(std::make_unique<BorrowInst>(view, text));
  entry->addInstruction(
      std::make_unique<StoreInst>(view, destination, StoreMode::Assign));
  entry->addInstruction(std::make_unique<ReturnInst>());
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));

  const auto verification = ZirVerifier().verify(module);
  return expect(hasError(verification, VerificationErrorCode::InvalidOperand),
                "escaping store of a function-local StringView was not "
                "diagnosed");
}

bool testOwnershipLoweringReleasesDeadOwnedResults() {
  Module module("ownership-lowering");
  auto classType = std::make_shared<ClassType>("Node");
  auto function =
      std::make_unique<Function>("valid", primitive(TypeKind::Void));
  auto entry = std::make_unique<BasicBlock>("entry");
  auto result = reg("node", classType);
  result->setOwnership(ValueOwnership::Owned);
  entry->addInstruction(std::make_unique<zir::AllocInst>(result, classType));
  entry->addInstruction(std::make_unique<ReturnInst>());
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));

  zir::lowerDeadOwnedResults(module);
  const auto &instructions =
      module.getFunctions().front()->getBlocks().front()->getInstructions();
  return expect(instructions.size() == 3 &&
                    instructions[1]->getOpCode() == OpCode::Destroy &&
                    instructions[1]->toString().rfind("destroy ", 0) == 0,
                "ownership lowering did not destroy a dead owned result") &&
         expect(ZirVerifier().verify(module).ok(),
                "ownership-lowered ZIR was rejected by the verifier");
}

bool testOwnershipLoweringPreservesManagedAssignmentCopy() {
  Module module("ownership-assignment-copy-lowering");
  auto classType = std::make_shared<ClassType>("Node");
  auto boolean = primitive(TypeKind::Bool);
  auto function =
      std::make_unique<Function>("valid", primitive(TypeKind::Void));
  auto source = std::make_shared<zir::Argument>("source", classType);
  source->setOwnership(ValueOwnership::Owned);
  function->arguments.push_back(source);

  auto entry = std::make_unique<BasicBlock>("entry");
  auto slot = reg("slot", std::make_shared<PointerType>(classType));
  auto copied = reg("copied", classType);
  copied->setOwnership(ValueOwnership::Owned);
  auto comparison = reg("comparison", boolean);
  entry->addInstruction(std::make_unique<zir::AllocaInst>(slot, classType));
  entry->addInstruction(std::make_unique<CopyInst>(copied, source));
  entry->addInstruction(
      std::make_unique<StoreInst>(copied, slot, StoreMode::Assign));
  entry->addInstruction(std::make_unique<CmpInst>(
      "eq", comparison, source, std::make_shared<Constant>("null", classType)));
  entry->addInstruction(std::make_unique<ReturnInst>());
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));

  zir::lowerDeadOwnedResults(module);
  const auto &instructions =
      module.getFunctions().front()->getBlocks().front()->getInstructions();
  if (instructions.size() < 3 || instructions[1]->getOpCode() != OpCode::Copy ||
      instructions[2]->getOpCode() != OpCode::Store) {
    return expect(false, "managed assignment copy was not preserved");
  }
  const auto &copy = static_cast<const CopyInst &>(*instructions[1]);
  const auto &store = static_cast<const StoreInst &>(*instructions[2]);
  return expect(instructions.size() == 6 &&
                    store.getSource() == copy.getResult() &&
                    store.getSource()->getOwnership() ==
                        ValueOwnership::Owned &&
                    instructions[3]->getOpCode() == OpCode::Cmp &&
                    instructions[4]->getOpCode() == OpCode::Destroy,
                "managed assignment copy did not transfer into the store") &&
         expect(ZirVerifier().verify(module).ok(),
                "managed assignment copy produced invalid ZIR") &&
         expect(ZirVerifier().verifyOwnershipObligations(module).ok(),
                "managed assignment copy left ownership open");
}

bool testOwnershipLoweringClosesUnambiguousOwnedArgumentAtReturn() {
  Module module("ownership-return-closure-lowering");
  auto classType = std::make_shared<ClassType>("Node");
  auto function =
      std::make_unique<Function>("valid", primitive(TypeKind::Void));
  auto value = std::make_shared<zir::Argument>("value", classType);
  value->setOwnership(ValueOwnership::Owned);
  function->arguments.push_back(value);
  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<ReturnInst>());
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));

  zir::lowerDeadOwnedResults(module);
  const auto &instructions =
      module.getFunctions().front()->getBlocks().front()->getInstructions();
  return expect(instructions.size() == 2 &&
                    instructions.front()->getOpCode() == OpCode::Destroy &&
                    instructions.back()->getOpCode() == OpCode::Ret,
                "ownership lowering did not close an owned argument before "
                "return") &&
         expect(ZirVerifier().verify(module).ok(),
                "return-closure-lowered ZIR was rejected by the verifier") &&
         expect(ZirVerifier().verifyOwnershipObligations(module).ok(),
                "return-closure lowering left an ownership obligation");
}

bool testOwnershipLoweringClosesSimpleEdgeObligations() {
  Module module("ownership-simple-edge-closure");
  auto classType = std::make_shared<ClassType>("Node");
  auto boolean = primitive(TypeKind::Bool);
  auto function =
      std::make_unique<Function>("valid", primitive(TypeKind::Void));
  auto value = std::make_shared<zir::Argument>("value", classType);
  value->setOwnership(ValueOwnership::Owned);
  function->arguments.push_back(value);

  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<CondBranchInst>(
      std::make_shared<Constant>("true", boolean), "live", "closed"));
  auto live = std::make_unique<BasicBlock>("live");
  live->addInstruction(std::make_unique<BranchInst>("exit"));
  auto closed = std::make_unique<BasicBlock>("closed");
  closed->addInstruction(std::make_unique<DestroyInst>(value));
  closed->addInstruction(std::make_unique<BranchInst>("exit"));
  auto exit = std::make_unique<BasicBlock>("exit");
  exit->addInstruction(std::make_unique<ReturnInst>());
  function->addBlock(std::move(entry));
  function->addBlock(std::move(live));
  function->addBlock(std::move(closed));
  function->addBlock(std::move(exit));
  module.addFunction(std::move(function));

  zir::lowerDeadOwnedResults(module);
  const auto &blocks = module.getFunctions().front()->getBlocks();
  const auto &liveInstructions = blocks[1]->getInstructions();
  return expect(blocks.size() == 4 && liveInstructions.size() == 2 &&
                    liveInstructions.front()->getOpCode() == OpCode::Destroy &&
                    liveInstructions.back()->getOpCode() == OpCode::Br,
                "ownership lowering did not close a simple live CFG edge") &&
         expect(ZirVerifier().verify(module).ok(),
                "simple edge closure produced invalid ZIR") &&
         expect(ZirVerifier().verifyOwnershipObligations(module).ok(),
                "simple edge closure left an ownership obligation");
}

bool testOwnershipLoweringClosesCriticalEdgeObligations() {
  Module module("ownership-critical-edge-closure");
  auto classType = std::make_shared<ClassType>("Node");
  auto boolean = primitive(TypeKind::Bool);
  auto i32 = primitive(TypeKind::Int32);
  auto function =
      std::make_unique<Function>("valid", primitive(TypeKind::Void));
  auto value = std::make_shared<zir::Argument>("value", classType);
  value->setOwnership(ValueOwnership::Owned);
  function->arguments.push_back(value);

  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<CondBranchInst>(
      std::make_shared<Constant>("true", boolean), "exit", "closed"));
  auto closed = std::make_unique<BasicBlock>("closed");
  closed->addInstruction(std::make_unique<DestroyInst>(value));
  closed->addInstruction(std::make_unique<BranchInst>("exit"));
  auto exit = std::make_unique<BasicBlock>("exit");
  auto selected = reg("selected", i32);
  auto comparison = reg("comparison", boolean);
  exit->addInstruction(std::make_unique<PhiInst>(
      selected,
      std::vector<std::pair<std::string, std::shared_ptr<zir::Value>>>{
          {"entry", std::make_shared<Constant>("1", i32)},
          {"closed", std::make_shared<Constant>("2", i32)}}));
  exit->addInstruction(std::make_unique<CmpInst>(
      "eq", comparison, selected, std::make_shared<Constant>("1", i32)));
  exit->addInstruction(std::make_unique<ReturnInst>());
  function->addBlock(std::move(entry));
  function->addBlock(std::move(closed));
  function->addBlock(std::move(exit));
  module.addFunction(std::move(function));

  zir::lowerDeadOwnedResults(module);
  auto *lowered = module.getFunctions().front().get();
  const auto &branch = static_cast<const CondBranchInst &>(
      *lowered->findBlock("entry")->getInstructions().back());
  auto *edge = lowered->findBlock(branch.getTrueLabel());
  auto *exitBlock = lowered->findBlock("exit");
  const auto &phi =
      static_cast<const PhiInst &>(*exitBlock->getInstructions().front());
  return expect(edge && edge->getInstructions().size() == 2 &&
                    edge->getInstructions().front()->getOpCode() ==
                        OpCode::Destroy &&
                    edge->getInstructions().back()->getOpCode() == OpCode::Br &&
                    phi.getIncoming().front().first == edge->label,
                "ownership lowering did not split the critical edge and "
                "update phi labels") &&
         expect(ZirVerifier().verify(module).ok(),
                "critical edge closure produced invalid ZIR") &&
         expect(ZirVerifier().verifyOwnershipObligations(module).ok(),
                "critical edge closure left an ownership obligation");
}

bool testOwnershipLoweringReleasesAtLastLocalUse() {
  Module module("ownership-last-use-lowering");
  auto classType = std::make_shared<ClassType>("Node");
  auto boolean = primitive(TypeKind::Bool);
  auto function =
      std::make_unique<Function>("valid", primitive(TypeKind::Void));
  auto entry = std::make_unique<BasicBlock>("entry");
  auto node = reg("node", classType);
  node->setOwnership(ValueOwnership::Owned);
  auto comparison = reg("comparison", boolean);
  entry->addInstruction(std::make_unique<zir::AllocInst>(node, classType));
  entry->addInstruction(std::make_unique<CmpInst>(
      "eq", comparison, node, std::make_shared<Constant>("null", classType)));
  entry->addInstruction(std::make_unique<ReturnInst>());
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));

  zir::lowerDeadOwnedResults(module);
  const auto &instructions =
      module.getFunctions().front()->getBlocks().front()->getInstructions();
  return expect(instructions.size() == 4 &&
                    instructions[2]->getOpCode() == OpCode::Destroy,
                "ownership lowering did not destroy at the local last use") &&
         expect(ZirVerifier().verify(module).ok(),
                "last-use-lowered ZIR was rejected by the verifier");
}

bool testOwnershipLoweringReleasesOwnerAfterBorrowUse() {
  Module module("ownership-borrow-lowering");
  auto stringType = zir::makeStringType();
  auto stringViewType = zir::makeStringViewType();
  auto make = std::make_unique<Function>("make", stringType);
  module.addExternalFunction(std::move(make));
  auto consume =
      std::make_unique<Function>("consume", primitive(TypeKind::Void));
  consume->arguments.push_back(std::make_shared<zir::Argument>(
      "view", stringViewType, false, false, nullptr,
      zir::ParameterOwnership::Borrow, zir::ParameterEscape::NoEscape));
  module.addExternalFunction(std::move(consume));

  auto function =
      std::make_unique<Function>("valid", primitive(TypeKind::Void));
  auto entry = std::make_unique<BasicBlock>("entry");
  auto text = reg("text", stringType);
  text->setOwnership(ValueOwnership::Owned);
  auto view = reg("view", stringViewType);
  entry->addInstruction(std::make_unique<zir::CallInst>(
      text, "make", std::vector<std::shared_ptr<zir::Value>>{},
      std::vector<bool>{}, nullptr));
  entry->addInstruction(std::make_unique<BorrowInst>(view, text));
  entry->addInstruction(std::make_unique<zir::CallInst>(
      nullptr, "consume", std::vector<std::shared_ptr<zir::Value>>{view},
      std::vector<bool>{false}, nullptr));
  entry->addInstruction(std::make_unique<ReturnInst>());
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));

  zir::lowerDeadOwnedResults(module);
  const auto &instructions =
      module.getFunctions().front()->getBlocks().front()->getInstructions();
  return expect(instructions.size() == 5 &&
                    instructions[3]->getOpCode() == OpCode::Destroy,
                "ownership lowering destroyed a String owner before its "
                "borrowed view's last use") &&
         expect(ZirVerifier().verify(module).ok(),
                "borrow-lowered ZIR was rejected by the verifier");
}

bool testCallBorrowAllowsOwnedValueToBeReleasedAfterward() {
  Module module("call-borrow-ownership");
  auto classType = std::make_shared<ClassType>("Node");

  auto make = std::make_unique<Function>("make", classType);
  module.addExternalFunction(std::move(make));
  auto consume =
      std::make_unique<Function>("consume", primitive(TypeKind::Void));
  consume->arguments.push_back(
      std::make_shared<zir::Argument>("value", classType));
  module.addExternalFunction(std::move(consume));

  auto function =
      std::make_unique<Function>("valid", primitive(TypeKind::Void));
  auto entry = std::make_unique<BasicBlock>("entry");
  auto value = reg("value", classType);
  value->setOwnership(ValueOwnership::Owned);
  entry->addInstruction(std::make_unique<zir::CallInst>(
      value, "make", std::vector<std::shared_ptr<zir::Value>>{},
      std::vector<bool>{}, nullptr));
  entry->addInstruction(std::make_unique<zir::CallInst>(
      nullptr, "consume", std::vector<std::shared_ptr<zir::Value>>{value}));
  entry->addInstruction(std::make_unique<ReturnInst>());
  function->addBlock(std::move(entry));
  module.addFunction(std::move(function));

  zir::lowerDeadOwnedResults(module);
  const auto &instructions =
      module.getFunctions().front()->getBlocks().front()->getInstructions();
  return expect(instructions.size() == 4 &&
                    instructions[2]->getOpCode() == OpCode::Destroy,
                "borrowed call did not preserve a later destroy") &&
         expect(ZirVerifier().verify(module).ok(),
                "borrowed call of an owned value was rejected");
}

bool testOwnershipLoweringReleasesOnDeadCfgEdge() {
  Module module("ownership-edge-lowering");
  auto classType = std::make_shared<ClassType>("Node");
  auto boolean = primitive(TypeKind::Bool);
  auto function =
      std::make_unique<Function>("valid", primitive(TypeKind::Void));

  auto entry = std::make_unique<BasicBlock>("entry");
  auto node = reg("node", classType);
  node->setOwnership(ValueOwnership::Owned);
  entry->addInstruction(std::make_unique<zir::AllocInst>(node, classType));
  entry->addInstruction(std::make_unique<CondBranchInst>(
      std::make_shared<Constant>("true", boolean), "live", "dead"));

  auto live = std::make_unique<BasicBlock>("live");
  auto comparison = reg("comparison", boolean);
  live->addInstruction(std::make_unique<CmpInst>(
      "eq", comparison, node, std::make_shared<Constant>("null", classType)));
  live->addInstruction(std::make_unique<ReturnInst>());

  auto dead = std::make_unique<BasicBlock>("dead");
  dead->addInstruction(std::make_unique<ReturnInst>());

  function->addBlock(std::move(entry));
  function->addBlock(std::move(live));
  function->addBlock(std::move(dead));
  module.addFunction(std::move(function));

  zir::lowerDeadOwnedResults(module);
  const auto &blocks = module.getFunctions().front()->getBlocks();
  const auto &branch = static_cast<const CondBranchInst &>(
      *blocks.front()->getInstructions().back());
  auto *releaseBlock =
      module.getFunctions().front()->findBlock(branch.getFalseLabel());
  return expect(blocks.size() == 4 && releaseBlock &&
                    releaseBlock->getInstructions().size() == 2 &&
                    releaseBlock->getInstructions().front()->getOpCode() ==
                        OpCode::Destroy,
                "ownership lowering did not split the dead CFG edge") &&
         expect(ZirVerifier().verify(module).ok(),
                "edge-lowered ZIR was rejected by the verifier");
}

bool testOwnershipLoweringRemovesUnusedBorrowedPhiBeforeCleanup() {
  Module module("ownership-unused-borrowed-phi");
  auto stringType = zir::makeStringType();
  auto boolean = primitive(TypeKind::Bool);
  auto make = std::make_unique<Function>("make", stringType);
  module.addExternalFunction(std::move(make));

  auto function =
      std::make_unique<Function>("valid", primitive(TypeKind::Void));
  auto borrowed = std::make_shared<zir::Argument>("borrowed", stringType);
  borrowed->setOwnership(ValueOwnership::Borrowed);
  function->arguments.push_back(borrowed);

  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<CondBranchInst>(
      std::make_shared<Constant>("true", boolean), "borrowed.path",
      "owned.path"));

  auto borrowedPath = std::make_unique<BasicBlock>("borrowed.path");
  borrowedPath->addInstruction(std::make_unique<BranchInst>("merge"));

  auto ownedPath = std::make_unique<BasicBlock>("owned.path");
  auto owned = reg("owned", stringType);
  owned->setOwnership(ValueOwnership::Owned);
  ownedPath->addInstruction(std::make_unique<zir::CallInst>(
      owned, "make", std::vector<std::shared_ptr<zir::Value>>{},
      std::vector<bool>{}, nullptr));
  ownedPath->addInstruction(std::make_unique<BranchInst>("merge"));

  auto merge = std::make_unique<BasicBlock>("merge");
  auto unused = reg("unused", stringType);
  unused->setOwnership(ValueOwnership::Borrowed);
  merge->addInstruction(std::make_unique<PhiInst>(
      unused, std::vector<std::pair<std::string, std::shared_ptr<zir::Value>>>{
                  {"borrowed.path", borrowed}, {"owned.path", owned}}));
  merge->addInstruction(std::make_unique<ReturnInst>());

  function->addBlock(std::move(entry));
  function->addBlock(std::move(borrowedPath));
  function->addBlock(std::move(ownedPath));
  function->addBlock(std::move(merge));
  module.addFunction(std::move(function));

  zir::lowerDeadOwnedResults(module);
  auto *lowered = module.getFunctions().front().get();
  const auto &ownedInstructions =
      lowered->findBlock("owned.path")->getInstructions();
  const auto &mergeInstructions =
      lowered->findBlock("merge")->getInstructions();
  return expect(ownedInstructions.size() == 3 &&
                    ownedInstructions[1]->getOpCode() == OpCode::Destroy &&
                    mergeInstructions.size() == 1 &&
                    mergeInstructions.front()->getOpCode() == OpCode::Ret,
                "ownership lowering did not remove an unused borrowed phi "
                "before closing its owned incoming value") &&
         expect(ZirVerifier().verify(module).ok(),
                "unused borrowed phi cleanup produced invalid ZIR") &&
         expect(ZirVerifier().verifyOwnershipObligations(module).ok(),
                "unused borrowed phi cleanup left ownership open");
}

bool testDominanceViolation() {
  Module module("dominance-violation");
  auto i32 = primitive(TypeKind::Int32);
  auto boolean = primitive(TypeKind::Bool);
  auto function = std::make_unique<Function>("broken", i32);

  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<CondBranchInst>(
      std::make_shared<Constant>("true", boolean), "defines", "skips"));

  auto defines = std::make_unique<BasicBlock>("defines");
  auto onePathValue = reg("one.path", i32);
  defines->addInstruction(std::make_unique<zir::BinaryInst>(
      OpCode::Add, onePathValue, std::make_shared<Constant>("1", i32),
      std::make_shared<Constant>("2", i32)));
  defines->addInstruction(std::make_unique<BranchInst>("merge"));

  auto skips = std::make_unique<BasicBlock>("skips");
  skips->addInstruction(std::make_unique<BranchInst>("merge"));

  auto merge = std::make_unique<BasicBlock>("merge");
  merge->addInstruction(std::make_unique<ReturnInst>(onePathValue));

  function->addBlock(std::move(entry));
  function->addBlock(std::move(defines));
  function->addBlock(std::move(skips));
  function->addBlock(std::move(merge));
  module.addFunction(std::move(function));
  auto result = ZirVerifier().verify(module);
  return expect(hasError(result, VerificationErrorCode::DominanceViolation),
                "non-dominating definition was not diagnosed");
}

bool testPhiRequiresEveryPredecessor() {
  Module module("incomplete-phi");
  auto i32 = primitive(TypeKind::Int32);
  auto boolean = primitive(TypeKind::Bool);
  auto function = std::make_unique<Function>("broken", i32);
  auto entry = std::make_unique<BasicBlock>("entry");
  entry->addInstruction(std::make_unique<CondBranchInst>(
      std::make_shared<Constant>("true", boolean), "left", "right"));
  auto left = std::make_unique<BasicBlock>("left");
  left->addInstruction(std::make_unique<BranchInst>("merge"));
  auto right = std::make_unique<BasicBlock>("right");
  right->addInstruction(std::make_unique<BranchInst>("merge"));
  auto merge = std::make_unique<BasicBlock>("merge");
  auto result = reg("result", i32);
  merge->addInstruction(std::make_unique<PhiInst>(
      result, std::vector<std::pair<std::string, std::shared_ptr<zir::Value>>>{
                  {"left", std::make_shared<Constant>("1", i32)}}));
  merge->addInstruction(std::make_unique<ReturnInst>(result));
  function->addBlock(std::move(entry));
  function->addBlock(std::move(left));
  function->addBlock(std::move(right));
  function->addBlock(std::move(merge));
  module.addFunction(std::move(function));
  auto verification = ZirVerifier().verify(module);
  return expect(hasError(verification, VerificationErrorCode::InvalidPhi),
                "incomplete phi was not diagnosed");
}

} // namespace

int main() {
  bool ok = true;
  ok = testValidFunction() && ok;
  ok = testMissingTerminator() && ok;
  ok = testUnknownBranchTarget() && ok;
  ok = testInstructionAfterTerminator() && ok;
  ok = testUseBeforeDefinition() && ok;
  ok = testStoreTypeMismatch() && ok;
  ok = testStoreModeRendering() && ok;
  ok = testAllocRequiresOwnedResult() && ok;
  ok = testWeakLockRequiresOwnedStrongResult() && ok;
  ok = testManagedCallRequiresOwnedResult() && ok;
  ok = testRefCallDerivesResultTypeFromSignature() && ok;
  ok = testManagedTypeClassification() && ok;
  ok = testPhiRequiresOwnershipMatchingIncomingValues() && ok;
  ok = testManagedInitializationRequiresOwnershipTransfer() && ok;
  ok = testCastRequiresOwnershipMatchingSourceAndTarget() && ok;
  ok = testCallRequiresOwnershipMatchingArguments() && ok;
  ok = testIndirectCallDerivesNoEscapeFromFunctionType() && ok;
  ok = testCallConsumesExplicitManagedCopy() && ok;
  ok = testOwnershipTransferAcrossControlFlow() && ok;
  ok = testPhiTransfersOwnershipOnIncomingEdge() && ok;
  ok = testPhiAllowsSeparateAlternativeOwnershipTransfers() && ok;
  ok = testOwnershipLivenessTracksPhiEdges() && ok;
  ok = testOwnershipFlowTracksEdgesMergesAndLoops() && ok;
  ok = testOwnershipFlowRejectsTransferAfterPartialDefinition() && ok;
  ok = testControlFlowGraphBuildsEdgesAndReachability() && ok;
  ok = testDestroyConsumesOwnedValue() && ok;
  ok = testUseAfterDestroyIsRejected() && ok;
  ok = testUseAfterMoveIsRejected() && ok;
  ok = testCopyCreatesIndependentOwnership() && ok;
  ok = testMoveTransfersOwnershipIntoInitialization() && ok;
  ok = testWeakCopyTransfersWeakOwnership() && ok;
  ok = testWeakCopyRejectsStrongOwnership() && ok;
  ok = testOwnershipExitObligationsReportLiveValues() && ok;
  ok = testOwnershipClosurePlanConnectsDefinitionAndExit() && ok;
  ok = testOwnershipClosurePlanUsesCriticalLiveEdge() && ok;
  ok = testOwnershipExitObligationsAllowMovedReturn() && ok;
  ok = testOwnershipObligationVerifierReportsLiveValues() && ok;
  ok = testOwnershipExitObligationsTrackPartialDefinitions() && ok;
  ok = testOwnershipExitObligationsAllowPhiTransfer() && ok;
  ok = testOwnershipExitObligationsTrackLoops() && ok;
  ok = testBorrowPreservesOwnedString() && ok;
  ok = testBorrowRequiresStringOwnerAndBorrowedStringView() && ok;
  ok = testDestroyRejectsLiveBorrowedStringView() && ok;
  ok = testOwnershipLivenessTracksBorrowPhiEdges() && ok;
  ok = testOwnershipLivenessTracksBorrowedViewsThroughLocalStorage() && ok;
  ok = testReturnRejectsFunctionLocalStringView() && ok;
  ok = testReturnAllowsBorrowedStringView() && ok;
  ok = testReturnRejectsStringViewLoadedFromLocalStorage() && ok;
  ok = testReturnRejectsCastFunctionLocalStringView() && ok;
  ok = testStoreRejectsEscapingFunctionLocalStringView() && ok;
  ok = testOwnershipLoweringReleasesDeadOwnedResults() && ok;
  ok = testOwnershipLoweringPreservesManagedAssignmentCopy() && ok;
  ok = testOwnershipLoweringClosesUnambiguousOwnedArgumentAtReturn() && ok;
  ok = testOwnershipLoweringClosesSimpleEdgeObligations() && ok;
  ok = testOwnershipLoweringClosesCriticalEdgeObligations() && ok;
  ok = testOwnershipLoweringReleasesAtLastLocalUse() && ok;
  ok = testOwnershipLoweringReleasesOwnerAfterBorrowUse() && ok;
  ok = testCallBorrowAllowsOwnedValueToBeReleasedAfterward() && ok;
  ok = testOwnershipLoweringReleasesOnDeadCfgEdge() && ok;
  ok = testOwnershipLoweringRemovesUnusedBorrowedPhiBeforeCleanup() && ok;
  ok = testDominanceViolation() && ok;
  ok = testPhiRequiresEveryPredecessor() && ok;
  return ok ? 0 : 1;
}
