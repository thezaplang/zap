#include "parser.hpp"

namespace zap {

std::unique_ptr<BindingDecl> Parser::parseBindingDecl(BindingKind kind) {
  const TokenType keywordType = kind == BindingKind::CompileTimeConstant
                                    ? TokenType::CONST
                                    : TokenType::VAR;
  Token keyword = eat(keywordType);
  Token name = eat(TokenType::ID);

  std::unique_ptr<TypeNode> type = nullptr;
  if (peek().type == TokenType::COLON) {
    eat(TokenType::COLON);
    type = parseType();
  } else if (kind == BindingKind::Mutable && peek().type != TokenType::ASSIGN) {
    eat(TokenType::COLON);
  }

  std::unique_ptr<ExpressionNode> initializer = nullptr;
  if (peek().type == TokenType::ASSIGN) {
    eat(TokenType::ASSIGN);
    initializer = parseExpression();
  } else if (kind == BindingKind::CompileTimeConstant) {
    eat(TokenType::ASSIGN);
  }

  Token semicolon = eat(TokenType::SEMICOLON);
  auto declaration = _builder.makeBindingDecl(name.value, std::move(type),
                                              std::move(initializer), kind);
  _builder.setSpan(declaration.get(),
                   SourceSpan::merge(keyword.span, semicolon.span));
  return declaration;
}

std::unique_ptr<BindingDecl> Parser::parseForInitBindingDecl() {
  Token varKeyword = eat(TokenType::VAR);
  Token varNameToken = eat(TokenType::ID);
  eat(TokenType::COLON);

  auto typeNode = parseType();

  std::unique_ptr<ExpressionNode> initializer = nullptr;
  SourceSpan endSpan = typeNode ? typeNode->span : varNameToken.span;
  if (peek().type == TokenType::ASSIGN) {
    eat(TokenType::ASSIGN);
    initializer = parseExpression();
    endSpan = initializer->span;
  }

  auto declaration =
      _builder.makeBindingDecl(varNameToken.value, std::move(typeNode),
                               std::move(initializer), BindingKind::Mutable);
  _builder.setSpan(declaration.get(),
                   SourceSpan::merge(varKeyword.span, endSpan));
  return declaration;
}

} // namespace zap
