#include "frontend/expanded_syntax.hpp"
#include "frontend/frontend_session.hpp"
#include "frontend/module_outline.hpp"
#include "lexer/lexer.hpp"
#include "macros/macro_expander.hpp"
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

std::string nested(size_t depth) {
  std::string result = "1";
  for (size_t i = 0; i < depth; ++i)
    result = "identity!(" + result + ")";
  return result;
}

zap::frontend::FrontendProject load(const std::string &source) {
  const std::filesystem::path entry = "/tmp/zap-expansion-pipeline-test.zp";
  zap::frontend::FrontendSessionConfig config{
      zap::frontend::RuntimePaths({}, {}, {}, {},
                                  zap::frontend::EnvironmentOverrides::Ignore),
      {}};
  config.includePrelude = false;
  zap::frontend::FrontendSession session(
      config,
      [&](const std::filesystem::path &path) -> std::optional<std::string> {
        return path == entry ? std::optional<std::string>(source)
                             : std::nullopt;
      });
  return session.load(entry);
}

bool hasCode(const std::vector<zap::Diagnostic> &diagnostics,
             const char *code) {
  for (const auto &diagnostic : diagnostics)
    if (diagnostic.code == code &&
        diagnostic.level == zap::DiagnosticLevel::Error)
      return true;
  return false;
}

void testDepthIsNotHygiene() {
  for (size_t depth : {size_t(128), size_t(129), size_t(160)}) {
    const std::string source = "macro identity($x: expr) { $x }\n"
                               "fun main() Int { return " +
                               nested(depth) + "; }";
    auto project = load(source);
    require(project.loaded == (depth <= 128),
            "capture nesting bypassed expansion depth");
    if (depth > 128)
      require(hasCode(project.diagnostics, "M1004") ||
                  hasCode(project.diagnostics, "P1006"),
              "depth overflow did not have a limit diagnostic");
  }
  const std::string source = R"(
macro identity($x: expr) { $x }
macro wrap($x: expr) { identity!($x) }
fun main() Int { return wrap!(identity!(1)); }
)";
  zap::DiagnosticEngine diagnostics(source, "mixed.zp");
  Lexer lexer(diagnostics);
  auto tokens = lexer.tokenize(source);
  std::map<std::string, zap::frontend::ModuleOutline> outlines;
  outlines.emplace("mixed.zp",
                   zap::frontend::ModuleOutline::scan(tokens, diagnostics));
  std::vector<zap::frontend::MacroResolutionError> errors;
  auto registry =
      zap::frontend::MacroRegistrySet::resolve(outlines, {}, errors);
  zap::MacroLimits limits;
  limits.maxDepth = 2;
  zap::MacroExpander expander(registry, diagnostics, limits);
  zap::Parser parser(tokens, diagnostics, &expander, "mixed.zp");
  parser.parse();
  require(
      hasCode(diagnostics.diagnostics(), "M1004"),
      "mixed template/capture recursion was counted as separate depth chains");
}

void testSharedParsedExpansion() {
  const std::string source = R"(
macro add($a: expr, $b: expr) { $a + $b }
macro nested() { add!(1, 2) * 3 }
fun main() Int { return nested!() + add!(1, 2) * 3; }
)";
  auto project = load(source);
  require(project.loaded, "shared expansion fixture failed to parse");
  const auto &tokens =
      project.modules.at(project.entryModuleId)->expandedTokens;
  std::string compact;
  for (const auto &token : tokens) {
    require(token.type != TokenType::MACRO && token.type != TokenType::NOT,
            "unexpanded macro leaked into parser-produced syntax");
    compact += token.spelling;
  }
  require(compact.find("return((1+2)*3)+(1+2)*3;") != std::string::npos,
          "parsed expression boundary was flattened during token emission");
  const auto rendered = zap::frontend::ExpandedSyntaxEmitter::render(
      project.entryModuleId, project.entryModuleId, tokens);
  require(
      zap::frontend::ExpandedSyntaxEmitter::renderProject(project) ==
          "// unbound expansion: diagnostic view, not a hygienic round-trip\n" +
              rendered,
      "emitter did not consume the parser's normalized expansion model");
}

void testItemListsAndSingleCaptures() {
  for (const std::string body : {"fun a() {} fun b() {}", ""}) {
    const std::string source =
        "macro generate($x: tokens) item { return syntaxItem(\"" + body +
        "\"); }\ngenerate!(1)\nfun main() Int { return 0; }";
    auto project = load(source);
    require(project.loaded,
            "procedural item output rejected a declaration list");
    require(zap::frontend::ExpandedSyntaxEmitter::renderProject(project).find(
                "generate !") == std::string::npos,
            "emitter retained an accepted item macro");
  }
  const std::string source = "macro forward($x: item) { $x }\n"
                             "forward!(fun a() {} fun b() {})";
  auto project = load(source);
  require(!project.loaded && hasCode(project.diagnostics, "M1002"),
          "single item capture silently became an item-list capture");
}

void testSyntaxNestingLimits() {
  const std::string parens =
      std::string(4000, '(') + "1" + std::string(4000, ')');
  const std::string unary = std::string(513, '!') + "true";
  const std::string generated =
      std::string(257, '(') + "1" + std::string(257, ')');
  for (const std::string &source :
       {"macro identity($x: expr) { $x }\nfun main() Int { return identity!(" +
            parens + "); }",
        "macro generate($x: tokens) expr { return syntaxExpr(\"" + generated +
            "\"); }\nfun main() Int { return generate!(1); }",
        "macro generate($x: tokens) expr { return syntaxExpr(\"" + unary +
            "\"); }\nfun main() Bool { return generate!(1); }"}) {
    auto project = load(source);
    require(!project.loaded && (hasCode(project.diagnostics, "P1006") ||
                                hasCode(project.diagnostics, "M3003")),
            "source or generated syntax nesting was not rejected");
  }
  std::string chain;
  for (size_t i = 0; i < 100; ++i) {
    chain += "macro m" + std::to_string(i) + "() { ((((((m" +
             std::to_string(i + 1) + "!())))))) }\n";
  }
  chain += "macro m100() { 1 }\nfun main() Int { return m0!(); }";
  auto project = load(chain);
  require(!project.loaded && hasCode(project.diagnostics, "P1006"),
          "child expansion parsers reset the active syntax depth");
  chain.clear();
  for (size_t i = 0; i < 100; ++i) {
    chain += "macro s" + std::to_string(i) + "() { if true { if true { s" +
             std::to_string(i + 1) + "!(); } } }\n";
  }
  chain += "macro s100() { return 1; }\nfun main() Int { s0!(); return 0; }";
  project = load(chain);
  require(!project.loaded && hasCode(project.diagnostics, "P1006"),
          "statement expansions lost the child's nesting diagnostic");
}
} // namespace

int main() {
  testSharedParsedExpansion();
  testDepthIsNotHygiene();
  testItemListsAndSingleCaptures();
  testSyntaxNestingLimits();
}
