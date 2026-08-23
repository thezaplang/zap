#include "ir/function_reachability.hpp"
#include "ir/type.hpp"
#include "sema/bound_nodes.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const std::string &message) {
  if (!condition)
    std::cerr << message << '\n';
  return condition;
}

std::shared_ptr<sema::FunctionSymbol> makeFunction(const std::string &name) {
  return std::make_shared<sema::FunctionSymbol>(
      name, std::vector<std::shared_ptr<sema::VariableSymbol>>{},
      std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void));
}

std::unique_ptr<sema::BoundFunctionDeclaration>
functionWithExpression(const std::shared_ptr<sema::FunctionSymbol> &symbol,
                       std::unique_ptr<sema::BoundExpression> expression) {
  auto body = std::make_unique<sema::BoundBlock>();
  if (expression) {
    body->statements.push_back(std::make_unique<sema::BoundExpressionStatement>(
        std::move(expression)));
  }
  return std::make_unique<sema::BoundFunctionDeclaration>(symbol,
                                                          std::move(body));
}

bool contains(const std::unordered_set<const sema::FunctionSymbol *> &functions,
              const std::shared_ptr<sema::FunctionSymbol> &symbol) {
  return functions.count(symbol.get()) == 1;
}

bool testReachabilityRootsAndDependencies() {
  auto entry = makeFunction("start");
  entry->isEntryModule = true;
  auto recursiveFirst = makeFunction("recursive_first");
  auto recursiveSecond = makeFunction("recursive_second");
  auto functionReferenceTarget = makeFunction("callback_target");
  functionReferenceTarget->ownerTypeCodegenName = "test.Callback";
  functionReferenceTarget->vtableSlot = 3;
  auto globalTarget = makeFunction("global_target");
  auto noMangle = makeFunction("exported");
  noMangle->hasNoMangle = true;
  auto attributedEntry = makeFunction("explicit_entry");
  attributedEntry->hasEntry = true;
  auto virtualMethod = makeFunction("virtual_method");
  virtualMethod->ownerTypeCodegenName = "test.Virtual";
  virtualMethod->vtableSlot = 4;
  auto virtualDependency = makeFunction("virtual_dependency");
  auto unreachable = makeFunction("unreachable");
  auto external = makeFunction("puts");
  external->isExternal = true;

  sema::BoundRootNode root;
  root.functions.push_back(functionWithExpression(
      entry, std::make_unique<sema::BoundFunctionCall>(
                 recursiveFirst,
                 std::vector<std::unique_ptr<sema::BoundExpression>>{})));
  root.functions.push_back(functionWithExpression(
      recursiveFirst,
      std::make_unique<sema::BoundFunctionCall>(
          recursiveSecond,
          std::vector<std::unique_ptr<sema::BoundExpression>>{})));
  root.functions.push_back(functionWithExpression(
      recursiveSecond,
      std::make_unique<sema::BoundFunctionCall>(
          recursiveFirst,
          std::vector<std::unique_ptr<sema::BoundExpression>>{})));
  root.functions.push_back(functionWithExpression(
      functionReferenceTarget,
      std::make_unique<sema::BoundLiteral>(
          "0", std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int))));
  root.functions.push_back(functionWithExpression(
      noMangle,
      std::make_unique<sema::BoundFunctionReference>(
          functionReferenceTarget,
          std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void))));
  root.functions.push_back(functionWithExpression(
      attributedEntry,
      std::make_unique<sema::BoundFunctionCall>(
          external, std::vector<std::unique_ptr<sema::BoundExpression>>{})));
  root.functions.push_back(functionWithExpression(
      virtualMethod,
      std::make_unique<sema::BoundFunctionCall>(
          virtualDependency,
          std::vector<std::unique_ptr<sema::BoundExpression>>{})));
  root.functions.push_back(functionWithExpression(virtualDependency, nullptr));
  root.functions.push_back(functionWithExpression(unreachable, nullptr));
  root.functions.push_back(functionWithExpression(globalTarget, nullptr));
  root.externalFunctions.push_back(
      std::make_unique<sema::BoundExternalFunctionDeclaration>(external));

  auto global = std::make_shared<sema::VariableSymbol>(
      "callback", std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void));
  root.globals.push_back(std::make_unique<sema::BoundVariableDeclaration>(
      global, std::make_unique<sema::BoundFunctionReference>(
                  globalTarget,
                  std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void))));

  const auto result = zir::FunctionReachabilityAnalyzer{}.analyze(root);
  const auto vtableSlots = result.liveVtableSlots.find("test.Callback");
  return expect(contains(result.functions, entry),
                "entry module function is not live") &&
         expect(contains(result.functions, recursiveFirst),
                "direct dependency is not live") &&
         expect(contains(result.functions, recursiveSecond),
                "recursive dependency is not live") &&
         expect(contains(result.functions, functionReferenceTarget),
                "function reference is not live") &&
         expect(contains(result.functions, globalTarget),
                "global initializer dependency is not live") &&
         expect(contains(result.functions, noMangle),
                "noMangle function is not a root") &&
         expect(contains(result.functions, attributedEntry),
                "@entry function is not a root") &&
         expect(!contains(result.functions, virtualMethod),
                "unreachable virtual method is live") &&
         expect(!contains(result.functions, virtualDependency),
                "dependency of an unreachable virtual method is live") &&
         expect(!contains(result.functions, unreachable),
                "unreachable function is live") &&
         expect(vtableSlots != result.liveVtableSlots.end() &&
                    vtableSlots->second.count(3) == 1,
                "live virtual method slot is missing") &&
         expect(contains(result.externalFunctions, external),
                "external dependency is not live");
}

bool testLiveClassRetainsVirtualOverridesAndDestructors() {
  auto base = std::make_shared<zir::ClassType>("Base", "test.Base");
  auto derived = std::make_shared<zir::ClassType>("Derived", "test.Derived");
  derived->setBase(base);

  auto entry = makeFunction("main");
  entry->isEntryModule = true;
  auto baseMethod = makeFunction("run");
  baseMethod->ownerTypeCodegenName = "test.Base";
  baseMethod->vtableSlot = 0;
  auto derivedOverride = makeFunction("run");
  derivedOverride->ownerTypeCodegenName = "test.Derived";
  derivedOverride->vtableSlot = 0;
  auto unusedMethod = makeFunction("unused");
  unusedMethod->ownerTypeCodegenName = "test.Derived";
  unusedMethod->vtableSlot = 1;
  auto baseDestructor = makeFunction("deinit");
  baseDestructor->ownerTypeCodegenName = "test.Base";
  baseDestructor->isDestructor = true;
  auto derivedDestructor = makeFunction("deinit");
  derivedDestructor->ownerTypeCodegenName = "test.Derived";
  derivedDestructor->isDestructor = true;
  auto constructor = makeFunction("init");
  constructor->ownerTypeCodegenName = "test.Derived";
  constructor->isConstructor = true;

  auto entryBody = std::make_unique<sema::BoundBlock>();
  entryBody->statements.push_back(
      std::make_unique<sema::BoundExpressionStatement>(
          std::make_unique<sema::BoundNewExpression>(
              derived, constructor,
              std::vector<std::unique_ptr<sema::BoundExpression>>{})));
  entryBody->statements.push_back(
      std::make_unique<sema::BoundExpressionStatement>(
          std::make_unique<sema::BoundFunctionCall>(
              baseMethod,
              std::vector<std::unique_ptr<sema::BoundExpression>>{})));

  sema::BoundRootNode root;
  auto baseRecord = std::make_unique<sema::BoundRecordDeclaration>();
  baseRecord->type = base;
  root.records.push_back(std::move(baseRecord));
  auto derivedRecord = std::make_unique<sema::BoundRecordDeclaration>();
  derivedRecord->type = derived;
  root.records.push_back(std::move(derivedRecord));
  root.functions.push_back(std::make_unique<sema::BoundFunctionDeclaration>(
      entry, std::move(entryBody)));
  root.functions.push_back(functionWithExpression(baseMethod, nullptr));
  root.functions.push_back(functionWithExpression(derivedOverride, nullptr));
  root.functions.push_back(functionWithExpression(unusedMethod, nullptr));
  root.functions.push_back(functionWithExpression(baseDestructor, nullptr));
  root.functions.push_back(functionWithExpression(derivedDestructor, nullptr));
  root.functions.push_back(functionWithExpression(constructor, nullptr));

  const auto result = zir::FunctionReachabilityAnalyzer{}.analyze(root);
  return expect(contains(result.functions, baseMethod),
                "live vtable slot is not retained for its base class") &&
         expect(contains(result.functions, derivedOverride),
                "live vtable slot is not retained for a derived class") &&
         expect(!contains(result.functions, unusedMethod),
                "unused vtable slot is live") &&
         expect(contains(result.functions, baseDestructor),
                "base destructor is not retained for a live derived class") &&
         expect(contains(result.functions, derivedDestructor),
                "derived destructor is not retained") &&
         expect(contains(result.functions, constructor),
                "constructor is not retained for a live class");
}

bool testCasePatternsRetainNestedDependencies() {
  auto entry = makeFunction("main");
  entry->isEntryModule = true;
  auto literalPayloadTarget = makeFunction("literal_payload_target");
  auto nestedPatternTarget = makeFunction("nested_pattern_target");
  auto intType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int);

  // Reachability must traverse every expression-bearing field in the bound
  // pattern tree. Current source syntax limits these to literals, but this
  // AST-level invariant keeps future expression patterns from silently
  // dropping function dependencies.

  auto payloadRecord =
      std::make_shared<zir::RecordType>("Payload", "test.Payload");
  payloadRecord->addField("value", intType);
  std::vector<sema::BoundCaseRecordField> fields;
  fields.push_back({0,
                    std::make_unique<sema::BoundCasePattern>(
                        std::make_unique<sema::BoundFunctionReference>(
                            nestedPatternTarget, intType)),
                    nullptr});
  auto nestedPayloadPattern = std::make_unique<sema::BoundCasePattern>(
      payloadRecord, std::move(fields));

  std::vector<sema::BoundCasePattern> literalPayloadPatterns;
  literalPayloadPatterns.emplace_back(
      sema::BoundCasePatternKind::TaggedUnionVariant, 0, intType,
      std::make_unique<sema::BoundFunctionReference>(literalPayloadTarget,
                                                     intType));
  std::vector<sema::BoundCasePattern> nestedPayloadPatterns;
  nestedPayloadPatterns.emplace_back(
      sema::BoundCasePatternKind::TaggedUnionVariant, 1, payloadRecord, nullptr,
      std::move(nestedPayloadPattern));

  std::vector<sema::BoundCaseArm> arms;
  arms.emplace_back(false, std::move(literalPayloadPatterns), nullptr,
                    std::vector<std::shared_ptr<sema::VariableSymbol>>{},
                    std::make_unique<sema::BoundBlock>());
  arms.emplace_back(false, std::move(nestedPayloadPatterns), nullptr,
                    std::vector<std::shared_ptr<sema::VariableSymbol>>{},
                    std::make_unique<sema::BoundBlock>());

  auto body = std::make_unique<sema::BoundBlock>();
  body->statements.push_back(std::make_unique<sema::BoundCaseStatement>(
      std::make_unique<sema::BoundLiteral>("0", intType), std::move(arms),
      false));

  sema::BoundRootNode root;
  root.functions.push_back(
      std::make_unique<sema::BoundFunctionDeclaration>(entry, std::move(body)));
  root.functions.push_back(
      functionWithExpression(literalPayloadTarget, nullptr));
  root.functions.push_back(
      functionWithExpression(nestedPatternTarget, nullptr));

  const auto result = zir::FunctionReachabilityAnalyzer{}.analyze(root);
  return expect(contains(result.functions, literalPayloadTarget),
                "case literal payload dependency is not live") &&
         expect(contains(result.functions, nestedPatternTarget),
                "nested case record payload dependency is not live");
}

bool testNewExpressionMarksClassAndConstructor() {
  auto entry = makeFunction("main");
  entry->isEntryModule = true;
  auto constructor = makeFunction("init");
  constructor->isConstructor = true;
  constructor->ownerTypeCodegenName = "test.Widget";
  auto widget = std::make_shared<zir::ClassType>("Widget", "test.Widget");

  sema::BoundRootNode root;
  root.functions.push_back(functionWithExpression(
      entry, std::make_unique<sema::BoundNewExpression>(
                 widget, constructor,
                 std::vector<std::unique_ptr<sema::BoundExpression>>{})));
  root.functions.push_back(functionWithExpression(constructor, nullptr));

  const auto result = zir::FunctionReachabilityAnalyzer{}.analyze(root);
  return expect(contains(result.functions, constructor),
                "constructor called by new is not live") &&
         expect(result.liveClassCodegenNames.count("test.Widget") == 1,
                "class allocated by new is not live");
}

} // namespace

int main() {
  bool ok = true;
  ok = testReachabilityRootsAndDependencies() && ok;
  ok = testCasePatternsRetainNestedDependencies() && ok;
  ok = testNewExpressionMarksClassAndConstructor() && ok;
  ok = testLiveClassRetainsVirtualOverridesAndDestructors() && ok;
  return ok ? 0 : 1;
}
