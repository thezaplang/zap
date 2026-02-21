#include "parser.hpp"
#include <cstdlib>
#include <iostream>

namespace zap {

Parser::Parser(const std::vector<Token> &tokens, DiagnosticEngine &diag)
    : _diag(diag), _tokens(tokens), _pos(0) {}

Parser::~Parser() {}

std::unique_ptr<RootNode> Parser::parse() {
  auto root = _builder.makeRoot();
  while (!isAtEnd()) {
    try {
      if (peek().type == TokenType::FUN) {
        root->addChild(parseFunDecl());
      } else if (peek().type == TokenType::ENUM) {
        root->addChild(parseEnumDecl());
      } else if (peek().type == TokenType::RECORD) {
        root->addChild(parseRecordDecl());
      } else {
        _diag.report(peek().span, DiagnosticLevel::Error,
                     "Unexpected token " + peek().value);
        _pos++; // Consume the unexpected token to avoid loops
        synchronize();
      }
    } catch (const ParseError &e) {
      synchronize();
    }
  }
  return root;
}

std::unique_ptr<FunDecl> Parser::parseFunDecl() {
  Token funKeyword = eat(TokenType::FUN);

  Token funNameToken = eat(TokenType::ID);
  auto funDecl = _builder.makeFunDecl(funNameToken.value);

  eat(TokenType::LPAREN);

  if (peek().type != TokenType::RPAREN) {
    do {
      funDecl->params_.push_back(parseParameter());
    } while (peek().type == TokenType::COMMA &&
             eat(TokenType::COMMA).type == TokenType::COMMA);
  }

  eat(TokenType::RPAREN);

  if (peek().type != TokenType::LBRACE) {
    funDecl->returnType_ = parseType();
  } else {
    funDecl->returnType_ = _builder.makeType("void");
    const auto &nextToken = peek();
    _builder.setSpan(funDecl->returnType_.get(),
                     SourceSpan(nextToken.span.line, nextToken.span.column,
                                nextToken.span.offset, 0));
  }

  eat(TokenType::LBRACE);

  funDecl->body_ = parseBody();

  Token rbraceToken = eat(TokenType::RBRACE);

  _builder.setSpan(funDecl.get(),
                   SourceSpan::merge(funKeyword.span, rbraceToken.span));

  return funDecl;
}

std::unique_ptr<BodyNode> Parser::parseBody() {
  auto body = _builder.makeBody();
  while (!isAtEnd() && peek().type != TokenType::RBRACE) {
    try {
      if (peek().type == TokenType::VAR) {
        body->addStatement(parseVarDecl());
      } else if (peek().type == TokenType::RETURN) {
        body->addStatement(parseReturnStmt());
      } else if (peek().type == TokenType::IF) {
        auto ifNode = parseIf();
        if (peek().type == TokenType::SEMICOLON) {
          eat(TokenType::SEMICOLON);
          body->addStatement(std::move(ifNode));
        } else if (peek().type == TokenType::RBRACE) {
          body->setResult(std::move(ifNode));
        } else {
          body->addStatement(std::move(ifNode));
        }
      } else if (peek().type == TokenType::WHILE) {
        auto whileNode = parseWhile();
        if (peek().type == TokenType::SEMICOLON) {
          eat(TokenType::SEMICOLON);
        }
        body->addStatement(std::move(whileNode));
      } else if (peek().type == TokenType::ID &&
                 peek(1).type == TokenType::ASSIGN) {
        body->addStatement(parseAssign());
      } else {
        auto expr = parseExpression();
        if (peek().type == TokenType::SEMICOLON) {
          eat(TokenType::SEMICOLON);
          body->addStatement(std::move(expr));
        } else if (peek().type == TokenType::RBRACE) {
          body->setResult(std::move(expr));
        } else {
          body->addStatement(std::move(expr));
        }
      }
    } catch (const ParseError &e) {
      synchronize();
    }
  }
  return body;
}

std::unique_ptr<ParameterNode> Parser::parseParameter() {
  Token paramNameToken = eat(TokenType::ID);
  eat(TokenType::COLON);
  auto typeNode = parseType();
  auto paramNode =
      _builder.makeParam(paramNameToken.value, std::move(typeNode));
  _builder.setSpan(paramNode.get(), SourceSpan::merge(paramNameToken.span,
                                                      paramNode->type->span));
  return paramNode;
}

std::unique_ptr<VarDecl> Parser::parseVarDecl() {
  Token varKeyword = eat(TokenType::VAR);
  Token varNameToken = eat(TokenType::ID);

  eat(TokenType::COLON);

  auto typeNode = parseType();

  if (peek().type == TokenType::ASSIGN) {
    eat(TokenType::ASSIGN);
    auto expr = parseExpression();
    Token semicolonToken = eat(TokenType::SEMICOLON);

    auto varDecl = _builder.makeVarDecl(varNameToken.value, std::move(typeNode),
                                        std::move(expr));
    _builder.setSpan(varDecl.get(),
                     SourceSpan::merge(varKeyword.span, semicolonToken.span));
    return varDecl;
  } else {
    Token semicolonToken = eat(TokenType::SEMICOLON);
    auto varDecl =
        _builder.makeVarDecl(varNameToken.value, std::move(typeNode), nullptr);
    _builder.setSpan(varDecl.get(),
                     SourceSpan::merge(varKeyword.span, semicolonToken.span));
    return varDecl;
  }
}

std::unique_ptr<AssignNode> Parser::parseAssign() {
  Token target = eat(TokenType::ID);
  eat(TokenType::ASSIGN);
  auto expr = parseExpression();
  Token semicolonToken = eat(TokenType::SEMICOLON);

  auto node = _builder.makeAssign(target.value, std::move(expr));
  _builder.setSpan(node.get(),
                   SourceSpan::merge(target.span, semicolonToken.span));
  return node;
}

std::unique_ptr<TypeNode> Parser::parseType() {
  if (peek().type == TokenType::SQUARE_LBRACE) {
    Token lbracket = eat(TokenType::SQUARE_LBRACE);
    auto size = parseExpression();
    Token rbracket = eat(TokenType::SQUARE_RBRACE);
    auto baseType = parseType();
    baseType->isArray = true;
    baseType->arraySize = std::move(size);
    _builder.setSpan(baseType.get(),
                     SourceSpan::merge(lbracket.span, baseType->span));
    return baseType;
  }
  Token t = eat(TokenType::ID);
  auto typeNode = _builder.makeType(t.value);
  _builder.setSpan(typeNode.get(), t.span);
  return typeNode;
}

std::unique_ptr<ArrayLiteralNode> Parser::parseArrayLiteral() {
  Token lbrace = eat(TokenType::LBRACE);
  std::vector<std::unique_ptr<ExpressionNode>> elements;
  if (peek().type != TokenType::RBRACE) {
    do {
      elements.push_back(parseExpression());
    } while (peek().type == TokenType::COMMA &&
             eat(TokenType::COMMA).type == TokenType::COMMA);
  }
  Token rbrace = eat(TokenType::RBRACE);
  auto node = _builder.makeArrayLiteral(std::move(elements));
  _builder.setSpan(node.get(), SourceSpan::merge(lbrace.span, rbrace.span));
  return node;
}

std::unique_ptr<IfNode> Parser::parseIf() {
  Token ifKeyword = eat(TokenType::IF);

  bool hasParen = false;
  if (peek().type == TokenType::LPAREN) {
    eat(TokenType::LPAREN);
    hasParen = true;
  }

  auto condition = parseExpression();

  if (hasParen) {
    eat(TokenType::RPAREN);
  }

  eat(TokenType::LBRACE);
  auto thenBody = parseBody();
  eat(TokenType::RBRACE);

  std::unique_ptr<BodyNode> elseBody = nullptr;
  SourceSpan endSpan = _tokens[_pos - 1].span;

  if (peek().type == TokenType::ELSE) {
    eat(TokenType::ELSE);
    if (peek().type == TokenType::IF) {
      auto nestedIf = parseIf();
      elseBody = _builder.makeBody();
      SourceSpan nestedSpan = nestedIf->span;
      elseBody->addStatement(std::move(nestedIf));
      _builder.setSpan(elseBody.get(), nestedSpan);
      endSpan = nestedSpan;
    } else {
      eat(TokenType::LBRACE);
      elseBody = parseBody();
      Token rbrace = eat(TokenType::RBRACE);
      endSpan = rbrace.span;
    }
  }

  auto ifNode = _builder.makeIf(std::move(condition), std::move(thenBody),
                                std::move(elseBody));

  _builder.setSpan(ifNode.get(), SourceSpan::merge(ifKeyword.span, endSpan));
  return ifNode;
}

std::unique_ptr<WhileNode> Parser::parseWhile() {
  Token whileKeyword = eat(TokenType::WHILE);

  bool hasParen = false;
  if (peek().type == TokenType::LPAREN) {
    eat(TokenType::LPAREN);
    hasParen = true;
  }

  auto condition = parseExpression();

  if (hasParen) {
    eat(TokenType::RPAREN);
  }

  eat(TokenType::LBRACE);
  auto body = parseBody();
  Token rbraceToken = eat(TokenType::RBRACE);

  auto whileNode = _builder.makeWhile(std::move(condition), std::move(body));
  _builder.setSpan(whileNode.get(),
                   SourceSpan::merge(whileKeyword.span, rbraceToken.span));
  return whileNode;
}

std::unique_ptr<ReturnNode> Parser::parseReturnStmt() {
  Token returnKeyword = eat(TokenType::RETURN);

  auto expr = parseExpression();

  Token semicolonToken = eat(TokenType::SEMICOLON);

  auto returnNode = _builder.makeReturn(std::move(expr));
  _builder.setSpan(returnNode.get(),
                   SourceSpan::merge(returnKeyword.span, semicolonToken.span));
  return returnNode;
}

std::unique_ptr<ExpressionNode> Parser::parseExpression() {
  return parseBinaryExpression(0);
}

std::unique_ptr<ExpressionNode>
Parser::parseBinaryExpression(int minPrecedence) {
  auto left = parsePrimaryExpression();

  while (true) {
    if (isAtEnd())
      break;
    Token opToken = peek();
    int precedence = getPrecedence(opToken.type);

    if (precedence < minPrecedence) {
      break;
    }

    eat(opToken.type);

    int nextMinPrecedence =
        (opToken.type == TokenType::POW) ? precedence : precedence + 1;
    auto right = parseBinaryExpression(nextMinPrecedence);

    SourceSpan leftSpan = left->span;
    SourceSpan rightSpan = right->span;
    left =
        _builder.makeBinExpr(std::move(left), opToken.value, std::move(right));
    _builder.setSpan(static_cast<BinExpr *>(left.get()),
                     SourceSpan::merge(leftSpan, rightSpan));
  }
  return left;
}

std::unique_ptr<ExpressionNode> Parser::parsePrimaryExpression() {
  Token current = peek();
  if (current.type == TokenType::INTEGER) {
    eat(TokenType::INTEGER);
    auto constInt = _builder.makeConstInt(std::stoi(current.value));
    _builder.setSpan(constInt.get(), current.span);
    return constInt;
  } else if (current.type == TokenType::FLOAT) {
    eat(TokenType::FLOAT);
    auto constFloat = _builder.makeConstFloat(std::stod(current.value));
    _builder.setSpan(constFloat.get(), current.span);
    return constFloat;
  } else if (current.type == TokenType::STRING) {
    eat(TokenType::STRING);
    auto constStr = _builder.makeConstString(current.value);
    _builder.setSpan(constStr.get(), current.span);
    return constStr;
  } else if (current.type == TokenType::BOOL) {
    eat(TokenType::BOOL);
    auto constBool = _builder.makeConstBool(current.value == "true");
    _builder.setSpan(constBool.get(), current.span);
    return constBool;
  } else if (current.type == TokenType::ID) {
    Token idToken = eat(TokenType::ID);
    if (peek().type == TokenType::LPAREN) {
      auto funCall = _builder.makeFunCall(idToken.value);
      eat(TokenType::LPAREN);

      if (peek().type != TokenType::RPAREN) {
        do {
          std::string argName = "";
          if (peek().type == TokenType::ID &&
              peek(1).type == TokenType::ASSIGN) {
            argName = eat(TokenType::ID).value;
            eat(TokenType::ASSIGN);
          }
          auto argValue = parseExpression();
          funCall->params_.push_back(
              std::make_unique<Argument>(argName, std::move(argValue)));
        } while (peek().type == TokenType::COMMA &&
                 eat(TokenType::COMMA).type == TokenType::COMMA);
      }

      Token rparenToken = eat(TokenType::RPAREN);
      _builder.setSpan(funCall.get(),
                       SourceSpan::merge(idToken.span, rparenToken.span));
      return funCall;
    } else {
      auto constId = _builder.makeConstId(idToken.value);
      _builder.setSpan(constId.get(), idToken.span);
      return constId;
    }
  } else if (current.type == TokenType::LPAREN) {
    eat(TokenType::LPAREN);
    auto expr = parseExpression();
    Token rparenToken = eat(TokenType::RPAREN);
    _builder.setSpan(static_cast<ExpressionNode *>(expr.get()),
                     SourceSpan::merge(current.span, rparenToken.span));
    return expr;
  } else if (current.type == TokenType::LBRACE) {
    return parseArrayLiteral();
  } else if (current.type == TokenType::IF) {
    return parseIf();
  }
  _diag.report(current.span, DiagnosticLevel::Error,
               "Expected primary expression, got " + current.value);
  exit(EXIT_FAILURE);
}
int Parser::getPrecedence(TokenType type) {
  switch (type) {
  case TokenType::EQUAL:
  case TokenType::NOTEQUAL:
  case TokenType::LESS:
  case TokenType::LESSEQUAL:
  case TokenType::GREATER:
  case TokenType::GREATEREQUAL:
    return 5;
  case TokenType::PLUS:
  case TokenType::MINUS:
    return 10;
  case TokenType::MULTIPLY:
  case TokenType::DIVIDE:
  case TokenType::MODULO:
    return 20;
  case TokenType::POW:
    return 30;
  default:
    return -1;
  }
}

const Token &Parser::peek(size_t offset) const {
  if (_pos + offset >= _tokens.size()) {
    static const Token dummy(TokenType::SEMICOLON, "", SourceSpan(0, 0, 0, 0));
    return dummy;
  }
  return _tokens[_pos + offset];
}

Token Parser::eat(TokenType expectedType) {
  if (isAtEnd()) {
    _diag.report(peek().span, DiagnosticLevel::Error,
                 "Expected " + tokenTypeToString(expectedType) +
                     " but reached end of file.");
    throw ParseError();
  }
  Token current = _tokens[_pos];
  if (current.type == expectedType) {
    _pos++;
    return current;
  } else {
    _diag.report(current.span, DiagnosticLevel::Error,
                 "Expected " + tokenTypeToString(expectedType) + ", but got '" +
                     current.value + "'");
    throw ParseError();
  }
}

void Parser::synchronize() {
  while (!isAtEnd()) {
    switch (peek().type) {
    case TokenType::SEMICOLON:
      _pos++;
      return;
    case TokenType::FUN:
    case TokenType::ENUM:
    case TokenType::RECORD:
    case TokenType::VAR:
    case TokenType::IF:
    case TokenType::WHILE:
    case TokenType::RETURN:
    case TokenType::RBRACE:
      return;
    default:
      _pos++;
      break;
    }
  }
}

bool Parser::isAtEnd() const { return _pos >= _tokens.size(); }

std::unique_ptr<EnumDecl> Parser::parseEnumDecl() {
  Token enumKeyword = eat(TokenType::ENUM);
  Token enumNameToken = eat(TokenType::ID);

  std::vector<std::string> entries;
  eat(TokenType::LBRACE);

  if (peek().type != TokenType::RBRACE) {
    do {
      Token entryToken = eat(TokenType::ID);
      entries.push_back(entryToken.value);
    } while (peek().type == TokenType::COMMA &&
             eat(TokenType::COMMA).type == TokenType::COMMA);
  }

  Token rbraceToken = eat(TokenType::RBRACE);

  auto enumDecl =
      _builder.makeEnumDecl(enumNameToken.value, std::move(entries));
  _builder.setSpan(enumDecl.get(),
                   SourceSpan::merge(enumKeyword.span, rbraceToken.span));
  return enumDecl;
}

std::unique_ptr<RecordDecl> Parser::parseRecordDecl() {
  Token recordKeyword = eat(TokenType::RECORD);
  Token recordNameToken = eat(TokenType::ID);

  std::vector<std::unique_ptr<ParameterNode>> fields;
  eat(TokenType::LBRACE);

  while (peek().type != TokenType::RBRACE) {
    fields.push_back(parseParameter());
    if (peek().type == TokenType::COMMA ||
        peek().type == TokenType::SEMICOLON) {
      eat(peek().type);
    }
  }

  Token rbraceToken = eat(TokenType::RBRACE);

  auto recordDecl =
      _builder.makeRecordDecl(recordNameToken.value, std::move(fields));
  _builder.setSpan(recordDecl.get(),
                   SourceSpan::merge(recordKeyword.span, rbraceToken.span));
  return recordDecl;
}

} // namespace zap
