#include "lexer/lexer.hpp"
#include "token/token_tree.hpp"
#include "utils/diagnostics.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

struct TreeResult {
  std::vector<Token> tokens;
  TokenTreeResult trees;
  zap::DiagnosticEngine diagnostics;

  explicit TreeResult(const std::string &source)
      : diagnostics(source, "token_tree_test.zp") {
    Lexer lexer(diagnostics);
    tokens = lexer.tokenize(source);
    trees = TokenTreeBuilder::build(tokens, diagnostics);
  }
};

void requireFlattened(const TreeResult &result, const char *message) {
  const auto flattened = flattenTokenTrees(result.trees.trees);
  require(flattened.size() == result.tokens.size(), message);
  for (size_t index = 0; index < flattened.size(); ++index) {
    require(flattened[index].type == result.tokens[index].type &&
                flattened[index].spelling == result.tokens[index].spelling &&
                flattened[index].span.offset == result.tokens[index].span.offset,
            message);
  }
}

void testNestedGroupsAndSpan() {
  TreeResult result("outer({[inner]})");
  require(!result.trees.hadDelimiterErrors,
          "balanced groups produced delimiter diagnostics");
  require(result.trees.trees.size() == 2,
          "top-level token trees were not preserved");

  const TokenTree &paren = result.trees.trees[1];
  require(!paren.isLeaf() && paren.delimiter() == Delimiter::Parenthesis,
          "parenthesized group was not built");
  require(paren.children().size() == 1,
          "parenthesized group did not retain its child");
  const TokenTree &brace = paren.children().front();
  require(brace.delimiter() == Delimiter::Brace && brace.children().size() == 1,
          "brace group was not nested under parenthesis");
  require(brace.children().front().delimiter() == Delimiter::Bracket,
          "bracket group was not nested under brace");
  require(paren.span().offset == 5 &&
              paren.span().length == result.tokens.back().span.offset + 1 - 5,
          "group span did not include both delimiters");
  requireFlattened(result, "flattening changed balanced token spelling");
}

void testEmptyGroupAndLiteralDelimiter() {
  TreeResult result("{} \"}\" /* } */ []");
  require(!result.trees.hadDelimiterErrors,
          "string or comment delimiter produced a group error");
  require(result.trees.trees.size() == 3,
          "empty groups or string token were not preserved");
  require(!result.trees.trees[0].isLeaf() &&
              result.trees.trees[0].children().empty(),
          "empty brace group was not represented");
  require(result.trees.trees[1].isLeaf() &&
              result.trees.trees[1].token().spelling == "\"}\"",
          "string delimiter was not preserved as a leaf");
  require(!result.trees.trees[2].isLeaf() &&
              result.trees.trees[2].children().empty(),
          "empty bracket group was not represented");
  requireFlattened(result, "flattening changed empty groups or literals");
}

void testDelimiterDiagnosticsAreLossless() {
  TreeResult mismatched("(]");
  require(mismatched.trees.hadDelimiterErrors && mismatched.diagnostics.hadErrors(),
          "mismatched delimiter did not produce a diagnostic");
  require(mismatched.trees.trees.size() == 1 &&
              mismatched.trees.trees.front().closing().has_value() &&
              mismatched.trees.trees.front().closing()->type ==
                  TokenType::SQUARE_RBRACE,
          "mismatched closing delimiter was not retained");
  requireFlattened(mismatched, "flattening lost mismatched delimiter");

  TreeResult unterminated("{");
  require(unterminated.trees.hadDelimiterErrors &&
              !unterminated.trees.trees.front().closing().has_value(),
          "unterminated delimiter did not produce an open group");
  requireFlattened(unterminated, "flattening lost unterminated opening delimiter");

  TreeResult unexpected(")");
  require(unexpected.trees.hadDelimiterErrors &&
              unexpected.trees.trees.front().isLeaf(),
          "unexpected closing delimiter was not reported as a leaf");
  requireFlattened(unexpected, "flattening lost unexpected closing delimiter");

  TreeResult outerCloser("([)");
  const TokenTree &outer = outerCloser.trees.trees.front();
  require(outer.closing().has_value() &&
              outer.closing()->type == TokenType::RPAREN &&
              !outer.children().front().closing().has_value(),
          "inner group consumed its outer group's closing delimiter");
  require(outerCloser.diagnostics.diagnostics().size() == 1,
          "nested unterminated group produced redundant diagnostics");
  requireFlattened(outerCloser, "flattening lost an outer closing delimiter");

  TreeResult crossedClosers("([)]");
  require(crossedClosers.trees.trees.size() == 2 &&
              crossedClosers.trees.trees.front().closing()->type ==
                  TokenType::RPAREN &&
              crossedClosers.trees.trees.back().isLeaf() &&
              crossedClosers.trees.trees.back().token().type ==
                  TokenType::SQUARE_RBRACE,
          "crossed closing delimiters were associated with the wrong group");
  requireFlattened(crossedClosers,
                   "flattening lost crossed closing delimiters");
}

} // namespace

int main() {
  testNestedGroupsAndSpan();
  testEmptyGroupAndLiteralDelimiter();
  testDelimiterDiagnosticsAreLossless();
  return 0;
}
