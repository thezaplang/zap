#include "lexer/lexer.hpp"
#include "lexer/source_group.hpp"
#include "token/source_fragment.hpp"
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

std::vector<Token> lex(const std::string &source,
                       zap::DiagnosticEngine &diagnostics) {
  Lexer lexer(diagnostics);
  return lexer.tokenize(source);
}

void testLosslessSourceAndOffsets() {
  const std::string source =
      "sql!{\n  SELECT \"user}name\", `other` FROM users // } ignored\n"
      "  WHERE id = ${user.id + 1} /* } */ AND x = '@#?'\n"
      "  AND nested = fn({a: [1, 2]}) #~% -- plain text\n};";
  zap::DiagnosticEngine diagnostics(source, "query.zp");
  const auto tokens = lex(source, diagnostics);
  require(!diagnostics.hadErrors() && tokens.size() == 5,
          "foreign SQL punctuation escaped the lazy source group");
  require(tokens[2].type == TokenType::LBRACE &&
              tokens[3].type == TokenType::RBRACE && tokens[2].sourceFragment &&
              tokens[4].type == TokenType::SEMICOLON,
          "source group did not retain its brace tokens or following syntax");
  const auto &fragment = *tokens[2].sourceFragment;
  require(fragment.text ==
              source.substr(source.find('{') + 1,
                            source.rfind('}') - source.find('{') - 1),
          "source group lost whitespace, comments, or foreign characters");
  require(fragment.interpolations.size() == 1 &&
              fragment.interpolations[0].tokens.size() == 5,
          "interpolation was not lexed as normal Zap tokens");
  const auto &interpolation = fragment.interpolations[0];
  require(interpolation.tokens[0].type == TokenType::ID &&
              interpolation.tokens[0].span.offset == source.find("user.id") &&
              interpolation.tokens[0].span.line == 3 &&
              interpolation.tokens[0].span.column == 16,
          "interpolation token span was not mapped to the source file");
  const size_t quoted = source.find("user}name");
  const auto mapped = fragment.spanAt(quoted - tokens[2].span.offset - 1, 4);
  require(mapped.offset == quoted && mapped.line == 2,
          "source fragment offset map was incorrect");
}

void testQuotedInterpolationIsText() {
  const std::string source = "sql!{SELECT '${not_an_expr}', \"${also_text}\"}";
  zap::DiagnosticEngine diagnostics(source, "quotes.zp");
  auto tokens = lex(source, diagnostics);
  require(!diagnostics.hadErrors() && tokens[2].sourceFragment &&
              tokens[2].sourceFragment->interpolations.empty(),
          "quoted interpolation text was treated as Zap code");
}

void testNestedSourceGroupInInterpolation() {
  const std::string source = "sql!{WHERE id = ${wrap!{user.id}}}";
  zap::DiagnosticEngine diagnostics(source, "nested.zp");
  const auto tokens = lex(source, diagnostics);
  require(!diagnostics.hadErrors() && tokens[2].sourceFragment &&
              tokens[2].sourceFragment->interpolations.size() == 1,
          "nested source group invalidated interpolation capture");
  const auto &expression =
      tokens[2].sourceFragment->interpolations.front().tokens;
  require(expression.size() == 4 && expression[2].sourceFragment &&
              expression[2].sourceFragment->text == "user.id" &&
              expression[2].sourceFragment->offsetMap[0].offset ==
                  source.find("user.id"),
          "nested source group offsets were not rebased to the outer file");
}

void testInvalidGroups() {
  for (const char *source :
       {"sql!{SELECT (value] FROM table}", "sql!{SELECT '${unterminated}'",
        "sql!{SELECT ${value", "sql!{SELECT ${value # other}}"}) {
    zap::DiagnosticEngine diagnostics(source, "invalid.zp");
    lex(source, diagnostics);
    require(diagnostics.hadErrors(),
            "malformed source group or interpolation was accepted");
  }
}

void testOrdinaryBlocksStillLexNormally() {
  const std::string source = "fun main() Int { return 1; }";
  zap::DiagnosticEngine diagnostics(source, "plain.zp");
  const auto tokens = lex(source, diagnostics);
  require(!diagnostics.hadErrors(), "ordinary Zap block failed to lex");
  for (const auto &token : tokens)
    require(!token.sourceFragment,
            "ordinary Zap block became a source macro group");
}

void testDecrementIsNotSqlComment() {
  for (const std::string source :
       {"decrement!{value--}", "decrement!{value --}"}) {
    zap::DiagnosticEngine diagnostics(source, "decrement.zp");
    const auto tokens = lex(source, diagnostics);
    require(!diagnostics.hadErrors() && tokens.size() == 4,
            "Zap decrement inside a macro group became a SQL comment");
    auto grouped = TokenTreeBuilder::build(tokens, diagnostics);
    require(!grouped.hadDelimiterErrors && grouped.trees.size() == 3,
            "source macro group did not form a balanced token tree");
    auto materialized =
        zap::materializeSourceGroup(grouped.trees.back(), diagnostics);
    require(materialized && materialized->children().size() == 2 &&
                materialized->children()[1].token().type ==
                    TokenType::DECREMENT,
            "Zap decrement was not recovered during lazy tokenization");
  }
}

} // namespace

int main() {
  testLosslessSourceAndOffsets();
  testQuotedInterpolationIsText();
  testNestedSourceGroupInInterpolation();
  testInvalidGroups();
  testOrdinaryBlocksStillLexNormally();
  testDecrementIsNotSqlComment();
}
