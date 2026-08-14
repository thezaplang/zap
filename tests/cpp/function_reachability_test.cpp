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
         expect(!contains(result.functions, unreachable),
                "unreachable function is live") &&
         expect(vtableSlots != result.liveVtableSlots.end() &&
                    vtableSlots->second.count(3) == 1,
                "live virtual method slot is missing") &&
         expect(contains(result.externalFunctions, external),
                "external dependency is not live");
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
  ok = testNewExpressionMarksClassAndConstructor() && ok;
  return ok ? 0 : 1;
}
