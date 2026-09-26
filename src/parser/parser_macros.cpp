#include "parser.hpp"

#include "macros/macro_expander.hpp"
#include "token/token_tree.hpp"

#include <utility>

namespace zap {

namespace {

bool opensGroup(TokenType type) {
  return type == TokenType::LPAREN || type == TokenType::LBRACE ||
         type == TokenType::SQUARE_LBRACE;
}

const char *fragmentName(FragmentKind kind) {
  return kind == FragmentKind::Type ? "type" : "expression";
}

} // namespace

bool Parser::isMacroInvocationStart() const {
  if (peek().type != TokenType::ID)
    return false;
  size_t offset = 1;
  while (peek(offset).type == TokenType::DOT &&
         peek(offset + 1).type == TokenType::ID) {
    offset += 2;
  }
  return peek(offset).type == TokenType::NOT &&
         opensGroup(peek(offset + 1).type);
}

ParsedFragment Parser::parseMacroInvocation(FragmentKind kind) {
  const Token start = peek();
  std::vector<std::string> path;
  path.push_back(eat(TokenType::ID).value);
  while (peek().type == TokenType::DOT && peek(1).type == TokenType::ID) {
    eat(TokenType::DOT);
    path.push_back(eat(TokenType::ID).value);
  }
  eat(TokenType::NOT);
  const size_t groupStart = _cursor.position();
  auto grouped =
      TokenTreeBuilder::buildPrefix(_tokens, groupStart, _cursor.end(), _diag);
  _cursor.advance(grouped.nextPosition - groupStart);
  if (grouped.hadDelimiterErrors || grouped.trees.size() != 1 ||
      grouped.trees.front().isLeaf() || !grouped.trees.front().closing()) {
    throw ParseError();
  }

  const SourceSpan invocationSpan =
      SourceSpan::merge(start.span, grouped.trees.front().span());
  if (_macroMode == MacroParseMode::ValidateFragmentSyntax) {
    if (kind == FragmentKind::Type) {
      auto placeholder = _builder.makeType("__macro_type_fragment__");
      _builder.setSpan(placeholder.get(), invocationSpan);
      return placeholder;
    }
    auto placeholder = _builder.makeConstId("__macro_expr_fragment__");
    _builder.setSpan(placeholder.get(), invocationSpan);
    return placeholder;
  }
  if (!_macroExpander) {
    _diag.report(invocationSpan, DiagnosticLevel::Error,
                 "Macro invocation requires a resolved macro registry.");
    throw ParseError();
  }

  MacroCall call{std::move(path), std::move(grouped.trees.front()),
                 invocationSpan, start.expansionOrigin};
  auto expanded = _macroExpander->expand(_moduleId, call);
  if (!expanded)
    throw ParseError();

  DiagnosticEngine fragmentDiagnostics(_diag.sourceText(), _diag.sourceName());
  Parser fragmentParser(flattenTokenTrees(*expanded), fragmentDiagnostics,
                        _macroExpander, _moduleId);
  fragmentParser._allowStructLiteral = _allowStructLiteral;
  auto fragment = fragmentParser.parseFragment(kind);
  for (const auto &diagnostic : fragmentDiagnostics.diagnostics()) {
    _diag.report(diagnostic.span, diagnostic.level, diagnostic.code,
                 diagnostic.message);
  }
  if (!fragment) {
    _diag.report(invocationSpan, DiagnosticLevel::Error,
                 "Macro expansion is not a valid " +
                     std::string(fragmentName(kind)) + " fragment.");
    throw ParseError();
  }
  return std::move(*fragment);
}

} // namespace zap
