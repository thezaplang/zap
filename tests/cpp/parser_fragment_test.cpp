#include "ast/body_node.hpp"
#include "ast/const/const_int.hpp"
#include "ast/fun_decl.hpp"
#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#include "utils/diagnostics.hpp"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

struct FragmentParse {
  std::optional<zap::ParsedFragment> fragment;
  bool hadErrors;
};

FragmentParse parse(const std::string &source, zap::FragmentKind kind,
                    size_t begin = 0, size_t end = static_cast<size_t>(-1)) {
  zap::DiagnosticEngine diagnostics(source);
  Lexer lexer(diagnostics);
  auto tokens = lexer.tokenize(source);
  if (end == static_cast<size_t>(-1)) {
    end = tokens.size();
  }
  zap::Parser parser(std::move(tokens), diagnostics, begin, end);
  auto fragment = parser.parseFragment(kind);
  return {std::move(fragment), diagnostics.hadErrors()};
}

void testTypedFragments() {
  auto expression = parse("1 + 2", zap::FragmentKind::Expression);
  require(expression.fragment &&
              std::holds_alternative<std::unique_ptr<ExpressionNode>>(
                  *expression.fragment) &&
              !expression.hadErrors,
          "expression fragment was not parsed with its expected type");

  auto type = parse("[]Int", zap::FragmentKind::Type);
  require(
      type.fragment &&
          std::holds_alternative<std::unique_ptr<TypeNode>>(*type.fragment) &&
          !type.hadErrors,
      "type fragment was not parsed with its expected type");

  auto statement = parse("return 42;", zap::FragmentKind::Statement);
  require(
      statement.fragment &&
          std::holds_alternative<zap::StatementFragment>(*statement.fragment) &&
          !statement.hadErrors,
      "statement fragment was not parsed with its expected type");

  auto block = parse("{ var x: Int = 1; x }", zap::FragmentKind::Block);
  require(
      block.fragment &&
          std::holds_alternative<std::unique_ptr<BodyNode>>(*block.fragment) &&
          std::get<std::unique_ptr<BodyNode>>(*block.fragment)
                  ->statements.size() == 1 &&
          std::get<std::unique_ptr<BodyNode>>(*block.fragment)->result &&
          !block.hadErrors,
      "block fragment did not retain its statement and result");

  auto item =
      parse("pub fun answer() Int { return 42; }", zap::FragmentKind::Item);
  require(item.fragment &&
              std::holds_alternative<zap::ItemFragment>(*item.fragment) &&
              dynamic_cast<FunDecl *>(
                  std::get<zap::ItemFragment>(*item.fragment).node.get()) &&
              !item.hadErrors,
          "item fragment did not reuse top-level declaration parsing");
}

void testEntireRangeIsRequired() {
  require(!parse("1 2", zap::FragmentKind::Expression).fragment,
          "expression fragment accepted trailing tokens");
  require(!parse("Int;", zap::FragmentKind::Type).fragment,
          "type fragment accepted trailing tokens");
  require(!parse("return 1; return 2;", zap::FragmentKind::Statement).fragment,
          "statement fragment accepted two statements");
  require(!parse("{ return 1; } trailing", zap::FragmentKind::Block).fragment,
          "block fragment accepted trailing tokens");
  require(!parse("fun a() {} fun b() {}", zap::FragmentKind::Item).fragment,
          "item fragment accepted two declarations");
  require(!parse("", zap::FragmentKind::Expression).fragment,
          "empty expression fragment was accepted");
}

void testTokenSubrange() {
  const std::string source = "ignored 42 trailing";
  auto fragment = parse(source, zap::FragmentKind::Expression, 1, 2);
  require(fragment.fragment &&
              dynamic_cast<ConstInt *>(
                  std::get<std::unique_ptr<ExpressionNode>>(*fragment.fragment)
                      .get()) &&
              !fragment.hadErrors,
          "parser did not limit the expression to the selected token range");
}

void testMalformedFragment() {
  auto fragment = parse("fun broken( { }", zap::FragmentKind::Item);
  require(!fragment.fragment && fragment.hadErrors,
          "malformed item was accepted after error recovery");
}

void testGenericPrefixes() {
  for (const auto &[source, kind, spelling] :
       std::vector<std::tuple<std::string, zap::FragmentKind, std::string>>{
           {"Pair<Pair<Int, Int>, Bool>, Int", zap::FragmentKind::Type,
            "Pair<Pair<Int,Int>,Bool>"},
           {"combine<Int, Int>(1, 2), 3", zap::FragmentKind::Expression,
            "combine<Int,Int>(1,2)"},
           {"1 < 2, 3 > 4", zap::FragmentKind::Expression, "1<2"}}) {
    zap::DiagnosticEngine diagnostics(source);
    Lexer lexer(diagnostics);
    const auto tokens = lexer.tokenize(source);
    zap::Parser parser(tokens, diagnostics);
    auto prefix = parser.parseFragmentPrefix(kind);
    require(
        prefix && !diagnostics.hadErrors() &&
            prefix->tokenCount < tokens.size() &&
            tokens[prefix->tokenCount].type == TokenType::COMMA,
        "fragment prefix stopped at a generic comma instead of a separator");
    std::string actual;
    for (size_t i = 0; i < prefix->tokenCount; ++i)
      actual += tokens[i].spelling;
    require(actual == spelling, "prefix token count does not describe its AST");
  }
}

} // namespace

int main() {
  testTypedFragments();
  testGenericPrefixes();
  testEntireRangeIsRequired();
  testTokenSubrange();
  testMalformedFragment();
  return 0;
}
