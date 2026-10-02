#include "parser.hpp"

#include "macros/macro_diagnostic_codes.hpp"
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

ctfe::SyntaxContext syntaxContext(FragmentKind kind) {
  switch (kind) {
  case FragmentKind::Expression:
    return ctfe::SyntaxContext::Expression;
  case FragmentKind::Statement:
  case FragmentKind::StatementList:
  case FragmentKind::Block:
    return ctfe::SyntaxContext::Statement;
  case FragmentKind::Type:
    return ctfe::SyntaxContext::Type;
  case FragmentKind::Item:
    return ctfe::SyntaxContext::Item;
  }
  return ctfe::SyntaxContext::Expression;
}

void forwardDiagnostics(const DiagnosticEngine &from, DiagnosticEngine &to) {
  for (const auto &diagnostic : from.diagnostics()) {
    to.report(diagnostic.span, diagnostic.level, diagnostic.code,
              diagnostic.message);
  }
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

bool Parser::isStandaloneMacroInvocation() const {
  if (!isMacroInvocationStart())
    return false;
  size_t offset = 1;
  while (peek(offset).type == TokenType::DOT &&
         peek(offset + 1).type == TokenType::ID)
    offset += 2;
  const size_t groupStart = _cursor.position() + offset + 1;
  DiagnosticEngine scratch(_diag.sourceText(), _diag.sourceName());
  auto grouped = TokenTreeBuilder::buildPrefix(_tokens, groupStart,
                                               _cursor.end(), scratch);
  if (grouped.hadDelimiterErrors || grouped.trees.size() != 1)
    return false;
  const TokenType next = peek(grouped.nextPosition - _cursor.position()).type;
  switch (next) {
  case TokenType::DOT:
  case TokenType::LPAREN:
  case TokenType::SQUARE_LBRACE:
  case TokenType::LBRACE:
  case TokenType::LESS:
  case TokenType::LESSEQUAL:
  case TokenType::GREATER:
  case TokenType::GREATEREQUAL:
  case TokenType::EQUAL:
  case TokenType::NOTEQUAL:
  case TokenType::PLUS:
  case TokenType::MINUS:
  case TokenType::MULTIPLY:
  case TokenType::DIVIDE:
  case TokenType::MODULO:
  case TokenType::POW:
  case TokenType::REFERENCE:
  case TokenType::BIT_OR:
  case TokenType::LSHIFT:
  case TokenType::RSHIFT:
  case TokenType::AND:
  case TokenType::OR:
  case TokenType::CONCAT:
  case TokenType::DOTDOT:
  case TokenType::QUESTION:
  case TokenType::AS:
  case TokenType::IS:
  case TokenType::ASSIGN:
  case TokenType::PLUS_ASSIGN:
  case TokenType::MINUS_ASSIGN:
  case TokenType::STAR_ASSIGN:
  case TokenType::SLASH_ASSIGN:
  case TokenType::PERCENT_ASSIGN:
  case TokenType::AMP_ASSIGN:
  case TokenType::PIPE_ASSIGN:
  case TokenType::CARET_ASSIGN:
  case TokenType::LSHIFT_ASSIGN:
  case TokenType::RSHIFT_ASSIGN:
  case TokenType::INCREMENT:
  case TokenType::DECREMENT:
    return false;
  default:
    return true;
  }
}

MacroCall Parser::readMacroInvocation() {
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
  return MacroCall{std::move(path), std::move(grouped.trees.front()),
                   invocationSpan, start.expansionOrigin};
}

ParsedFragment Parser::parseMacroInvocation(FragmentKind kind) {
  auto call = readMacroInvocation();
  if (_macroMode == MacroParseMode::ValidateFragmentSyntax) {
    if (kind == FragmentKind::Type) {
      auto placeholder = _builder.makeType("__macro_type_fragment__");
      _builder.setSpan(placeholder.get(), call.span);
      return placeholder;
    }
    auto placeholder = _builder.makeConstId("__macro_expr_fragment__");
    _builder.setSpan(placeholder.get(), call.span);
    return placeholder;
  }
  if (!_macroExpander) {
    _diag.report(call.span, DiagnosticLevel::Error,
                 macro_diagnostic::Resolution,
                 "Macro invocation requires a resolved macro registry.");
    throw ParseError();
  }

  auto expanded = _macroExpander->expand(_moduleId, call, syntaxContext(kind));
  if (!expanded)
    throw ParseError();

  DiagnosticEngine fragmentDiagnostics(_diag.sourceText(), _diag.sourceName());
  fragmentDiagnostics.inheritSourcesFrom(_diag);
  Parser fragmentParser(flattenTokenTrees(*expanded), fragmentDiagnostics,
                        _macroExpander, _moduleId);
  fragmentParser._allowStructLiteral = _allowStructLiteral;
  auto fragment = fragmentParser.parseFragment(kind);
  forwardDiagnostics(fragmentDiagnostics, _diag);
  if (!fragment) {
    _diag.report(call.span, DiagnosticLevel::Error, macro_diagnostic::Fragment,
                 "Macro expansion is not a valid " +
                     std::string(fragmentName(kind)) + " fragment.");
    throw ParseError();
  }
  return std::move(*fragment);
}

std::unique_ptr<BodyNode> Parser::parseMacroStatements() {
  auto call = readMacroInvocation();
  const bool hasSemicolon = peek().type == TokenType::SEMICOLON;
  if (hasSemicolon)
    eat(TokenType::SEMICOLON);

  if (_macroMode == MacroParseMode::ValidateFragmentSyntax) {
    auto body = _builder.makeBody();
    auto placeholder = _builder.makeConstId("__macro_stmt_fragment__");
    _builder.setSpan(placeholder.get(), call.span);
    body->addStatement(std::move(placeholder));
    return body;
  }
  if (!_macroExpander) {
    _diag.report(call.span, DiagnosticLevel::Error,
                 macro_diagnostic::Resolution,
                 "Macro invocation requires a resolved macro registry.");
    throw ParseError();
  }
  auto expanded =
      _macroExpander->expand(_moduleId, call, ctfe::SyntaxContext::Statement);
  if (!expanded)
    throw ParseError();

  DiagnosticEngine fragmentDiagnostics(_diag.sourceText(), _diag.sourceName());
  fragmentDiagnostics.inheritSourcesFrom(_diag);
  Parser fragmentParser(flattenTokenTrees(*expanded), fragmentDiagnostics,
                        _macroExpander, _moduleId);
  fragmentParser._allowStructLiteral = _allowStructLiteral;
  auto body = fragmentParser.parseBody(true);
  if (!fragmentParser.isAtEnd()) {
    fragmentDiagnostics.report(fragmentParser.peek().span,
                               DiagnosticLevel::Error,
                               "Unexpected token after macro statements.");
  }
  forwardDiagnostics(fragmentDiagnostics, _diag);
  if (fragmentDiagnostics.hadErrors()) {
    _diag.report(call.span, DiagnosticLevel::Error, macro_diagnostic::Fragment,
                 "Macro expansion is not a valid statement fragment.");
    throw ParseError();
  }
  if (hasSemicolon && body->result)
    body->addStatement(std::move(body->result));
  return body;
}

std::unique_ptr<RootNode> Parser::parseMacroItems() {
  auto call = readMacroInvocation();
  if (peek().type == TokenType::SEMICOLON)
    eat(TokenType::SEMICOLON);

  if (_macroMode == MacroParseMode::ValidateFragmentSyntax) {
    auto root = _builder.makeRoot();
    root->addChild(_builder.makeTypeAliasDecl("__macro_item_fragment__",
                                              _builder.makeType("Int")));
    return root;
  }
  if (!_macroExpander) {
    _diag.report(call.span, DiagnosticLevel::Error,
                 macro_diagnostic::Resolution,
                 "Macro invocation requires a resolved macro registry.");
    throw ParseError();
  }
  auto expanded =
      _macroExpander->expand(_moduleId, call, ctfe::SyntaxContext::Item);
  if (!expanded)
    throw ParseError();

  DiagnosticEngine fragmentDiagnostics(_diag.sourceText(), _diag.sourceName());
  fragmentDiagnostics.inheritSourcesFrom(_diag);
  Parser fragmentParser(flattenTokenTrees(*expanded), fragmentDiagnostics,
                        _macroExpander, _moduleId);
  auto root = fragmentParser.parse();
  if (!fragmentParser.macroDefinitions().empty()) {
    fragmentDiagnostics.report(call.span, DiagnosticLevel::Error,
                               macro_diagnostic::Fragment,
                               "Macro expansion cannot define macros.");
  }
  for (const auto &item : root->children) {
    if (dynamic_cast<const ImportNode *>(item.get())) {
      fragmentDiagnostics.report(item->span, DiagnosticLevel::Error,
                                 macro_diagnostic::Fragment,
                                 "Macro expansion cannot generate imports.");
    }
  }
  forwardDiagnostics(fragmentDiagnostics, _diag);
  if (fragmentDiagnostics.hadErrors()) {
    _diag.report(call.span, DiagnosticLevel::Error, macro_diagnostic::Fragment,
                 "Macro expansion is not a valid item fragment.");
    throw ParseError();
  }
  return root;
}

} // namespace zap
