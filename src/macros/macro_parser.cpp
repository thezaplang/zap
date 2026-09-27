#include "macro_parser.hpp"
#include "macro_diagnostic_codes.hpp"

#include "../parser/token_cursor.hpp"

#include <string>
#include <unordered_set>
#include <utility>

namespace zap {
namespace {

std::optional<MacroParameterKind> parameterKind(const std::string &name) {
  if (name == "ident")
    return MacroParameterKind::Identifier;
  if (name == "literal")
    return MacroParameterKind::Literal;
  if (name == "expr")
    return MacroParameterKind::Expression;
  if (name == "type")
    return MacroParameterKind::Type;
  if (name == "stmt")
    return MacroParameterKind::Statement;
  if (name == "block")
    return MacroParameterKind::Block;
  if (name == "item")
    return MacroParameterKind::Item;
  if (name == "tokens")
    return MacroParameterKind::Tokens;
  return std::nullopt;
}

class DeclarationParser {
public:
  DeclarationParser(const std::vector<Token> &tokens, size_t begin, size_t end,
                    Visibility visibility, DiagnosticEngine &diagnostics)
      : tokens_(tokens), cursor_(tokens, begin, end), visibility_(visibility),
        diagnostics_(diagnostics) {}

  MacroParseResult parse() {
    const auto keyword = take(TokenType::MACRO);
    const auto name = take(TokenType::ID);
    if (!keyword || !name || !take(TokenType::LPAREN))
      return failure();

    std::vector<MacroParameter> parameters;
    std::unordered_set<std::string> names;
    while (!cursor_.isAtEnd() && cursor_.peek().type != TokenType::RPAREN) {
      const auto dollar = take(TokenType::DOLLAR);
      const auto parameterName = take(TokenType::ID);
      if (!dollar || !parameterName || !take(TokenType::COLON))
        return failure();
      const auto kindToken = take(TokenType::ID);
      if (!kindToken)
        return failure();

      const auto kind = parameterKind(kindToken->value);
      if (!kind) {
        report(kindToken->span, kindToken->value == "source"
                                    ? "Source captures are not supported yet."
                                    : "Unknown macro parameter kind '" +
                                          kindToken->value + "'.");
      }
      if (!names.insert(parameterName->value).second) {
        report(parameterName->span,
               "Duplicate macro parameter '$" + parameterName->value + "'.");
      }
      bool isVariadic = false;
      SourceSpan parameterEnd = kindToken->span;
      if (cursor_.peek().type == TokenType::ELLIPSIS) {
        parameterEnd = cursor_.peek().span;
        cursor_.advance();
        isVariadic = true;
      }
      if (kind) {
        parameters.push_back({*parameterName, *kind,
                              SourceSpan::merge(dollar->span, parameterEnd),
                              isVariadic});
      }

      if (cursor_.peek().type == TokenType::COMMA) {
        cursor_.advance();
        if (isVariadic && cursor_.peek().type != TokenType::RPAREN) {
          report(parameterEnd, "Variadic macro parameter must be last.");
          return failure();
        }
      } else if (cursor_.peek().type != TokenType::RPAREN) {
        report(cursor_.peek().span,
               "Expected ',' or ')' after macro parameter.");
        return failure();
      }
    }

    if (!take(TokenType::RPAREN))
      return failure();
    if (cursor_.peek().type != TokenType::LBRACE) {
      report(cursor_.peek().span, "Expected '{' after macro parameters.");
      return failure();
    }

    auto body = TokenTreeBuilder::buildPrefix(tokens_, cursor_.position(),
                                              cursor_.end(), diagnostics_);
    cursor_.advance(body.nextPosition - cursor_.position());
    if (body.hadDelimiterErrors || body.trees.size() != 1 ||
        !body.trees.front().closing()) {
      return failure();
    }

    TokenTree expansion = std::move(body.trees.front());
    if (invalid_)
      return failure();
    MacroDefinition definition{
        *name, visibility_, std::move(parameters), std::move(expansion),
        SourceSpan::merge(keyword->span, cursor_.previous().span)};
    return {std::move(definition), cursor_.position()};
  }

private:
  std::optional<Token> take(TokenType expected) {
    if (cursor_.peek().type != expected) {
      report(cursor_.peek().span, "Expected " + tokenTypeToString(expected) +
                                      " in macro declaration.");
      return std::nullopt;
    }
    Token token = cursor_.peek();
    cursor_.advance();
    return token;
  }

  void report(const SourceSpan &span, const std::string &message) {
    diagnostics_.report(span, DiagnosticLevel::Error,
                        macro_diagnostic::Declaration, message);
    invalid_ = true;
  }

  MacroParseResult failure() const {
    return {std::nullopt, cursor_.position()};
  }

  const std::vector<Token> &tokens_;
  TokenCursor cursor_;
  Visibility visibility_;
  DiagnosticEngine &diagnostics_;
  bool invalid_ = false;
};

} // namespace

MacroParseResult MacroParser::parse(const std::vector<Token> &tokens,
                                    size_t begin, size_t end,
                                    Visibility visibility,
                                    DiagnosticEngine &diagnostics) {
  return DeclarationParser(tokens, begin, end, visibility, diagnostics).parse();
}

} // namespace zap
