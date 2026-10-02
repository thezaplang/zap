#include "../ast/fun_call.hpp"
#include "parser.hpp"

#include <cstdlib>
#include <limits>

namespace zap {

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

std::unique_ptr<ExpressionNode> Parser::parseExpression() {
  return parseFailableExpression();
}

std::unique_ptr<ExpressionNode> Parser::parseFailableExpression() {
  auto expr = parseCastExpression();
  size_t chainDepth = _syntaxDepth;

  while (peek().type == TokenType::OR && peek().value == "or") {
    checkSyntaxDepth(++chainDepth);
    eat(TokenType::OR);

    SourceSpan startSpan = expr->span;

    const bool explicitBinding = peek().type == TokenType::VAR &&
                                 peek(1).type == TokenType::ID &&
                                 peek(2).type == TokenType::LBRACE;
    if (explicitBinding ||
        (peek().type == TokenType::ID && peek().value == "err" &&
         peek(1).type == TokenType::LBRACE)) {
      if (explicitBinding)
        eat(TokenType::VAR);
      Token errToken = eat(TokenType::ID);
      eat(TokenType::LBRACE);
      auto handler = parseBody();
      Token rbraceToken = eat(TokenType::RBRACE);

      auto handled = _builder.makeFailableHandleExpr(
          std::move(expr), errToken.value, std::move(handler));
      handled->errorSyntaxName_ = SyntaxName(errToken);
      _builder.setSpan(handled.get(),
                       SourceSpan::merge(startSpan, rbraceToken.span));
      expr = std::move(handled);
      continue;
    }

    bool oldAllowStructLiteral = _allowStructLiteral;
    _allowStructLiteral = true;

    std::unique_ptr<ExpressionNode> fallback;
    if (peek().type == TokenType::ID && peek(1).type == TokenType::LBRACE) {
      Token typeToken = eat(TokenType::ID);
      auto typeNode = _builder.makeType(typeToken.value);
      typeNode->syntaxName = SyntaxName(typeToken);
      _builder.setSpan(typeNode.get(), typeToken.span);
      fallback = parseStructLiteral(std::move(typeNode));
    } else {
      fallback = parseCastExpression();
    }

    _allowStructLiteral = oldAllowStructLiteral;

    SourceSpan endSpan = fallback->span;
    auto fallbackExpr =
        _builder.makeFallbackExpr(std::move(expr), std::move(fallback));
    _builder.setSpan(fallbackExpr.get(), SourceSpan::merge(startSpan, endSpan));
    expr = std::move(fallbackExpr);
  }

  return expr;
}

std::unique_ptr<ExpressionNode> Parser::parseCastExpression() {
  auto expr = parseTernaryExpression();
  size_t chainDepth = _syntaxDepth;

  while (peek().type == TokenType::AS) {
    checkSyntaxDepth(++chainDepth);
    eat(TokenType::AS);
    auto type = parseType();
    SourceSpan startSpan = expr->span;
    SourceSpan endSpan = type->span;
    auto castExpr = _builder.makeCastExpr(std::move(expr), std::move(type));
    _builder.setSpan(castExpr.get(), SourceSpan::merge(startSpan, endSpan));
    expr = std::move(castExpr);
  }

  return expr;
}

std::unique_ptr<ExpressionNode> Parser::parseTernaryExpression() {
  auto condition = parseRangeExpression();

  if (peek().type != TokenType::QUESTION)
    return condition;

  DepthGuard depth(*this);
  eat(TokenType::QUESTION);
  auto thenExpr = parseTernaryExpression();
  eat(TokenType::COLON);
  auto elseExpr = parseTernaryExpression();

  SourceSpan conditionSpan = condition->span;
  SourceSpan elseSpan = elseExpr->span;
  auto ternary = _builder.makeTernaryExpr(
      std::move(condition), std::move(thenExpr), std::move(elseExpr));
  _builder.setSpan(ternary.get(), SourceSpan::merge(conditionSpan, elseSpan));
  return ternary;
}

std::unique_ptr<ExpressionNode>
Parser::parseBinaryExpression(int minPrecedence) {
  auto left = parseUnaryExpression();
  size_t chainDepth = _syntaxDepth;

  while (true) {
    if (isAtEnd())
      break;
    Token opToken = peek();
    if (opToken.type == TokenType::OR && opToken.value == "or") {
      break;
    }

    int precedence = getPrecedence(opToken.type);

    if (precedence < minPrecedence) {
      break;
    }

    checkSyntaxDepth(++chainDepth);
    eat(opToken.type);

    int nextMinPrecedence = precedence + 1;

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

std::unique_ptr<ExpressionNode> Parser::parseUnaryExpression() {
  DepthGuard depth(*this);
  if (peek().type == TokenType::NOT || peek().type == TokenType::MINUS ||
      peek().type == TokenType::PLUS || peek().type == TokenType::MULTIPLY ||
      peek().type == TokenType::REFERENCE || peek().type == TokenType::CONCAT) {
    Token opToken = eat(peek().type);
    auto expr = parseUnaryExpression();
    SourceSpan endSpan = expr->span;
    auto node = _builder.makeUnaryExpr(opToken.value, std::move(expr));
    _builder.setSpan(node.get(), SourceSpan::merge(opToken.span, endSpan));
    return node;
  }
  return parsePostfixExpression();
}

std::unique_ptr<ExpressionNode> Parser::parsePostfixExpression() {
  auto left = parsePrimaryExpression();
  size_t chainDepth = _syntaxDepth;

  while (true) {
    if (isAtEnd())
      break;
    Token opToken = peek();

    if (opToken.type == TokenType::DOT) {
      eat(TokenType::DOT);
      Token memberToken = eat(TokenType::ID);
      SourceSpan leftSpan = left->span;
      left = _builder.makeMemberAccess(std::move(left), memberToken.value);
      _builder.setSpan(left.get(),
                       SourceSpan::merge(leftSpan, memberToken.span));
    } else if (opToken.type == TokenType::SQUARE_LBRACE) {
      eat(TokenType::SQUARE_LBRACE);
      auto index = parseExpression();
      Token rbracket = eat(TokenType::SQUARE_RBRACE);
      SourceSpan leftSpan = left->span;
      left = _builder.makeIndexAccess(std::move(left), std::move(index));
      _builder.setSpan(left.get(), SourceSpan::merge(leftSpan, rbracket.span));
    } else if (_allowStructLiteral && opToken.type == TokenType::LESS &&
               isGenericStructLiteralStart()) {
      auto typeNode = typeNodeFromQualifiedExpression(left.get());
      if (!typeNode) {
        break;
      }
      typeNode->genericArgs = parseGenericTypeArguments();
      auto structLiteral = parseStructLiteral(std::move(typeNode));
      left = std::move(structLiteral);
    } else if (opToken.type == TokenType::LESS && isGenericCallStart()) {
      auto genericCall = _builder.makeFunCall(std::move(left));
      SourceSpan leftSpan = genericCall->callee_->span;
      genericCall->genericArgs_ = parseGenericTypeArguments();

      eat(TokenType::LPAREN);

      if (peek().type != TokenType::RPAREN) {
        do {
          std::string argName = "";
          SyntaxName argSyntaxName;
          bool argIsRef = false;
          bool argIsSpread = false;
          if (peek().type == TokenType::ID &&
              peek(1).type == TokenType::ASSIGN) {
            Token nameToken = eat(TokenType::ID);
            argName = nameToken.value;
            argSyntaxName = SyntaxName(nameToken);
            eat(TokenType::ASSIGN);
          }
          if (peek().type == TokenType::ELLIPSIS) {
            eat(TokenType::ELLIPSIS);
            argIsSpread = true;
          }
          if (peek().type == TokenType::REF) {
            eat(TokenType::REF);
            argIsRef = true;
          }
          auto argValue = parseExpression();
          genericCall->params_.push_back(
              std::make_unique<Argument>(argName, std::move(argValue), argIsRef,
                                         argIsSpread, argSyntaxName));
        } while (peek().type == TokenType::COMMA &&
                 eat(TokenType::COMMA).type == TokenType::COMMA);
      }

      Token rparenToken = eat(TokenType::RPAREN);
      _builder.setSpan(genericCall.get(),
                       SourceSpan::merge(leftSpan, rparenToken.span));
      left = std::move(genericCall);
    } else if (opToken.type == TokenType::LPAREN) {
      SourceSpan leftSpan = left->span;
      auto funCall = _builder.makeFunCall(std::move(left));
      eat(TokenType::LPAREN);

      if (peek().type != TokenType::RPAREN) {
        do {
          std::string argName = "";
          SyntaxName argSyntaxName;
          bool argIsRef = false;
          bool argIsSpread = false;
          if (peek().type == TokenType::ID &&
              peek(1).type == TokenType::ASSIGN) {
            Token nameToken = eat(TokenType::ID);
            argName = nameToken.value;
            argSyntaxName = SyntaxName(nameToken);
            eat(TokenType::ASSIGN);
          }
          if (peek().type == TokenType::ELLIPSIS) {
            eat(TokenType::ELLIPSIS);
            argIsSpread = true;
          }
          if (peek().type == TokenType::REF) {
            eat(TokenType::REF);
            argIsRef = true;
          }
          auto argValue = parseExpression();
          funCall->params_.push_back(
              std::make_unique<Argument>(argName, std::move(argValue), argIsRef,
                                         argIsSpread, argSyntaxName));
        } while (peek().type == TokenType::COMMA &&
                 eat(TokenType::COMMA).type == TokenType::COMMA);
      }

      Token rparenToken = eat(TokenType::RPAREN);
      _builder.setSpan(funCall.get(),
                       SourceSpan::merge(leftSpan, rparenToken.span));
      left = std::move(funCall);
    } else if (opToken.type == TokenType::QUESTION &&
               isTryPostfixContext(peek(1).type)) {
      eat(TokenType::QUESTION);
      SourceSpan leftSpan = left->span;
      auto tryExpr = _builder.makeTryExpr(std::move(left));
      _builder.setSpan(tryExpr.get(),
                       SourceSpan::merge(leftSpan, opToken.span));
      left = std::move(tryExpr);
    } else if (_allowStructLiteral && opToken.type == TokenType::LBRACE) {
      auto typeNode = typeNodeFromQualifiedExpression(left.get());
      if (!typeNode) {
        break;
      }
      auto structLiteral = parseStructLiteral(std::move(typeNode));
      const ExpressionNode *first = left.get();
      while (auto member = dynamic_cast<const MemberAccessNode *>(first))
        first = member->left_.get();
      if (auto identifier = dynamic_cast<const ConstId *>(first))
        structLiteral->type_->syntaxName = identifier->syntaxName_;
      structLiteral->type_->span = left->span;
      left = std::move(structLiteral);
    } else {
      break;
    }
    checkSyntaxDepth(++chainDepth);
  }
  return left;
}

std::unique_ptr<ExpressionNode> Parser::parsePrimaryExpression() {
  if (isMacroInvocationStart()) {
    auto fragment = parseMacroInvocation(FragmentKind::Expression);
    return std::move(std::get<std::unique_ptr<ExpressionNode>>(fragment));
  }
  Token current = peek();
  if (current.type == TokenType::NEW) {
    Token newToken = eat(TokenType::NEW);
    auto type = parseType();
    auto newExpr = _builder.makeNewExpr(std::move(type));
    eat(TokenType::LPAREN);
    if (peek().type != TokenType::RPAREN) {
      do {
        std::string argName = "";
        SyntaxName argSyntaxName;
        bool argIsRef = false;
        bool argIsSpread = false;
        if (peek().type == TokenType::ID && peek(1).type == TokenType::ASSIGN) {
          Token nameToken = eat(TokenType::ID);
          argName = nameToken.value;
          argSyntaxName = SyntaxName(nameToken);
          eat(TokenType::ASSIGN);
        }
        if (peek().type == TokenType::ELLIPSIS) {
          eat(TokenType::ELLIPSIS);
          argIsSpread = true;
        }
        if (peek().type == TokenType::REF) {
          eat(TokenType::REF);
          argIsRef = true;
        }
        auto argValue = parseExpression();
        newExpr->args_.push_back(
            std::make_unique<Argument>(argName, std::move(argValue), argIsRef,
                                       argIsSpread, argSyntaxName));
      } while (peek().type == TokenType::COMMA &&
               eat(TokenType::COMMA).type == TokenType::COMMA);
    }
    Token rparenToken = eat(TokenType::RPAREN);
    _builder.setSpan(newExpr.get(),
                     SourceSpan::merge(newToken.span, rparenToken.span));
    return newExpr;
  } else if (current.type == TokenType::INTEGER) {
    eat(TokenType::INTEGER);

    try {
      int base = 10;
      std::string parseValue = current.value;
      if (current.value.size() > 2 && current.value[0] == '0') {
        if (current.value[1] == 'x' || current.value[1] == 'X') {
          base = 16;
        } else if (current.value[1] == 'b' || current.value[1] == 'B') {
          base = 2;
          parseValue = current.value.substr(2);
        } else if (current.value[1] == 'o' || current.value[1] == 'O') {
          base = 8;
          parseValue = current.value.substr(2);
        }
      }

      (void)std::stoull(parseValue, nullptr, base);
    } catch (const std::exception &) {
      _diag.report(current.span, DiagnosticLevel::Error,
                   "Invalid integer literal: " + current.value);
      throw ParseError();
    }

    auto constInt = _builder.makeConstInt(current.value);
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
  } else if (current.type == TokenType::CHAR) {
    eat(TokenType::CHAR);
    auto constChar = _builder.makeConstChar(current.value);
    _builder.setSpan(constChar.get(), current.span);
    return constChar;
  } else if (current.type == TokenType::BOOL) {
    eat(TokenType::BOOL);
    auto constBool = _builder.makeConstBool(current.value == "true");
    _builder.setSpan(constBool.get(), current.span);
    return constBool;
  } else if (current.type == TokenType::NULL_LITERAL) {
    eat(TokenType::NULL_LITERAL);
    auto constNull = _builder.makeConstNull();
    _builder.setSpan(constNull.get(), current.span);
    return constNull;
  } else if (current.type == TokenType::ID) {
    Token idToken = eat(TokenType::ID);
    if (_allowStructLiteral && peek().type == TokenType::LESS &&
        isGenericStructLiteralStart()) {
      auto typeNode = _builder.makeType(idToken.value);
      typeNode->syntaxName = SyntaxName(idToken);
      _builder.setSpan(typeNode.get(), idToken.span);
      typeNode->genericArgs = parseGenericTypeArguments();
      if (!typeNode->genericArgs.empty()) {
        _builder.setSpan(typeNode.get(),
                         SourceSpan::merge(idToken.span,
                                           typeNode->genericArgs.back()->span));
      }
      return parseStructLiteral(std::move(typeNode));
    } else if (_allowStructLiteral && peek().type == TokenType::LBRACE) {
      auto typeNode = _builder.makeType(idToken.value);
      typeNode->syntaxName = SyntaxName(idToken);
      _builder.setSpan(typeNode.get(), idToken.span);
      return parseStructLiteral(std::move(typeNode));
    } else {
      auto constId = _builder.makeConstId(idToken.value);
      constId->syntaxName_ = SyntaxName(idToken);
      _builder.setSpan(constId.get(), idToken.span);
      return constId;
    }
  } else if (current.type == TokenType::LPAREN) {
    eat(TokenType::LPAREN);
    bool oldAllow = _allowStructLiteral;
    _allowStructLiteral = true;
    auto expr = parseExpression();
    _allowStructLiteral = oldAllow;
    Token rparenToken = eat(TokenType::RPAREN);
    _builder.setSpan(static_cast<ExpressionNode *>(expr.get()),
                     SourceSpan::merge(current.span, rparenToken.span));
    return expr;
  } else if (current.type == TokenType::LBRACE) {
    if (!_allowStructLiteral) {
      _diag.report(current.span, DiagnosticLevel::Error,
                   "Struct literal not allowed in this context");
      throw ParseError();
    }
    return parseArrayLiteral();
  }
  _diag.report(current.span, DiagnosticLevel::Error,
               "Expected primary expression, got " + current.value);
  throw ParseError();
}
int Parser::getPrecedence(TokenType type) {
  switch (type) {
  case TokenType::OR:
    return 2;
  case TokenType::BIT_OR:
    return 3;
  case TokenType::POW:
    return 4;
  case TokenType::AND:
    return 5;
  case TokenType::REFERENCE:
    return 6;
  case TokenType::EQUAL:
  case TokenType::NOTEQUAL:
  case TokenType::IS:
  case TokenType::LESS:
  case TokenType::LESSEQUAL:
  case TokenType::GREATER:
  case TokenType::GREATEREQUAL:
    return 7;
  case TokenType::LSHIFT:
  case TokenType::RSHIFT:
    return 8;
  case TokenType::PLUS:
  case TokenType::MINUS:
    return 9;
  case TokenType::MULTIPLY:
  case TokenType::DIVIDE:
  case TokenType::MODULO:
    return 10;
  default:
    return -1;
  }
}

std::unique_ptr<StructLiteralNode>
Parser::parseStructLiteral(std::unique_ptr<TypeNode> type) {
  Token lbrace = eat(TokenType::LBRACE);
  std::vector<StructFieldInit> fields;

  if (peek().type != TokenType::RBRACE) {
    do {
      Token fieldName = eat(TokenType::ID);
      eat(TokenType::COLON);
      auto value = parseExpression();
      fields.emplace_back(fieldName.value, std::move(value));

      if (peek().type == TokenType::COMMA ||
          peek().type == TokenType::SEMICOLON) {
        eat(peek().type);
      } else {
        break;
      }
    } while (peek().type != TokenType::RBRACE);
  }

  Token rbrace = eat(TokenType::RBRACE);
  auto literal =
      std::make_unique<StructLiteralNode>(std::move(type), std::move(fields));
  if (literal->type_) {
    _builder.setSpan(literal.get(),
                     SourceSpan::merge(literal->type_->span, rbrace.span));
  } else {
    _builder.setSpan(literal.get(),
                     SourceSpan::merge(lbrace.span, rbrace.span));
  }
  return literal;
}

std::unique_ptr<ExpressionNode> Parser::parseRangeExpression() {
  auto start = parseBinaryExpression(0);
  if (peek().type != TokenType::DOTDOT) {
    return start;
  }

  Token dotDotTok = eat(TokenType::DOTDOT);
  auto end = parseBinaryExpression(0);
  std::unique_ptr<ExpressionNode> step = nullptr;
  SourceSpan eSpan = end->span;

  if (peek().type == TokenType::DOTDOT) {
    eat(TokenType::DOTDOT);
    step = parseBinaryExpression(0);
    eSpan = step->span;
  }

  SourceSpan sSpan = start->span;
  auto range =
      _builder.makeRangeExpr(std::move(start), std::move(end), std::move(step));
  _builder.setSpan(range.get(), SourceSpan::merge(sSpan, eSpan));
  return range;
}

} // namespace zap
