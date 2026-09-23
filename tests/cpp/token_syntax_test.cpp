#include "lexer/lexer.hpp"
#include "token/token.hpp"
#include "utils/diagnostics.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

const Token &findToken(const std::vector<Token> &tokens, TokenType type,
                       size_t occurrence = 0) {
  for (const Token &token : tokens) {
    if (token.type == type && occurrence-- == 0) {
      return token;
    }
  }

  std::cerr << "token was not found\n";
  std::exit(1);
}

void testRawSpelling() {
  const std::string source = "1_024 \"line\\n\" \"zażółć\"";
  zap::DiagnosticEngine diagnostics(source, "spelling.zp");
  Lexer lexer(diagnostics);
  const auto tokens = lexer.tokenize(source);

  const Token &integer = findToken(tokens, TokenType::INTEGER);
  require(integer.value == "1024", "integer value was not normalized");
  require(integer.spelling == "1_024", "integer spelling was not preserved");

  const Token &escaped = findToken(tokens, TokenType::STRING);
  require(escaped.value == "line\n", "string escape was not normalized");
  require(escaped.spelling == "\"line\\n\"",
          "string escape spelling was not preserved");

  const Token &utf8 = findToken(tokens, TokenType::STRING, 1);
  require(utf8.value == "zażółć", "UTF-8 string value was not preserved");
  require(utf8.spelling == "\"zażółć\"",
          "UTF-8 string spelling was not preserved");
  require(utf8.span.sourceName == "spelling.zp",
          "token source name was not retained");
}

void testRawSpellingForEscapedCharAndLexicalErrors() {
  const std::string characterSource = "'\\n'";
  zap::DiagnosticEngine characterDiagnostics(characterSource);
  Lexer characterLexer(characterDiagnostics);
  const auto characterTokens = characterLexer.tokenize(characterSource);
  const Token &escapedCharacter =
      findToken(characterTokens, TokenType::CHAR);
  require(escapedCharacter.value == "\n", "char escape was not normalized");
  require(escapedCharacter.spelling == "'\\n'",
          "char escape spelling was not preserved");

  const std::string invalidSource = "1_024 0x";
  zap::DiagnosticEngine invalidDiagnostics(invalidSource);
  Lexer invalidLexer(invalidDiagnostics);
  const auto invalidTokens = invalidLexer.tokenize(invalidSource);
  const Token &integer = findToken(invalidTokens, TokenType::INTEGER);
  require(integer.value == "1024" && integer.spelling == "1_024",
          "tokens before a lexical error were not finalized");
}

void testSyntaxContextAndOrigin() {
  const SourceSpan invocation(3, 7, 42, 4, "call.zp");
  const SourceSpan definition(1, 1, 0, 5, "macro.zp");
  auto origin = std::make_shared<ExpansionOrigin>(
      ExpansionOrigin{invocation, definition, nullptr});
  Token token(TokenType::ID, "generated", definition, "generated", 12,
              origin);

  require(token.syntaxContext == 12, "syntax context was not retained");
  require(token.expansionOrigin == origin,
          "expansion origin was not retained");
  require(token.expansionOrigin->invocationSpan.sourceName == "call.zp" &&
              token.expansionOrigin->definitionSpan.sourceName == "macro.zp",
          "expansion origin spans were not retained");
}

void testCrossSourceSpanMerge() {
  const SourceSpan start(2, 3, 10, 4, "first.zp");
  const SourceSpan foreignEnd(1, 1, 0, 2, "second.zp");
  const SourceSpan merged = SourceSpan::merge(start, foreignEnd);

  require(merged.sourceName == "first.zp" && merged.offset == 10 &&
              merged.length == 4,
          "cross-source span merge did not retain the start span");

  const SourceSpan localEnd(2, 9, 16, 2, "first.zp");
  const SourceSpan localMerged = SourceSpan::merge(start, localEnd);
  require(localMerged.sourceName == "first.zp" && localMerged.offset == 10 &&
              localMerged.length == 8,
          "same-source spans were not merged");

  const SourceSpan precedingEnd(1, 1, 4, 2, "first.zp");
  require(SourceSpan::merge(start, precedingEnd).length == start.length,
          "out-of-order spans did not retain the start span");
}

} // namespace

int main() {
  testRawSpelling();
  testRawSpellingForEscapedCharAndLexicalErrors();
  testSyntaxContextAndOrigin();
  testCrossSourceSpanMerge();
  return 0;
}
