#include "frontend/expanded_syntax.hpp"
#include "frontend/frontend_session.hpp"
#include "frontend/macro_registry.hpp"
#include "frontend/module_outline.hpp"
#include "lexer/lexer.hpp"
#include "macros/macro_expander.hpp"
#include "macros/syntax_bridge.hpp"
#include "parser/parser.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <string>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

struct Fixture {
  std::filesystem::path entry =
      std::filesystem::weakly_canonical(std::filesystem::path(__FILE__))
          .parent_path()
          .parent_path() /
      "macro_import" / "main.zp";
  std::filesystem::path helper = entry.parent_path() / "helper.zp";
  std::map<std::filesystem::path, std::string> sources;

  zap::frontend::FrontendProject load(bool bind = false) const {
    zap::frontend::FrontendSessionConfig config{
        zap::frontend::RuntimePaths(
            {}, {}, {}, {}, zap::frontend::EnvironmentOverrides::Ignore),
        {}};
    config.includePrelude = false;
    zap::frontend::FrontendSession session(
        config,
        [this](
            const std::filesystem::path &path) -> std::optional<std::string> {
          auto found = sources.find(path);
          return found == sources.end()
                     ? std::nullopt
                     : std::optional<std::string>(found->second);
        });
    auto project = session.load(entry);
    if (project.loaded && bind)
      require(session.bind(project),
              "procedural macro runtime AST did not bind");
    return project;
  }
};

const zap::Diagnostic *findError(const zap::frontend::FrontendProject &project,
                                 const std::string &code) {
  for (const auto &diagnostic : project.diagnostics)
    if (diagnostic.code == code &&
        diagnostic.level == zap::DiagnosticLevel::Error)
      return &diagnostic;
  return nullptr;
}

void testSignatures() {
  for (const auto &source :
       {"macro bad() expr {}", "macro bad($x: expr) expr {}",
        "macro bad($x: source...) expr {}",
        "macro bad($x: source, $y: source) expr {}",
        "macro bad($x: source) bogus {}", "macro bad($x: tokens...) expr {}",
        "macro bad($x: tokens, $y: tokens) expr {}",
        "macro bad($x: tokens) expr;"}) {
    zap::DiagnosticEngine diagnostics(source, "signature.zp");
    Lexer lexer(diagnostics);
    zap::Parser parser(lexer.tokenize(source), diagnostics);
    parser.parse();
    require(diagnostics.hadErrors() && parser.macroDefinitions().empty(),
            "invalid procedural signature was accepted");
  }
}

void testUnifiedDeclarations() {
  const std::string source = R"(
macro template_expr($x: expr) { $x }
macro template_source($x: source) { sourceText($x) }
macro patterned { ($x: literal) { $x } }
pub macro expression($x: source) expr { return syntaxExpr("42"); }
priv macro statement($x: tokens,) stmt { return x; }
macro integer($x: source) type { return syntaxTokens("Int"); }
macro function($x: source) item { return syntaxItem("fun value() Int { return 42; }"); }
)";
  zap::DiagnosticEngine diagnostics(source, "declarations.zp");
  Lexer lexer(diagnostics);
  const auto tokens = lexer.tokenize(source);
  auto outline = zap::frontend::ModuleOutline::scan(tokens, diagnostics);
  zap::Parser parser(tokens, diagnostics);
  parser.parse();
  const auto &definitions = parser.macroDefinitions();
  require(!diagnostics.hadErrors() && definitions.size() == 7 &&
              outline.macros.size() == definitions.size(),
          "parser and outline disagree on unified macro declarations");
  for (size_t index = 0; index < 3; ++index)
    require(!definitions[index].procedural,
            "ordinary template was mistaken for a procedural macro");
  const zap::ProceduralMacroOutput outputs[] = {
      zap::ProceduralMacroOutput::Expression,
      zap::ProceduralMacroOutput::Statement, zap::ProceduralMacroOutput::Type,
      zap::ProceduralMacroOutput::Item};
  for (size_t index = 0; index < 4; ++index) {
    const auto &definition = definitions[index + 3];
    require(definition.procedural &&
                definition.procedural->output == outputs[index] &&
                outline.macros[index + 3].procedural &&
                outline.macros[index + 3].procedural->output == outputs[index],
            "procedural output suffix was not preserved");
    require(definition.span.offset ==
                source.find("macro " + definition.name.value),
            "procedural definition span did not start at macro keyword");
  }
  require(definitions[3].visibility == Visibility::Public &&
              definitions[4].visibility == Visibility::Private,
          "procedural macro visibility was lost");

  const std::string obsolete =
      "syntax macro removed($x: source) expr { return syntaxExpr(\"42\"); }";
  zap::DiagnosticEngine obsoleteDiagnostics(obsolete, "obsolete.zp");
  Lexer obsoleteLexer(obsoleteDiagnostics);
  zap::Parser obsoleteParser(obsoleteLexer.tokenize(obsolete),
                             obsoleteDiagnostics);
  obsoleteParser.parse();
  require(obsoleteDiagnostics.hadErrors(),
          "obsolete syntax keyword declaration was accepted");
}

void testImportsHygieneAndEmitter() {
  Fixture fixture;
  fixture.sources[fixture.entry] = R"(
import "helper.zp" as helper { value, check };
fun hidden() Int { return 1; }
fun syntax() Int { return 5; }
fun main() Int {
    var temporary: Int = 5;
    var result: Int = value!{SELECT ${temporary}};
    check!{};
    check!{};
    if temporary != 5 { return 2; }
    if syntax() != 5 { return 4; }
    return result;
}
)";
  fixture.sources[fixture.helper] = R"zp(
fun hidden() Int { return 42; }
pub macro value($query: source) expr {
    if query.interpolationCount == 1 {
        return syntaxExpr("hidden()");
    }
    panic("missing interpolation");
}
pub macro check($input: source) stmt {
    return syntaxTokens("var temporary: Int = 9; if temporary != 9 { return 3; }");
}
)zp";
  auto project = fixture.load(true);
  if (!project.loaded)
    zap::DiagnosticTextFormatter::print(std::cerr, project.diagnostics);
  require(project.loaded, "imported procedural macros were not resolved");
  for (const auto &[moduleId, module] : project.modules) {
    require(!module->expandedTokens.empty(),
            "parser did not retain expanded procedural syntax");
    for (const auto &token : module->expandedTokens)
      require(token.type != TokenType::MACRO,
              "procedural declaration leaked into expanded syntax");
  }
}

void testFailures() {
  struct Case {
    const char *body;
    const char *kind;
    const char *use;
    const char *code;
    bool definitionError = false;
  };
  const Case cases[] = {
      {"panic(\"stopped\");", "expr", "return bad!{};", "M3004"},
      {"while true {}", "expr", "return bad!{};", "M3003"},
      {"return readFile(\"/etc/passwd\");", "expr", "return bad!{};", "M3002",
       true},
      {"return syntaxExpr(\"1 +\");", "expr", "return bad!{};", "M3001"},
      {"return syntaxTokens(\"return +;\");", "stmt", "bad!{}; return 0;",
       "M1005"},
      {"return syntaxTokens(\"1 +\");", "type", "var x: bad!{} = 1; return 0;",
       "M1005"},
      {"return syntaxTokens(\"42\");", "expr", "return bad!{};", "M3002", true},
      {"return syntaxItem(\"fun generated() Int { return 0; }\");", "item",
       "return bad!{};", "M1005"},
  };
  for (const auto &test : cases) {
    Fixture fixture;
    fixture.sources[fixture.entry] =
        std::string("macro bad($input: source) ") + test.kind + " { " +
        test.body + " }\nfun main() Int { " + test.use + " }\n";
    auto project = fixture.load();
    const auto *error = findError(project, test.code);
    if (!error) {
      zap::DiagnosticTextFormatter::print(std::cerr, project.diagnostics);
      std::cerr << "case: " << test.body << '\n';
    }
    require(!project.loaded && error &&
                error->span.line == (test.definitionError ? 1u : 2u) &&
                error->fileName == fixture.entry.string(),
            "procedural macro failure lost its code or call-site location");
  }

  Fixture fixture;
  fixture.sources[fixture.entry] =
      "macro bad($input: source) item { "
      "return syntaxItem(\"import \\\"helper.zp\\\";\"); }\nbad!{}\n";
  auto project = fixture.load();
  require(!project.loaded && findError(project, "M3001"),
          "procedural macro generated a forbidden import");

  fixture.sources[fixture.entry] =
      "import \"helper.zp\" as helper;\n"
      "fun main() Int { return helper.stop!{}; }\n";
  fixture.sources[fixture.helper] =
      "pub macro stop($input: source) expr { panic(\"stopped\"); }\n";
  auto importedPanic = fixture.load();
  bool definitionNote = false;
  for (const auto &diagnostic : importedPanic.diagnostics)
    definitionNote |= diagnostic.code == "M2002" &&
                      diagnostic.fileName == fixture.helper.string() &&
                      diagnostic.sourceText == fixture.sources[fixture.helper];
  require(findError(importedPanic, "M3004") && definitionNote,
          "procedural panic lost its imported definition trace");
}

void testCachedOutputFreshHygiene() {
  const std::string source =
      "macro generate($input: source) stmt { "
      "return syntaxTokens(\"var temporary: Int = 1;\"); }";
  zap::DiagnosticEngine diagnostics(source, "definition.zp");
  Lexer lexer(diagnostics);
  std::map<std::string, zap::frontend::ModuleOutline> outlines;
  outlines.emplace("definition.zp", zap::frontend::ModuleOutline::scan(
                                        lexer.tokenize(source), diagnostics));
  std::vector<zap::frontend::MacroResolutionError> errors;
  auto registry =
      zap::frontend::MacroRegistrySet::resolve(outlines, {}, errors);
  const std::string callSource = "generate!{}";
  zap::DiagnosticEngine callDiagnostics(callSource, "call.zp");
  Lexer callLexer(callDiagnostics);
  auto trees =
      TokenTreeBuilder::build(callLexer.tokenize(callSource), callDiagnostics)
          .trees;
  zap::MacroCall call{
      {"generate"}, trees.back(), trees.front().span(), nullptr};
  zap::MacroExpander expander(registry, callDiagnostics);
  auto first = expander.expand("definition.zp", call,
                               zap::ctfe::SyntaxContext::Statement);
  auto second = expander.expand("definition.zp", call,
                                zap::ctfe::SyntaxContext::Statement);
  require(first && second && !callDiagnostics.hadErrors() &&
              first->at(1).token().syntaxContext != ROOT_SYNTAX_CONTEXT &&
              first->at(1).token().syntaxContext !=
                  second->at(1).token().syntaxContext,
          "cached procedural output reused a previous expansion hygiene mark");
}

void testMissingValidatedProgram() {
  Fixture fixture;
  fixture.sources[fixture.entry] =
      "macro bad($input: source) expr { return unknown(); }";
  auto project = fixture.load();
  const auto *binding = project.macros.find(project.entryModuleId, "bad");
  require(!project.loaded && binding && binding->size() == 1 &&
              !project.macros.program(*binding->front().definition),
          "invalid macro unexpectedly had a validated CTFE program");
  const std::string source = "bad!{}";
  zap::DiagnosticEngine diagnostics(source, "call.zp");
  Lexer lexer(diagnostics);
  const auto trees =
      TokenTreeBuilder::build(lexer.tokenize(source), diagnostics);
  zap::MacroCall call{
      {"bad"}, trees.trees.back(), trees.trees.front().span(), nullptr};
  zap::MacroExpander expander(project.macros, diagnostics);
  require(!expander.expand(project.entryModuleId, call,
                           zap::ctfe::SyntaxContext::Expression),
          "macro without a validated program was executed");
  require(!diagnostics.empty() &&
              diagnostics.diagnostics().front().code == "M3002" &&
              diagnostics.diagnostics().front().message ==
                  "Procedural macro has no validated CTFE program.",
          "expander fell back to reparsing an invalid macro definition");
}

void testNestedContextAndDepth() {
  Fixture fixture;
  fixture.sources[fixture.entry] =
      "macro inner($x: tokens) stmt { return syntaxTokens(\"42\"); }\n"
      "macro outer($x: source) expr { return syntaxExpr(\"inner!(1)\"); "
      "}\n"
      "fun main() Int { return outer!{}; }\n";
  auto project = fixture.load();
  require(!project.loaded && findError(project, "M1005"),
          "nested procedural macro bypassed the expected fragment context");

  fixture.sources[fixture.entry] = "macro loop($x: tokens) expr { "
                                   "return syntaxExpr(\"loop!(1)\"); }\n"
                                   "fun main() Int { return loop!(1); }\n";
  auto recursive = fixture.load();
  require(!recursive.loaded && findError(recursive, "M1004"),
          "deferred procedural recursion bypassed the macro depth limit");

  fixture.sources[fixture.entry] = "import \"helper.zp\" as helper { outer };\n"
                                   "fun main() Int { return outer!{}; }\n";
  fixture.sources[fixture.helper] =
      "macro inner($x: tokens) expr { return syntaxExpr(\"42\"); }\n"
      "pub macro outer($x: source) expr { return "
      "syntaxExpr(\"inner!(1)\"); }\n";
  require(fixture.load(true).loaded,
          "deferred procedural call lost its definition-site macro lookup");

  fixture.sources[fixture.entry] = "import \"helper.zp\" as helper { outer };\n"
                                   "fun main() Int { outer!(); return 0; }\n";
  fixture.sources[fixture.helper] =
      "fun hidden() Int { return 42; }\n"
      "macro forward($x: tokens) stmt { return x; }\n"
      "pub macro outer() { forward!(var result: Int = hidden();); "
      "if result != 42 { return 4; } }\n";
  require(fixture.load(true).loaded,
          "forwarded CTFE tokens lost their captured definition-site origin");
}

void testExpressionStatementAndTail() {
  Fixture fixture;
  fixture.sources[fixture.entry] = R"(
macro answer($input: source) expr { return syntaxExpr("42"); }
fun tail() Int { answer!{} }
fun main() Int { answer!{}; return tail(); }
)";
  auto project = fixture.load(true);
  require(project.loaded,
          "expression macro was rejected in statement or tail position");
  require(zap::frontend::ExpandedSyntaxEmitter::renderProject(project).find(
              "answer !") == std::string::npos,
          "expanded syntax retained an expression macro in a function body");
}

void testCapturedTokenProtocolRoundTrip() {
  const std::string source = "wrapper!{SELECT ${wrap!{user.id}}}";
  zap::DiagnosticEngine diagnostics(source, "call.zp");
  Lexer lexer(diagnostics);
  auto tokens = lexer.tokenize(source);
  auto origin = std::make_shared<ExpansionOrigin>(ExpansionOrigin{
      tokens.front().span, tokens.front().span, nullptr, "definition.zp", 23});
  for (auto &token : tokens) {
    token.syntaxContext = origin->mark;
    token.expansionOrigin = origin;
    token.span.expansionOrigin = origin;
  }
  auto trees = TokenTreeBuilder::build(tokens, diagnostics);
  auto converted = zap::ctfe_bridge::syntaxTokens(trees.trees);
  require(converted.has_value(),
          "nested source input did not cross the syntax bridge");
  require(!zap::ctfe_bridge::syntaxTokens(trees.trees, 1),
          "syntax bridge copied input beyond its allocation budget");
  zap::ctfe::SyntaxMacroResult result;
  result.output = converted->value;
  auto encoded = zap::ctfe::encodeResult(result);
  require(std::holds_alternative<std::string>(encoded),
          "captured source syntax could not be serialized");
  auto decoded = zap::ctfe::decodeResult(std::get<std::string>(encoded));
  require(std::holds_alternative<zap::ctfe::SyntaxMacroResult>(decoded),
          "captured source syntax could not be decoded");
  const auto &syntax = std::get<zap::ctfe::SyntaxTokens>(
      *std::get<zap::ctfe::SyntaxMacroResult>(decoded).output);
  auto restored = zap::ctfe_bridge::compilerTokens(syntax, tokens.front().span,
                                                   origin, *converted);
  require(restored && restored->front().expansionOrigin == origin &&
              restored->at(2).sourceFragment == tokens.at(2).sourceFragment,
          "wire/cache round trip lost compiler-owned capture origins or source "
          "groups");
}

void testEagerDefinitionsAndInterpolations() {
  Fixture fixture;
  fixture.sources[fixture.entry] = "macro unused($q: source) expr {\n"
                                   "    var number: String = 42;\n"
                                   "    return syntaxExpr(\"0\");\n"
                                   "}\nfun main() Int { return 0; }";
  auto project = fixture.load();
  auto *error = findError(project, "M3002");
  require(
      !project.loaded && error && error->span.line == 2 &&
          error->span.column == 5 && error->fileName == fixture.entry.string(),
      "unused CTFE definition was not checked at its original source location");
  fixture.sources[fixture.entry] =
      "import \"helper\"; fun main() Int { return 0; }";
  fixture.sources[fixture.helper] =
      "macro unused($q: source) expr { return syntaxItem(\"\"); }";
  project = fixture.load();
  error = findError(project, "M3002");
  require(!project.loaded && error &&
              error->fileName == fixture.helper.string(),
          "unused invalid macro in an imported module escaped validation");
  fixture.sources.erase(fixture.helper);
  for (const std::string call :
       {"answer!{${1 + }}", "answer!{${}}", "answer!{${other!{${1 + }}}}"}) {
    fixture.sources[fixture.entry] =
        "macro answer($q: source) expr { return syntaxExpr(\"42\"); }\n"
        "fun main() Int { return " +
        call + "; }";
    project = fixture.load();
    error = findError(project, "M1002");
    require(!project.loaded && error && error->span.line == 2,
            "ignored source interpolation escaped syntax validation");
  }
}

void testModuleHelpers() {
  Fixture fixture;
  fixture.sources[fixture.entry] = R"(
import "helper" as h { build as imported };
fun twice(x: Int) Int { return x * 2; }
@ctfe fun local() SyntaxExpr {
  if twice(21) == 42 { return imported(); }
  return syntaxExpr("0");
}
macro answer($q: source) expr { return local(); }
macro qualified($q: source) expr { return h.build(); }
fun main() Int { return answer!{} + qualified!{} + twice(21); }
)";
  fixture.sources[fixture.helper] = R"(
fun private_number() Int { return 42; }
@ctfe pub fun build() SyntaxExpr {
  if private_number() == 42 { return syntaxExpr("42"); }
  return syntaxExpr("0");
}
)";
  auto project = fixture.load(true);
  require(project.loaded && project.boundRoot,
          "local/qualified/selective CTFE helpers or shared runtime function "
          "failed");
  for (const auto &[moduleId, module] : project.modules) {
    zap::DiagnosticEngine diagnostics(module->sourceText, moduleId);
    require(zap::frontend::ExpandedSyntaxEmitter::render(
                project.entryModuleId, moduleId, module->expandedTokens)
                        .find("@ ctfe") != std::string::npos ||
                moduleId == fixture.entry.string(),
            "expanded output lost a compile-time helper declaration");
  }

  fixture.sources[fixture.entry] = R"(
@ctfe fun capture(q: SyntaxSource) SyntaxExpr { return sourceInterpolation(q, 0); }
macro forward($q: source) expr { return capture(q); }
fun main() Int { var caller: Int = 42; return forward!{SELECT ${caller}}; }
)";
  fixture.sources.erase(fixture.helper);
  project = fixture.load(true);
  require(project.loaded && project.boundRoot,
          "helper lost caller capture hygiene");

  const auto facade = fixture.entry.parent_path().parent_path() /
                      "macro_expanded_imports" / "main.zp";
  fixture.sources[fixture.helper] = R"(
fun private_value() Int { return 42; }
@ctfe pub fun build() SyntaxExpr {
  if private_value() == 42 { return syntaxExpr("42"); }
  return syntaxExpr("0");
}
pub macro imported_macro($q: source) expr { return build(); }
)";
  fixture.sources[facade] = "pub import \"../macro_import/helper\" as h { "
                            "build as answer, imported_macro };";
  fixture.sources[fixture.entry] = R"(
import "../macro_expanded_imports/main.zp" as f { answer, imported_macro };
fun private_value() Int { return 0; }
macro local($q: source) expr { return f.answer(); }
fun main() Int { return local!{} + imported_macro!{}; }
)";
  project = fixture.load(true);
  if (!project.loaded)
    for (const auto &diagnostic : project.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  require(project.loaded && project.boundRoot,
          "reexport or imported macro used the caller's helper environment");
  const auto expanded =
      zap::frontend::ExpandedSyntaxEmitter::renderProject(project);
  require(expanded.find("@ ctfe") == std::string::npos,
          "bound expanded output retained compile-time-only helpers");
  fixture.sources.clear();

  for (const std::string helper :
       {"@ctfe fun bad() Int { return \"wrong\"; }",
        "@ctfe(1) fun bad() Int { return 0; }",
        "@ctfe @ctfe fun bad() Int { return 0; }",
        "@ctfe unsafe fun bad() Int { return 0; }",
        "@ctfe fun bad() Int { if false { readFile(\"bad\"); } return 0; }"}) {
    fixture.sources[fixture.entry] = helper + "\nfun main() Int { return 0; }";
    project = fixture.load();
    require(!project.loaded && findError(project, "M3002"),
            "invalid unused @ctfe helper escaped validation");
  }
  fixture.sources[fixture.entry] = R"(
fun dangerous() Int { if false { readFile("bad"); } return 42; }
macro unused($q: source) expr {
  if dangerous() == 42 { return syntaxExpr("42"); }
  return syntaxExpr("0");
}
fun main() Int { return 0; }
)";
  project = fixture.load();
  require(!project.loaded && findError(project, "M3002"),
          "ordinary helper dependency bypassed eager sandbox validation");
  fixture.sources[fixture.entry] = R"(
fun capture(q: SyntaxSource) SyntaxExpr { return sourceInterpolation(q, 0); }
macro forward($q: source) expr { return capture(q); }
fun main() Int { return 0; }
)";
  project = fixture.load();
  require(!project.loaded && findError(project, "M3002"),
          "Syntax* helper was accepted without @ctfe");
  fixture.sources[fixture.entry] = R"(
macro generate() { @ctfe fun hidden() Int { return "wrong"; } }
generate!()
fun main() Int { return 0; }
)";
  project = fixture.load();
  require(!project.loaded && findError(project, "M1005"),
          "generated @ctfe helper bypassed module-environment validation");
}

void testHelperVisibilityAndRuntimeBoundary() {
  Fixture fixture;
  fixture.sources[fixture.helper] =
      "@ctfe fun hidden() SyntaxExpr { return syntaxExpr(\"42\"); }";
  for (const std::string import :
       {"import \"helper\" as h;", "import \"helper\" as h { hidden };"}) {
    fixture.sources[fixture.entry] =
        import + " macro answer($q: source) expr { return " +
        (import.find("{") == std::string::npos ? "h.hidden()" : "hidden()") +
        "; } fun main() Int { return answer!{}; }";
    auto project = fixture.load();
    require(!project.loaded && findError(project, "M3002"),
            "private imported helper became visible to CTFE");
  }
  fixture.sources[fixture.helper] =
      "@ctfe pub fun build() SyntaxExpr { return syntaxExpr(\"42\"); }";
  for (const std::string use :
       {"h.build();", "build();", "var callback = build;"}) {
    fixture.sources[fixture.entry] =
        "import \"helper\" as h { build }; fun main() Int { " + use +
        " return 0; }";
    auto project = fixture.load();
    require(project.loaded, "valid @ctfe helper failed frontend validation");
    zap::frontend::FrontendSessionConfig config{
        zap::frontend::RuntimePaths(
            {}, {}, {}, {}, zap::frontend::EnvironmentOverrides::Ignore),
        {}};
    config.includePrelude = false;
    zap::frontend::FrontendSession session(config, {});
    require(!session.bind(project), "runtime used a compile-time-only helper");
  }
}

void testHelperCacheDependencies() {
  Fixture fixture;
  fixture.sources[fixture.entry] =
      "import \"helper\" as h; macro answer($q: source) expr { return "
      "h.build(); } fun main() Int { return answer!{}; }";
  fixture.sources[fixture.helper] =
      "@ctfe pub fun build() SyntaxExpr { return syntaxExpr(\"42\"); }";
  auto first = fixture.load();
  require(first.loaded, "helper cache fixture failed");
  const auto *definition =
      first.macros.find(first.entryModuleId, "answer")->front().definition;
  const auto *program = first.macros.program(*definition);
  require(program, "macro was not assigned its verified module program");
  zap::ctfe::SyntaxMacroRequest request;
  request.definitionId = "answer";
  request.invocation = {"caller.zp", 1, 1, 0, 1};
  zap::ctfe::SyntaxSource input;
  input.sourceName = "caller.zp";
  input.offsets.push_back({1, 1, 0});
  request.input = std::move(input);
  zap::ctfe::CtfeInterpreter interpreter;
  auto result = interpreter.execute(*program, "__syntax_macro__", request);
  require(result.output && std::get<zap::ctfe::SyntaxExpr>(*result.output)
                                   .syntax.tokens.front()
                                   .value == "42",
          "prepared helper program did not execute");
  require(interpreter.execute(*program, "__syntax_macro__", request).output &&
              interpreter.cacheHits() == 1,
          "prepared program missed cache");
  fixture.sources[fixture.helper] =
      "@ctfe pub fun build() SyntaxExpr { return syntaxExpr(\"43\"); }";
  auto second = fixture.load();
  require(second.loaded, "changed helper failed");
  definition =
      second.macros.find(second.entryModuleId, "answer")->front().definition;
  result = interpreter.execute(*second.macros.program(*definition),
                               "__syntax_macro__", request);
  require(result.output &&
              std::get<zap::ctfe::SyntaxExpr>(*result.output)
                      .syntax.tokens.front()
                      .value == "43" &&
              interpreter.cacheHits() == 1,
          "changed helper source reused stale cache result");
  zap::ctfe::CtfeLimits limits;
  limits.maxDefinitionTokens = 1;
  result = interpreter.execute(*program, "__syntax_macro__", request, limits);
  require(!result.output && result.diagnostics.front().code == "M3003",
          "prepared program bypassed stricter execution limits");

  const auto left = fixture.entry.parent_path().parent_path() /
                    "macro_expanded_imports" / "left" / "helper.zp";
  const auto right = fixture.entry.parent_path().parent_path() /
                     "macro_expanded_imports" / "right" / "helper.zp";
  fixture.sources[left] = "pub fun number() Int { return 41; }";
  fixture.sources[right] = "pub fun number() Int { return 42; }";
  const std::string helperBody = R"(
@ctfe pub fun build() SyntaxExpr {
  var keep: Int = a.number() + b.number();
  if chosen() == 41 { return syntaxExpr("41"); }
  return syntaxExpr("42");
}
)";
  fixture.sources[fixture.helper] =
      "import \"../macro_expanded_imports/left/helper.zp\" as a { number as "
      "chosen }; import \"../macro_expanded_imports/right/helper.zp\" as b { "
      "number as unused };" +
      helperBody;
  auto third = fixture.load();
  require(third.loaded, "resolved-import cache fixture failed");
  definition =
      third.macros.find(third.entryModuleId, "answer")->front().definition;
  result = interpreter.execute(*third.macros.program(*definition),
                               "__syntax_macro__", request);
  require(result.output && std::get<zap::ctfe::SyntaxExpr>(*result.output)
                                   .syntax.tokens.front()
                                   .value == "41",
          "initial imported helper binding failed");
  fixture.sources[fixture.helper] =
      "import \"../macro_expanded_imports/left/helper.zp\" as a { number as "
      "unused }; import \"../macro_expanded_imports/right/helper.zp\" as b { "
      "number as chosen };" +
      helperBody;
  auto fourth = fixture.load();
  require(fourth.loaded, "changed-import cache fixture failed");
  definition =
      fourth.macros.find(fourth.entryModuleId, "answer")->front().definition;
  result = interpreter.execute(*fourth.macros.program(*definition),
                               "__syntax_macro__", request);
  require(result.output &&
              std::get<zap::ctfe::SyntaxExpr>(*result.output)
                      .syntax.tokens.front()
                      .value == "42" &&
              interpreter.cacheHits() == 1,
          "changed lookup reused a cache result despite identical reachable "
          "function sources");
}

} // namespace

int main() {
  testSignatures();
  testUnifiedDeclarations();
  testImportsHygieneAndEmitter();
  testFailures();
  testCachedOutputFreshHygiene();
  testMissingValidatedProgram();
  testNestedContextAndDepth();
  testExpressionStatementAndTail();
  testCapturedTokenProtocolRoundTrip();
  testEagerDefinitionsAndInterpolations();
  testModuleHelpers();
  testHelperVisibilityAndRuntimeBoundary();
  testHelperCacheDependencies();
}
