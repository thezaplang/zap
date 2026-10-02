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
       {"syntax macro bad() expr {}", "syntax macro bad($x: expr) expr {}",
        "syntax macro bad($x: source...) expr {}",
        "syntax macro bad($x: source, $y: source) expr {}",
        "syntax macro bad($x: source) bogus {}"}) {
    zap::DiagnosticEngine diagnostics(source, "signature.zp");
    Lexer lexer(diagnostics);
    zap::Parser parser(lexer.tokenize(source), diagnostics);
    parser.parse();
    require(diagnostics.hadErrors() && parser.macroDefinitions().empty(),
            "invalid procedural signature was accepted");
  }
}

void testImportsHygieneAndEmitter() {
  Fixture fixture;
  fixture.sources[fixture.entry] = R"(
import "helper.zp" as helper { value, check };
fun hidden() Int { return 1; }
fun main() Int {
    var temporary: Int = 5;
    var result: Int = value!{SELECT ${temporary}};
    check!{};
    check!{};
    if temporary != 5 { return 2; }
    return result;
}
)";
  fixture.sources[fixture.helper] = R"zp(
fun hidden() Int { return 42; }
pub syntax macro value($query: source) expr {
    if query.interpolationCount == 1 {
        return syntaxExpr("hidden()");
    }
    panic("missing interpolation");
}
pub syntax macro check($input: source) stmt {
    return syntaxTokens("var temporary: Int = 9; if temporary != 9 { return 3; }");
}
)zp";
  auto project = fixture.load(true);
  require(project.loaded, "imported procedural macros were not resolved");
  for (const auto &[moduleId, module] : project.modules) {
    zap::DiagnosticEngine diagnostics(module->sourceText, moduleId);
    Lexer lexer(diagnostics);
    zap::frontend::ExpandedSyntaxEmitter emitter(project.macros, diagnostics);
    auto output = emitter.expand(moduleId, lexer.tokenize(module->sourceText));
    require(
        output && !diagnostics.hadErrors(),
        "expanded syntax emitter failed on procedural expr/stmt declarations");
    for (const auto &token : *output)
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
  };
  const Case cases[] = {
      {"panic(\"stopped\");", "expr", "return bad!{};", "M3004"},
      {"while true {}", "expr", "return bad!{};", "M3003"},
      {"return readFile(\"/etc/passwd\");", "expr", "return bad!{};", "M3002"},
      {"return syntaxExpr(\"1 +\");", "expr", "return bad!{};", "M3001"},
      {"return syntaxTokens(\"return +;\");", "stmt", "bad!{}; return 0;",
       "M1005"},
      {"return syntaxTokens(\"1 +\");", "type", "var x: bad!{} = 1; return 0;",
       "M1005"},
      {"return syntaxTokens(\"42\");", "expr", "return bad!{};", "M1005"},
      {"return syntaxItem(\"fun generated() Int { return 0; }\");", "item",
       "return bad!{};", "M1005"},
  };
  for (const auto &test : cases) {
    Fixture fixture;
    fixture.sources[fixture.entry] =
        std::string("syntax macro bad($input: source) ") + test.kind + " { " +
        test.body + " }\nfun main() Int { " + test.use + " }\n";
    auto project = fixture.load();
    const auto *error = findError(project, test.code);
    if (!error) {
      zap::DiagnosticTextFormatter::print(std::cerr, project.diagnostics);
      std::cerr << "case: " << test.body << '\n';
    }
    require(!project.loaded && error && error->span.line == 2 &&
                error->fileName == fixture.entry.string(),
            "procedural macro failure lost its code or call-site location");
  }

  Fixture fixture;
  fixture.sources[fixture.entry] =
      "syntax macro bad($input: source) item { "
      "return syntaxItem(\"import \\\"helper.zp\\\";\"); }\nbad!{}\n";
  auto project = fixture.load();
  require(!project.loaded && findError(project, "M1005"),
          "procedural macro generated a forbidden import");

  fixture.sources[fixture.entry] =
      "import \"helper.zp\" as helper;\n"
      "fun main() Int { return helper.stop!{}; }\n";
  fixture.sources[fixture.helper] =
      "pub syntax macro stop($input: source) expr { panic(\"stopped\"); }\n";
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
      "syntax macro generate($input: source) stmt { "
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

void testNestedContextAndDepth() {
  Fixture fixture;
  fixture.sources[fixture.entry] =
      "syntax macro inner($x: tokens) stmt { return syntaxTokens(\"42\"); }\n"
      "syntax macro outer($x: source) expr { return syntaxExpr(\"inner!(1)\"); "
      "}\n"
      "fun main() Int { return outer!{}; }\n";
  auto project = fixture.load();
  require(!project.loaded && findError(project, "M1005"),
          "nested procedural macro bypassed the expected fragment context");
  const auto &source = fixture.sources[fixture.entry];
  zap::DiagnosticEngine diagnostics(source, fixture.entry.string());
  Lexer lexer(diagnostics);
  zap::frontend::ExpandedSyntaxEmitter emitter(project.macros, diagnostics);
  require(!emitter.expand(fixture.entry.string(), lexer.tokenize(source)),
          "token emitter bypassed nested procedural fragment validation");

  fixture.sources[fixture.entry] = "syntax macro loop($x: tokens) expr { "
                                   "return syntaxExpr(\"loop!(1)\"); }\n"
                                   "fun main() Int { return loop!(1); }\n";
  auto recursive = fixture.load();
  require(!recursive.loaded && findError(recursive, "M1004"),
          "deferred procedural recursion bypassed the macro depth limit");

  fixture.sources[fixture.entry] = "import \"helper.zp\" as helper { outer };\n"
                                   "fun main() Int { return outer!{}; }\n";
  fixture.sources[fixture.helper] =
      "syntax macro inner($x: tokens) expr { return syntaxExpr(\"42\"); }\n"
      "pub syntax macro outer($x: source) expr { return "
      "syntaxExpr(\"inner!(1)\"); }\n";
  require(fixture.load(true).loaded,
          "deferred procedural call lost its definition-site macro lookup");

  fixture.sources[fixture.entry] = "import \"helper.zp\" as helper { outer };\n"
                                   "fun main() Int { outer!(); return 0; }\n";
  fixture.sources[fixture.helper] =
      "fun hidden() Int { return 42; }\n"
      "syntax macro forward($x: tokens) stmt { return x; }\n"
      "pub macro outer() { forward!(var result: Int = hidden();); "
      "if result != 42 { return 4; } }\n";
  require(fixture.load(true).loaded,
          "forwarded CTFE tokens lost their captured definition-site origin");
}

void testExpressionStatementAndTail() {
  Fixture fixture;
  fixture.sources[fixture.entry] = R"(
syntax macro answer($input: source) expr { return syntaxExpr("42"); }
fun tail() Int { answer!{} }
fun main() Int { answer!{}; return tail(); }
)";
  auto project = fixture.load(true);
  require(project.loaded,
          "expression macro was rejected in statement or tail position");
  for (const auto &[moduleId, module] : project.modules) {
    zap::DiagnosticEngine diagnostics(module->sourceText, moduleId);
    Lexer lexer(diagnostics);
    zap::frontend::ExpandedSyntaxEmitter emitter(project.macros, diagnostics);
    require(emitter.expand(moduleId, lexer.tokenize(module->sourceText)) &&
                !diagnostics.hadErrors(),
            "expanded syntax rejected an expression macro in a function body");
  }
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

} // namespace

int main() {
  testSignatures();
  testImportsHygieneAndEmitter();
  testFailures();
  testCachedOutputFreshHygiene();
  testNestedContextAndDepth();
  testExpressionStatementAndTail();
  testCapturedTokenProtocolRoundTrip();
}
