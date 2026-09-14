#include "parser.hpp"

namespace zap {

Visibility Parser::parseMemberVisibility() {
  Visibility visibility = Visibility::Private;
  if (peek().type != TokenType::PUB && peek().type != TokenType::PRIV &&
      peek().type != TokenType::PROT) {
    return visibility;
  }

  Token token = eat(peek().type);
  if (token.type == TokenType::PUB) {
    return Visibility::Public;
  }
  if (token.type == TokenType::PROT) {
    return Visibility::Protected;
  }
  return visibility;
}

std::unique_ptr<FunDecl>
Parser::parseMemberMethod(std::vector<AttributeNode> attributes,
                          Visibility visibility, FunctionContext context) {
  auto method = parseFunDecl(false, context);
  method->visibility_ = visibility;
  method->attributes_ = std::move(attributes);
  return method;
}

std::unique_ptr<ExtensionDecl> Parser::parseExtensionDecl() {
  Token extendToken = eat(TokenType::EXTEND);
  auto declaration = _builder.makeExtensionDecl();

  if (peek().type == TokenType::LESS && isTypeStartToken(peek(1).type)) {
    declaration->genericParams_ = parseGenericParameterList();
  }
  declaration->targetType_ = parseType();
  declaration->genericConstraints_ = parseWhereClauses();

  eat(TokenType::LBRACE);
  while (!isAtEnd() && peek().type != TokenType::RBRACE) {
    try {
      auto attributes = parseAttributes();
      Visibility visibility = parseMemberVisibility();
      if (peek().type != TokenType::FUN && peek().type != TokenType::UNSAFE &&
          peek().type != TokenType::STATIC) {
        _diag.report(peek().span, DiagnosticLevel::Error,
                     "Only methods may be declared inside an extension.");
        ++_pos;
        synchronizeExtensionMember();
        continue;
      }

      auto method = parseMemberMethod(std::move(attributes), visibility,
                                      FunctionContext::ExtensionMethod);
      declaration->methods_.push_back(std::move(method));
    } catch (const ParseError &) {
      synchronizeExtensionMember();
    }
  }

  Token rbraceToken = eat(TokenType::RBRACE);
  _builder.setSpan(declaration.get(),
                   SourceSpan::merge(extendToken.span, rbraceToken.span));
  return declaration;
}

std::unique_ptr<BindingDecl> Parser::parseBindingDecl(BindingKind kind) {
  TokenType keywordType = TokenType::VAR;
  if (kind == BindingKind::Immutable) {
    keywordType = TokenType::LET;
  } else if (kind == BindingKind::CompileTimeConstant) {
    keywordType = TokenType::CONST;
  }
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
