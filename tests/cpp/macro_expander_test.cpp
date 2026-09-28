#include "frontend/macro_registry.hpp"
#include "frontend/module_outline.hpp"
#include "lexer/lexer.hpp"
#include "macros/macro_expander.hpp"
#include "token/token_tree.hpp"

#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

struct RegistryFixture {
  std::map<std::string, std::string> sources;
  std::map<std::string, zap::frontend::ModuleOutline> outlines;
  zap::frontend::MacroRegistrySet::ImportGraph imports;
  std::vector<zap::frontend::MacroResolutionError> errors;
  zap::frontend::MacroRegistrySet registry;

  void add(const std::string &moduleId, std::string source) {
    auto [it, inserted] = sources.emplace(moduleId, std::move(source));
    require(inserted, "duplicate fixture module");
    zap::DiagnosticEngine diagnostics(it->second, moduleId);
    Lexer lexer(diagnostics);
    outlines.emplace(moduleId, zap::frontend::ModuleOutline::scan(
                                   lexer.tokenize(it->second), diagnostics));
    require(!diagnostics.hadErrors(), "fixture macro declaration is invalid");
  }

  void resolve() {
    registry =
        zap::frontend::MacroRegistrySet::resolve(outlines, imports, errors);
  }
};

zap::MacroCall makeCall(const std::string &source,
                        std::vector<std::string> path,
                        zap::DiagnosticEngine &diagnostics) {
  Lexer lexer(diagnostics);
  auto trees =
      TokenTreeBuilder::build(lexer.tokenize(source), diagnostics).trees;
  require(!trees.empty() && !trees.back().isLeaf(),
          "fixture invocation has no argument group");
  const auto span =
      SourceSpan::merge(trees.front().span(), trees.back().span());
  return {std::move(path), trees.back(), span, nullptr};
}

std::string spellings(const std::vector<TokenTree> &trees) {
  std::string result;
  for (const auto &token : flattenTokenTrees(trees))
    result += token.spelling;
  return result;
}

void testTypedCapturesAndOverloadSelection() {
  RegistryFixture fixture;
  fixture.add("module.zp", R"(
macro pick($x: tokens) { 0 }
macro pick($x: literal) { $x }
macro pick($x: ident) { 1 }
macro kinds($a: ident, $b: literal, $c: expr, $d: type,
            $e: stmt, $f: block, $g: item, $h: tokens) { $a }
)");
  fixture.resolve();
  require(fixture.errors.empty() &&
              fixture.registry.find("module.zp", "pick")->size() == 3,
          "distinct typed macro overloads were rejected");

  const std::string literal = "pick!(42)";
  zap::DiagnosticEngine literalDiagnostics(literal, "call.zp");
  zap::MacroExpander literalExpander(fixture.registry, literalDiagnostics);
  auto literalOutput = literalExpander.expand(
      "module.zp", makeCall(literal, {"pick"}, literalDiagnostics));
  require(literalOutput && spellings(*literalOutput) == "42" &&
              !literalDiagnostics.hadErrors(),
          "literal overload did not beat tokens overload");

  const std::string trailingComma = "pick!(42,)";
  zap::DiagnosticEngine trailingDiagnostics(trailingComma, "call.zp");
  zap::MacroExpander trailingExpander(fixture.registry, trailingDiagnostics);
  auto trailingOutput = trailingExpander.expand(
      "module.zp", makeCall(trailingComma, {"pick"}, trailingDiagnostics));
  require(trailingOutput && spellings(*trailingOutput) == "42",
          "trailing comma changed fixed-arity matching");

  RegistryFixture reversed;
  reversed.add("module.zp", "macro pick($x: literal) { $x } "
                            "macro pick($x: tokens) { 0 }");
  reversed.resolve();
  zap::DiagnosticEngine reversedDiagnostics(literal, "call.zp");
  zap::MacroExpander reversedExpander(reversed.registry, reversedDiagnostics);
  auto reversedOutput = reversedExpander.expand(
      "module.zp", makeCall(literal, {"pick"}, reversedDiagnostics));
  require(reversedOutput && spellings(*reversedOutput) == "42",
          "overload selection depended on declaration order");

  const std::string identifier = "pick!(answer)";
  zap::DiagnosticEngine identifierDiagnostics(identifier, "call.zp");
  zap::MacroExpander identifierExpander(fixture.registry,
                                        identifierDiagnostics);
  auto identifierOutput = identifierExpander.expand(
      "module.zp", makeCall(identifier, {"pick"}, identifierDiagnostics));
  require(identifierOutput && spellings(*identifierOutput) == "1",
          "identifier overload was not selected");

  const std::string tokens = "pick!(answer + 1)";
  zap::DiagnosticEngine tokenDiagnostics(tokens, "call.zp");
  zap::MacroExpander tokenExpander(fixture.registry, tokenDiagnostics);
  auto tokenOutput = tokenExpander.expand(
      "module.zp", makeCall(tokens, {"pick"}, tokenDiagnostics));
  require(tokenOutput && spellings(*tokenOutput) == "0",
          "tokens fallback overload was not selected");

  const std::string allKinds =
      "kinds!(name, 42, 1 + 2, Int, return 1;, { return 1; }, "
      "fun helper() {}, a + b)";
  zap::DiagnosticEngine kindsDiagnostics(allKinds, "call.zp");
  zap::MacroExpander kindsExpander(fixture.registry, kindsDiagnostics);
  auto kindsOutput = kindsExpander.expand(
      "module.zp", makeCall(allKinds, {"kinds"}, kindsDiagnostics));
  require(kindsOutput && spellings(*kindsOutput) == "name" &&
              !kindsDiagnostics.hadErrors(),
          "a valid typed scalar capture was rejected");
}

void testCustomPatternMatching() {
  RegistryFixture fixture;
  fixture.add("module.zp", R"(
macro pipe {
  ($value: expr |> $transform: expr) { $transform($value) }
  ($value: expr ; $transform: expr) { $transform($value) + 1 }
}
macro ambiguous { ($a: tokens $b: tokens) { $a } }
macro bounded { ($a: tokens $b: tokens) { $a } }
macro overload { ($value: expr) { 1 } ($value: type) { 2 } }
)");
  fixture.resolve();
  require(fixture.errors.empty(), "distinct pattern arms conflicted");

  const std::string pipe = "pipe!(3 + 4 |> double)";
  zap::DiagnosticEngine pipeDiagnostics(pipe, "call.zp");
  zap::MacroExpander expander(fixture.registry, pipeDiagnostics);
  auto result =
      expander.expand("module.zp", makeCall(pipe, {"pipe"}, pipeDiagnostics));
  require(result && spellings(*result) == "double(3+4)" &&
              !pipeDiagnostics.hadErrors(),
          "custom pipeline pattern failed to capture typed fragments");

  std::string longPipe = "pipe!(1";
  for (size_t index = 0; index < 150; ++index)
    longPipe += " + 1";
  longPipe += " |> double)";
  zap::DiagnosticEngine longDiagnostics(longPipe, "call.zp");
  zap::MacroExpander longExpander(fixture.registry, longDiagnostics);
  require(longExpander.expand("module.zp",
                              makeCall(longPipe, {"pipe"}, longDiagnostics)) &&
              !longDiagnostics.hadErrors(),
          "unambiguous long capture exhausted the backtracking budget");

  const std::string ambiguous = "ambiguous!(a b c)";
  zap::DiagnosticEngine ambiguityDiagnostics(ambiguous, "call.zp");
  zap::MacroExpander ambiguityExpander(fixture.registry, ambiguityDiagnostics);
  require(
      !ambiguityExpander.expand("module.zp", makeCall(ambiguous, {"ambiguous"},
                                                      ambiguityDiagnostics)) &&
          ambiguityDiagnostics.hadErrors() &&
          ambiguityDiagnostics.diagnostics().front().message.find(
              "Ambiguous macro pattern") != std::string::npos,
      "ambiguous custom pattern split was accepted");

  const std::string bounded = "bounded!(a b c)";
  zap::DiagnosticEngine boundedDiagnostics(bounded, "call.zp");
  zap::MacroLimits limits;
  limits.maxMatchAttempts = 3;
  zap::MacroExpander boundedExpander(fixture.registry, boundedDiagnostics,
                                     limits);
  require(!boundedExpander.expand("module.zp", makeCall(bounded, {"bounded"},
                                                        boundedDiagnostics)) &&
              boundedDiagnostics.hadErrors() &&
              boundedDiagnostics.diagnostics().front().message.find(
                  "matcher attempt limit") != std::string::npos,
          "custom pattern matcher ignored its backtracking limit");

  const std::string overload = "overload!(Name)";
  zap::DiagnosticEngine overloadDiagnostics(overload, "call.zp");
  zap::MacroExpander overloadExpander(fixture.registry, overloadDiagnostics);
  require(
      !overloadExpander.expand(
          "module.zp", makeCall(overload, {"overload"}, overloadDiagnostics)) &&
          overloadDiagnostics.hadErrors() &&
          overloadDiagnostics.diagnostics().front().message.find(
              "Ambiguous macro overload") != std::string::npos,
      "equally specific pattern arms were not diagnosed as ambiguous");

  RegistryFixture duplicate;
  duplicate.add("module.zp", "macro same($x: expr) { $x } "
                             "macro same { ($value: expr) { $value } }");
  duplicate.resolve();
  require(duplicate.errors.size() == 1,
          "signature macro and equivalent pattern arm did not conflict");
}

void testInvalidAndAmbiguousCalls() {
  RegistryFixture fixture;
  fixture.add("module.zp", R"(
macro typed($x: type) { $x }
macro ambiguous($x: expr) { 1 }
macro ambiguous($x: type) { 2 }
)");
  fixture.resolve();
  require(fixture.errors.empty(), "valid overload declarations conflicted");

  const std::string invalid = "typed!(1 +)";
  zap::DiagnosticEngine invalidDiagnostics(invalid, "call.zp");
  zap::MacroExpander invalidExpander(fixture.registry, invalidDiagnostics);
  require(!invalidExpander.expand(
              "module.zp", makeCall(invalid, {"typed"}, invalidDiagnostics)) &&
              invalidDiagnostics.hadErrors() &&
              invalidDiagnostics.diagnostics().front().message.find(
                  "not a valid type fragment") != std::string::npos,
          "invalid type capture matched");

  const std::string arity = "typed!(Int, Bool)";
  zap::DiagnosticEngine arityDiagnostics(arity, "call.zp");
  zap::MacroExpander arityExpander(fixture.registry, arityDiagnostics);
  require(!arityExpander.expand("module.zp",
                                makeCall(arity, {"typed"}, arityDiagnostics)) &&
              arityDiagnostics.hadErrors(),
          "wrong fixed arity matched");

  const std::string ambiguous = "ambiguous!(Name)";
  zap::DiagnosticEngine ambiguityDiagnostics(ambiguous, "call.zp");
  zap::MacroExpander ambiguityExpander(fixture.registry, ambiguityDiagnostics);
  require(
      !ambiguityExpander.expand("module.zp", makeCall(ambiguous, {"ambiguous"},
                                                      ambiguityDiagnostics)) &&
          ambiguityDiagnostics.hadErrors(),
      "equal-ranked overloads were selected nondeterministically");
}

void testTokenSplicingOriginsAndNestedExpansion() {
  RegistryFixture fixture;
  fixture.add("module.zp", R"(
macro inner() { 7 }
macro outer() { inner!() }
macro wrap($x: expr) { ($x + 1) }
macro pass($x: tokens) { $x }
)");
  fixture.resolve();

  const std::string nested = "outer!()";
  zap::DiagnosticEngine nestedDiagnostics(nested, "call.zp");
  zap::MacroExpander nestedExpander(fixture.registry, nestedDiagnostics);
  auto nestedOutput = nestedExpander.expand(
      "module.zp", makeCall(nested, {"outer"}, nestedDiagnostics));
  require(nestedOutput && spellings(*nestedOutput) == "7" &&
              nestedOutput->front().token().expansionOrigin &&
              nestedOutput->front().token().expansionOrigin->parent,
          "template-created invocation was not recursively expanded");

  const std::string wrapped = "wrap!(2)";
  zap::DiagnosticEngine wrappedDiagnostics(wrapped, "call.zp");
  zap::MacroExpander wrappedExpander(fixture.registry, wrappedDiagnostics);
  auto wrappedOutput = wrappedExpander.expand(
      "module.zp", makeCall(wrapped, {"wrap"}, wrappedDiagnostics));
  require(wrappedOutput && spellings(*wrappedOutput) == "(2+1)",
          "capture was not spliced into a nested token tree");
  auto flattened = flattenTokenTrees(*wrappedOutput);
  require(!flattened[1].expansionOrigin &&
              flattened[1].span.sourceName == "call.zp" &&
              flattened[2].expansionOrigin &&
              flattened[2].expansionOrigin->definitionSpan.sourceName ==
                  "module.zp" &&
              flattened[2].span.sourceName == "call.zp",
          "capture and template tokens lost their distinct origins");

  const std::string captured = "pass!(inner!())";
  zap::DiagnosticEngine capturedDiagnostics(captured, "call.zp");
  zap::MacroExpander capturedExpander(fixture.registry, capturedDiagnostics);
  auto capturedOutput = capturedExpander.expand(
      "module.zp", makeCall(captured, {"pass"}, capturedDiagnostics));
  require(capturedOutput && spellings(*capturedOutput) == "inner!()",
          "caller capture was expanded in the definition context");
}

void testLimitsAndInvalidTemplates() {
  RegistryFixture fixture;
  fixture.add("module.zp", R"(
macro loop() { loop!() }
macro many() { 1 + 2 }
macro missing() { $unknown }
)");
  fixture.resolve();

  const std::string recursive = "loop!()";
  zap::DiagnosticEngine recursiveDiagnostics(recursive, "call.zp");
  zap::MacroLimits depthLimits;
  depthLimits.maxDepth = 3;
  zap::MacroExpander recursiveExpander(fixture.registry, recursiveDiagnostics,
                                       depthLimits);
  require(
      !recursiveExpander.expand(
          "module.zp", makeCall(recursive, {"loop"}, recursiveDiagnostics)) &&
          recursiveDiagnostics.hadErrors(),
      "recursive expansion exceeded its depth limit without an error");
  require(recursiveDiagnostics.diagnostics().front().code == "M1004" &&
              recursiveDiagnostics.diagnostics().front().span.offset == 0 &&
              recursiveDiagnostics.diagnostics().size() > 1,
          "recursive expansion did not retain a coded invocation trace");

  const std::string many = "many!()";
  zap::DiagnosticEngine manyDiagnostics(many, "call.zp");
  zap::MacroLimits tokenLimits;
  tokenLimits.maxGeneratedTokens = 2;
  zap::MacroExpander manyExpander(fixture.registry, manyDiagnostics,
                                  tokenLimits);
  require(!manyExpander.expand("module.zp",
                               makeCall(many, {"many"}, manyDiagnostics)) &&
              manyDiagnostics.hadErrors() &&
              manyDiagnostics.diagnostics().front().code == "M1004",
          "generated token limit was ignored");

  const std::string missing = "missing!()";
  zap::DiagnosticEngine missingDiagnostics(missing, "call.zp");
  zap::MacroExpander missingExpander(fixture.registry, missingDiagnostics);
  require(!missingExpander.expand("module.zp", makeCall(missing, {"missing"},
                                                        missingDiagnostics)) &&
              missingDiagnostics.hadErrors(),
          "unknown capture did not produce a diagnostic");
}

void testDefinitionScopeAndPerModuleBudgets() {
  RegistryFixture fixture;
  fixture.add("definition.zp",
              "macro helper() { 7 } pub macro outer() { helper!() }");
  fixture.add("caller.zp", "");
  sema::ResolvedImport imported;
  imported.targetModuleIds.push_back("definition.zp");
  fixture.imports["caller.zp"].push_back(std::move(imported));
  fixture.resolve();
  require(fixture.errors.empty(), "valid imported macro was rejected");

  const std::string source = "definition.outer!()";
  zap::DiagnosticEngine diagnostics(source, "call.zp");
  zap::MacroExpander expander(fixture.registry, diagnostics);
  auto expanded = expander.expand(
      "caller.zp", makeCall(source, {"definition", "outer"}, diagnostics));
  require(
      expanded && spellings(*expanded) == "7" &&
          !fixture.registry.findQualified("caller.zp", "definition", "helper"),
      "template-created call did not resolve a private helper in its "
      "definition module");

  RegistryFixture budgets;
  budgets.add("left.zp", "macro one() { 1 }");
  budgets.add("right.zp", "macro one() { 1 }");
  budgets.resolve();
  const std::string one = "one!()";
  zap::DiagnosticEngine budgetDiagnostics(one, "call.zp");
  zap::MacroLimits limits;
  limits.maxGeneratedTokens = 1;
  zap::MacroExpander budgetExpander(budgets.registry, budgetDiagnostics,
                                    limits);
  const auto call = makeCall(one, {"one"}, budgetDiagnostics);
  require(budgetExpander.expand("left.zp", call).has_value() &&
              budgetExpander.expand("right.zp", call).has_value() &&
              !budgetExpander.expand("left.zp", call).has_value(),
          "generated token budget was not isolated by output module");
}

void testSignatureConflictsAndMatcherLimit() {
  RegistryFixture duplicate;
  duplicate.add("module.zp", "macro same($a: expr) {} "
                             "macro same($b: expr) {}");
  duplicate.resolve();
  require(duplicate.errors.size() == 1,
          "duplicate macro signature was not diagnosed once");

  RegistryFixture duplicatePack;
  duplicatePack.add("module.zp", "macro same($a: expr...) {} "
                                 "macro same($b: expr...) {}");
  duplicatePack.resolve();
  require(duplicatePack.errors.size() == 1,
          "duplicate variadic signature was not diagnosed once");

  RegistryFixture overloads;
  overloads.add("module.zp", "macro choose($x: tokens) { 0 } "
                             "macro choose($x: literal) { 1 }");
  overloads.resolve();
  const std::string source = "choose!(42)";
  zap::DiagnosticEngine diagnostics(source, "call.zp");
  zap::MacroLimits limits;
  limits.maxMatchAttempts = 1;
  zap::MacroExpander expander(overloads.registry, diagnostics, limits);
  require(!expander.expand("module.zp",
                           makeCall(source, {"choose"}, diagnostics)) &&
              diagnostics.hadErrors(),
          "matcher attempt limit was ignored");
}

void testVariadicCapturesAndRanking() {
  RegistryFixture fixture;
  fixture.add("module.zp", R"(
macro echo($items: expr...) { ($items) }
macro tail($head: literal, $items: expr...) { ($items) }
macro choose($x: tokens) { 10 }
macro choose($x: literal...) { 20 }
macro typed($head: literal, $rest: type...) { $head }
macro ambiguous($items: expr...) { 1 }
macro ambiguous($items: type...) { 2 }
)");
  fixture.resolve();
  require(fixture.errors.empty(),
          "variadic and fixed signatures conflicted in the registry");

  for (const auto &[source, expected] :
       std::vector<std::pair<std::string, std::string>>{
           {"echo!()", "()"},
           {"echo!(1)", "(1)"},
           {"echo!(1, 2, 3)", "(1,2,3)"},
           {"echo!(1, 2, 3,)", "(1,2,3)"}}) {
    zap::DiagnosticEngine diagnostics(source, "call.zp");
    zap::MacroExpander expander(fixture.registry, diagnostics);
    auto output =
        expander.expand("module.zp", makeCall(source, {"echo"}, diagnostics));
    require(output && spellings(*output) == expected &&
                !diagnostics.hadErrors(),
            "zero, one, many, or trailing-comma pack splice failed");
    if (source == "echo!(1, 2, 3,)") {
      auto tokens = flattenTokenTrees(*output);
      require(tokens[2].type == TokenType::COMMA &&
                  !tokens[2].expansionOrigin &&
                  tokens[2].span.sourceName == "call.zp",
              "pack separator lost its caller origin");
    }
  }

  for (const auto &[source, expected] :
       std::vector<std::pair<std::string, std::string>>{
           {"tail!(9,)", "()"}, {"tail!(9, 1, 2,)", "(1,2)"}}) {
    zap::DiagnosticEngine diagnostics(source, "call.zp");
    zap::MacroExpander expander(fixture.registry, diagnostics);
    auto output =
        expander.expand("module.zp", makeCall(source, {"tail"}, diagnostics));
    require(output && spellings(*output) == expected &&
                !diagnostics.hadErrors(),
            "fixed-prefix pack lost its elements or separators");
  }

  const std::string fixed = "choose!(7)";
  zap::DiagnosticEngine fixedDiagnostics(fixed, "call.zp");
  zap::MacroExpander fixedExpander(fixture.registry, fixedDiagnostics);
  auto fixedOutput = fixedExpander.expand(
      "module.zp", makeCall(fixed, {"choose"}, fixedDiagnostics));
  require(fixedOutput && spellings(*fixedOutput) == "10",
          "exact fixed-arity overload did not beat a variadic overload");

  const std::string variadic = "choose!(7, 8)";
  zap::DiagnosticEngine variadicDiagnostics(variadic, "call.zp");
  zap::MacroExpander variadicExpander(fixture.registry, variadicDiagnostics);
  auto variadicOutput = variadicExpander.expand(
      "module.zp", makeCall(variadic, {"choose"}, variadicDiagnostics));
  require(variadicOutput && spellings(*variadicOutput) == "20",
          "variadic overload did not match multiple arguments");

  const std::string bad = "typed!(7, Int, 1 +)";
  zap::DiagnosticEngine badDiagnostics(bad, "call.zp");
  zap::MacroExpander badExpander(fixture.registry, badDiagnostics);
  require(!badExpander.expand("module.zp",
                              makeCall(bad, {"typed"}, badDiagnostics)) &&
              badDiagnostics.hadErrors() &&
              badDiagnostics.diagnostics().front().message.find("Argument 3") !=
                  std::string::npos,
          "invalid pack element was accepted or misidentified");

  const std::string ambiguous = "ambiguous!(Name)";
  zap::DiagnosticEngine ambiguityDiagnostics(ambiguous, "call.zp");
  zap::MacroExpander ambiguityExpander(fixture.registry, ambiguityDiagnostics);
  require(
      !ambiguityExpander.expand("module.zp", makeCall(ambiguous, {"ambiguous"},
                                                      ambiguityDiagnostics)) &&
          ambiguityDiagnostics.hadErrors(),
      "equal-ranked variadic overloads were not ambiguous");
}

void testVariadicFragmentKinds() {
  const std::vector<std::pair<std::string, std::string>> cases = {
      {"ident", "name, other"},
      {"literal", "1, true"},
      {"expr", "1 + 2, 3"},
      {"type", "Int, Bool"},
      {"stmt", "return 1;, return 2;"},
      {"block", "{ return 1; }, { return 2; }"},
      {"item", "fun first() {}, fun second() {}"},
      {"tokens", "a + b, { c }"},
  };
  for (const auto &[kind, arguments] : cases) {
    RegistryFixture fixture;
    fixture.add("module.zp", "macro pack($items: " + kind + "...) { 1 }");
    fixture.resolve();
    require(fixture.errors.empty(), "variadic fragment fixture is invalid");
    const std::string source = "pack!(" + arguments + ")";
    zap::DiagnosticEngine diagnostics(source, "call.zp");
    zap::MacroExpander expander(fixture.registry, diagnostics);
    auto output =
        expander.expand("module.zp", makeCall(source, {"pack"}, diagnostics));
    require(output && spellings(*output) == "1" && !diagnostics.hadErrors(),
            "a supported variadic fragment kind did not match");
  }
}

void testTemplateForLoops() {
  RegistryFixture fixture;
  fixture.add("module.zp", R"(
macro joined($args: tokens...) {
  for $x in $args separated by { + } { $x }
}
macro indexed($args: tokens...) {
  for ($x, $p) in $args separated by { ; } {
    $p.index : $p.isFirst : $p.isLast : $x
  }
}
macro nested($args: tokens...) {
  for $x in $args separated by { ; } {
    $x for $x in $args separated by { + } { $x } $x
  }
}
macro runtime($args: tokens...) { for value in values { $args } }
macro empty($args: tokens...) { for $x in $args { $x } }
)");
  fixture.resolve();
  require(fixture.errors.empty(), "template loop fixture is invalid");

  const std::vector<std::tuple<std::string, std::string, std::string>> cases = {
      {"joined!(a, b, c,)", "joined", "a+b+c"},
      {"indexed!(a, b)", "indexed", "0:true:false:a;1:false:true:b"},
      {"nested!(a, b)", "nested", "aa+ba;ba+bb"},
      {"runtime!(x)", "runtime", "forvalueinvalues{x}"},
      {"empty!()", "empty", ""},
  };
  for (const auto &[source, name, expected] : cases) {
    zap::DiagnosticEngine diagnostics(source, "call.zp");
    zap::MacroExpander expander(fixture.registry, diagnostics);
    auto output =
        expander.expand("module.zp", makeCall(source, {name}, diagnostics));
    require(output && spellings(*output) == expected &&
                !diagnostics.hadErrors(),
            "template loop expansion, nesting, shadowing, or separator failed");
    if (name == "indexed") {
      require(output->front().token().type == TokenType::INTEGER &&
                  output->front().token().expansionOrigin &&
                  output->front().token().span.sourceName == "call.zp",
              "loop position token lost its origin");
    }
  }
}

void testInvalidTemplateForLoops() {
  RegistryFixture fixture;
  fixture.add("module.zp", R"(
macro scalar($value: tokens) { for $x in $value { $x } }
macro property($args: tokens...) {
  for ($x, $p) in $args { $p.unknown }
}
macro malformed($args: tokens...) {
  for $x in $args separated { $x }
}
macro dormant($args: tokens...) {
  for $x in $args { $missing }
}
macro repeated($args: tokens...) { for $x in $args { $x $x } }
macro no_output($args: tokens...) { for $x in $args {} }
)");
  fixture.resolve();
  require(fixture.errors.empty(), "invalid loop fixture declaration failed");

  for (const auto &[source, name] :
       std::vector<std::pair<std::string, std::string>>{
           {"scalar!(x)", "scalar"},
           {"property!(x)", "property"},
           {"malformed!(x)", "malformed"},
           {"dormant!()", "dormant"}}) {
    zap::DiagnosticEngine diagnostics(source, "call.zp");
    zap::MacroExpander expander(fixture.registry, diagnostics);
    require(
        !expander.expand("module.zp", makeCall(source, {name}, diagnostics)) &&
            diagnostics.hadErrors(),
        "invalid template loop was accepted");
  }

  const std::string source = "repeated!(a, b)";
  zap::DiagnosticEngine diagnostics(source, "call.zp");
  zap::MacroLimits limits;
  limits.maxGeneratedTokens = 3;
  zap::MacroExpander expander(fixture.registry, diagnostics, limits);
  require(!expander.expand("module.zp",
                           makeCall(source, {"repeated"}, diagnostics)) &&
              diagnostics.hadErrors(),
          "template loop bypassed the generated-token limit");

  const std::string emptySource = "no_output!(a, b)";
  zap::DiagnosticEngine iterationDiagnostics(emptySource, "call.zp");
  zap::MacroLimits iterationLimits;
  iterationLimits.maxTemplateIterations = 1;
  zap::MacroExpander iterationExpander(fixture.registry, iterationDiagnostics,
                                       iterationLimits);
  require(!iterationExpander.expand(
              "module.zp",
              makeCall(emptySource, {"no_output"}, iterationDiagnostics)) &&
              iterationDiagnostics.hadErrors() &&
              iterationDiagnostics.diagnostics().front().code == "M1004",
          "empty template iterations bypassed the iteration limit");
}

void testCompileTimeTemplateControl() {
  RegistryFixture fixture;
  fixture.add("module.zp", R"(
macro choose($args: tokens...) {
  when $args.isEmpty { empty } else { nonempty }
}
macro nested_when($args: tokens...) {
  when !$args.isEmpty { when $args.count > 1 { many } else { one } }
}
macro kinds($value: tokens) {
  case $value.kind {
    ident { identifier }
    literal { literal_value }
    else { other }
  }
}
macro count_kind($values: tokens...) {
  $let count = $values.count;
  case $count {
    0 { zero }
    1 { one }
    else { many }
  }
}
macro binding($value: ident) {
  $let name = $value.text;
  $let copy = $name;
  when $copy == "abc" { matched } else { missed }
  when true { $let name = "nested"; $name }
  $name
}
macro introspect($values: ident...) {
  $values.count : $values.first.text : $values.last.text
  for ($value, $position) in $values {
    $value.kind : $value.isIdent : $position.index : $position.isLast
  }
}
macro loop_control($values: ident...) {
  for $value in $values {
    when $value.text == "skip" { $continue; }
    when $value.text == "stop" { $break; }
    $value
  }
}
macro filtered($values: ident...) {
  for $value in $values separated by { + } {
    when $value.text == "skip" { $continue; }
    when $value.text == "stop" { $break; }
    $value
  }
}
macro runtime() { if true { break; continue; } }
macro runtime_case($value: tokens) { case $value { 1 { yes } else { no } } }
macro source($value: tokens) { $let text = sourceText($value); $text }
macro warnings($value: ident) {
  compileWarning($value.span, "warning from macro");
  compileNote("note from macro");
  done
}
macro fresh() {
  $let first = freshIdent("item");
  $let second = freshIdent("item");
  $first $second
}
)");
  fixture.resolve();
  require(fixture.errors.empty(), "compile-time control fixture is invalid");

  const std::vector<std::tuple<std::string, std::string, std::string>> cases = {
      {"choose!()", "choose", "empty"},
      {"choose!(x)", "choose", "nonempty"},
      {"nested_when!(x)", "nested_when", "one"},
      {"nested_when!(x, y)", "nested_when", "many"},
      {"kinds!(name)", "kinds", "identifier"},
      {"kinds!(42)", "kinds", "literal_value"},
      {"kinds!(a + b)", "kinds", "other"},
      {"count_kind!()", "count_kind", "zero"},
      {"count_kind!(a)", "count_kind", "one"},
      {"count_kind!(a, b)", "count_kind", "many"},
      {"binding!(abc)", "binding", "matched\"nested\"\"abc\""},
      {"introspect!(a, b)", "introspect",
       "2:\"a\":\"b\"ident:true:0:falseident:true:1:true"},
      {"loop_control!(a, skip, b, stop, c)", "loop_control", "ab"},
      {"filtered!(a, skip, b, stop, c)", "filtered", "a+b"},
      {"runtime!()", "runtime", "iftrue{break;continue;}"},
      {"runtime_case!(x)", "runtime_case", "casex{1{yes}else{no}}"},
      {"source!(a + b)", "source", "\"a + b\""},
  };
  for (const auto &[source, name, expected] : cases) {
    zap::DiagnosticEngine diagnostics(source, "call.zp");
    zap::MacroExpander expander(fixture.registry, diagnostics);
    auto output =
        expander.expand("module.zp", makeCall(source, {name}, diagnostics));
    if (!output || spellings(*output) != expected || diagnostics.hadErrors()) {
      std::cerr << source << ": got "
                << (output ? spellings(*output) : "<none>") << ", expected "
                << expected << '\n';
      for (const auto &diagnostic : diagnostics.diagnostics())
        std::cerr << diagnostic.message << '\n';
    }
    require(output && spellings(*output) == expected &&
                !diagnostics.hadErrors(),
            "compile-time template control produced unexpected tokens");
  }

  const std::string source = "fresh!()";
  zap::DiagnosticEngine diagnostics(source, "call.zp");
  zap::MacroExpander expander(fixture.registry, diagnostics);
  auto output =
      expander.expand("module.zp", makeCall(source, {"fresh"}, diagnostics));
  require(output && output->size() == 2 && spellings(*output) == "itemitem" &&
              output->front().token().syntaxContext !=
                  output->back().token().syntaxContext &&
              !diagnostics.hadErrors(),
          "freshIdent did not create distinct identifier contexts");

  const std::string warningCall = "warnings!(name)";
  zap::DiagnosticEngine warningDiagnostics(warningCall, "call.zp");
  zap::MacroExpander warningExpander(fixture.registry, warningDiagnostics);
  auto warningOutput = warningExpander.expand(
      "module.zp", makeCall(warningCall, {"warnings"}, warningDiagnostics));
  require(warningOutput && spellings(*warningOutput) == "done" &&
              warningDiagnostics.diagnostics().size() == 3 &&
              warningDiagnostics.diagnostics()[0].level ==
                  zap::DiagnosticLevel::Warning &&
              warningDiagnostics.diagnostics()[0].span.offset ==
                  warningCall.find("name") &&
              warningDiagnostics.diagnostics()[1].level ==
                  zap::DiagnosticLevel::Note &&
              warningDiagnostics.diagnostics()[2].code == "M2002" &&
              !warningDiagnostics.hadErrors(),
          "compile-time warning, note, or explicit source span failed");
}

void testInvalidCompileTimeTemplateControl() {
  RegistryFixture fixture;
  fixture.add("module.zp", R"(
macro bad_identifier($value: ident) { compileError($value.span, "bad identifier"); }
macro bad_break() { $break; }
macro bad_continue() { $continue; }
macro bad_while($values: tokens...) { while $values.isEmpty {} }
macro bad_state() { $var shared = 1; }
macro bad_property($value: ident) { when $value.missing { yes } }
macro bad_let() { $let name = 1; $let name = 2; }
macro bad_fresh() { $let name = freshIdent("not valid"); $name }
macro keyword_fresh() { $let name = freshIdent("if"); $name }
macro grouped_break($values: tokens...) { for $value in $values { { $break; } } }
)");
  fixture.resolve();
  require(fixture.errors.empty(), "invalid-control fixture is invalid");
  for (const auto &[source, name, message] :
       std::vector<std::tuple<std::string, std::string, std::string>>{
           {"bad_identifier!(x)", "bad_identifier", "bad identifier"},
           {"bad_break!()", "bad_break", "compile-time loop"},
           {"bad_continue!()", "bad_continue", "compile-time loop"},
           {"bad_while!()", "bad_while", "while"},
           {"bad_state!()", "bad_state", "Mutable compile-time state"},
           {"bad_property!(x)", "bad_property",
            "Unknown compile-time property"},
           {"bad_let!()", "bad_let", "Duplicate compile-time binding"},
           {"bad_fresh!()", "bad_fresh", "valid identifier"},
           {"keyword_fresh!()", "keyword_fresh", "reserved keyword"},
           {"grouped_break!(x)", "grouped_break", "syntax group"},
       }) {
    zap::DiagnosticEngine diagnostics(source, "call.zp");
    zap::MacroExpander expander(fixture.registry, diagnostics);
    auto output =
        expander.expand("module.zp", makeCall(source, {name}, diagnostics));
    require(!output && diagnostics.hadErrors() &&
                diagnostics.diagnostics().front().message.find(message) !=
                    std::string::npos,
            "invalid compile-time template was not rejected clearly");
  }
}

} // namespace

int main() {
  testTypedCapturesAndOverloadSelection();
  testCustomPatternMatching();
  testInvalidAndAmbiguousCalls();
  testTokenSplicingOriginsAndNestedExpansion();
  testLimitsAndInvalidTemplates();
  testDefinitionScopeAndPerModuleBudgets();
  testSignatureConflictsAndMatcherLimit();
  testVariadicCapturesAndRanking();
  testVariadicFragmentKinds();
  testTemplateForLoops();
  testInvalidTemplateForLoops();
  testCompileTimeTemplateControl();
  testInvalidCompileTimeTemplateControl();
  return 0;
}
