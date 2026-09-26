#include "ast/fun_decl.hpp"
#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#include "token/token_tree.hpp"
#include "utils/diagnostics.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

struct ParseResult {
  std::shared_ptr<std::string> source;
  std::unique_ptr<zap::DiagnosticEngine> diagnostics;
  std::unique_ptr<RootNode> root;
  std::vector<zap::MacroDefinition> macros;
};

ParseResult parse(const std::string &source) {
  auto ownedSource = std::make_shared<std::string>(source);
  auto diagnostics =
      std::make_unique<zap::DiagnosticEngine>(*ownedSource, "macros.zp");
  Lexer lexer(*diagnostics);
  zap::Parser parser(lexer.tokenize(*ownedSource), *diagnostics);
  auto root = parser.parse();
  return {std::move(ownedSource), std::move(diagnostics), std::move(root),
          parser.takeMacroDefinitions()};
}

void testDeclarationAndTemplate() {
  auto result = parse(R"(
pub macro unless($condition: expr, $body: block) {
  if !$condition { $body }
}
fun regular() Int { return 1; }
)");
  require(!result.diagnostics->hadErrors(),
          "valid macro declaration produced diagnostics");
  require(result.macros.size() == 1, "macro declaration was not collected");
  require(result.root->children.size() == 1 &&
              dynamic_cast<FunDecl *>(result.root->children.front().get()),
          "macro template leaked into runtime AST or hid the next declaration");

  const auto &macro = result.macros.front();
  require(macro.name.value == "unless" &&
              macro.visibility == Visibility::Public,
          "macro name or visibility was not retained");
  require(macro.parameters.size() == 2 &&
              macro.parameters[0].name.value == "condition" &&
              macro.parameters[0].kind == zap::MacroParameterKind::Expression &&
              macro.parameters[1].kind == zap::MacroParameterKind::Block,
          "typed fixed-arity parameters were not retained");
  require(macro.expansion.delimiter() == Delimiter::Brace &&
              macro.expansion.closing().has_value() &&
              macro.expansion.span().length > 2,
          "macro template was not stored as a complete token tree");
  const auto flattened = flattenTokenTrees({macro.expansion});
  require(flattened.size() > 6 && flattened.front().spelling == "{" &&
              flattened.back().spelling == "}",
          "macro token tree did not preserve template delimiters");
}

void testScalarParameterKinds() {
  auto result = parse(R"(macro kinds($a: ident, $b: literal, $c: type,
    $d: stmt, $e: item, $f: tokens) { "$not_a_capture" /* } */ ($a) })");
  require(!result.diagnostics->hadErrors() && result.macros.size() == 1,
          "supported scalar parameter kinds were rejected");
  const auto &parameters = result.macros.front().parameters;
  require(parameters.size() == 6 &&
              parameters[0].kind == zap::MacroParameterKind::Identifier &&
              parameters[1].kind == zap::MacroParameterKind::Literal &&
              parameters[2].kind == zap::MacroParameterKind::Type &&
              parameters[3].kind == zap::MacroParameterKind::Statement &&
              parameters[4].kind == zap::MacroParameterKind::Item &&
              parameters[5].kind == zap::MacroParameterKind::Tokens,
          "scalar parameter kind mapping is incorrect");
  require(result.macros.front().expansion.children().size() == 2,
          "string or comment delimiter changed template grouping");
}

void testConsecutiveDeclarations() {
  auto result = parse(R"(
macro first() { {} }
priv macro second($value: literal) { $value }
fun regular() Int { return 1; }
)");
  require(!result.diagnostics->hadErrors() && result.macros.size() == 2 &&
              result.root->children.size() == 1,
          "consecutive macro templates swallowed the next declaration");
  require(result.macros[0].name.value == "first" &&
              result.macros[0].parameters.empty() &&
              result.macros[1].name.value == "second" &&
              result.macros[1].visibility == Visibility::Private,
          "empty parameters or private macro visibility were not retained");
}

void testInvalidParameters() {
  const std::vector<std::string> invalid = {
      "macro duplicate($x: expr, $x: type) {}",
      "macro unknown($x: bogus) {}",
      "macro source($x: source) {}",
      "macro pack($x: expr...) {}",
      "macro missing($x expr) {}",
  };
  for (const auto &source : invalid) {
    auto result = parse(source);
    require(result.diagnostics->hadErrors() && result.macros.empty(),
            "invalid macro parameter was accepted");
  }
}

void testMalformedTemplate() {
  auto result = parse("macro broken($x: expr) { ($x] }");
  require(result.diagnostics->hadErrors() && result.macros.empty(),
          "mismatched template delimiter was accepted");
}

} // namespace

int main() {
  testDeclarationAndTemplate();
  testScalarParameterKinds();
  testConsecutiveDeclarations();
  testInvalidParameters();
  testMalformedTemplate();
  return 0;
}
