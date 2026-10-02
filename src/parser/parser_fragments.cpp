#include "parser.hpp"

#include <utility>

namespace zap {

std::optional<ParsedFragment> Parser::parseFragment(FragmentKind kind) {
  if (isAtEnd() && kind != FragmentKind::StatementList) {
    _diag.report(peek().span, DiagnosticLevel::Error,
                 "Expected a non-empty syntax fragment.");
    return std::nullopt;
  }

  std::optional<ParsedFragment> fragment;
  try {
    switch (kind) {
    case FragmentKind::Expression:
      fragment.emplace(parseExpression());
      break;
    case FragmentKind::Type:
      fragment.emplace(parseType());
      break;
    case FragmentKind::Statement: {
      auto body = parseBody();
      if (body->statements.size() != 1 || body->result) {
        _diag.report(peek().span, DiagnosticLevel::Error,
                     "Expected exactly one statement fragment.");
        return std::nullopt;
      }
      fragment.emplace(StatementFragment{std::move(body->statements.front())});
      break;
    }
    case FragmentKind::StatementList:
      fragment.emplace(parseBody(true));
      break;
    case FragmentKind::Block: {
      const Token opening = eat(TokenType::LBRACE);
      auto body = parseBody();
      const Token closing = eat(TokenType::RBRACE);
      _builder.setSpan(body.get(),
                       SourceSpan::merge(opening.span, closing.span));
      fragment.emplace(std::move(body));
      break;
    }
    case FragmentKind::Item: {
      auto root = parse();
      if (root->children.size() != 1) {
        _diag.report(peek().span, DiagnosticLevel::Error,
                     "Expected exactly one item fragment.");
        return std::nullopt;
      }
      fragment.emplace(ItemFragment{std::move(root->children.front())});
      break;
    }
    }
  } catch (const ParseError &) {
    return std::nullopt;
  }

  if (!isAtEnd()) {
    _diag.report(peek().span, DiagnosticLevel::Error,
                 "Unexpected token after syntax fragment.");
  }
  if (_diag.hadErrors()) {
    return std::nullopt;
  }
  return fragment;
}

} // namespace zap
