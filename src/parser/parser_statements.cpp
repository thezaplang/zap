#include "../ast/fun_call.hpp"
#include "parser.hpp"

namespace zap {
namespace {
std::string compoundAssignOp(TokenType type) {
  switch (type) {
  case TokenType::PLUS_ASSIGN:
    return "+";
  case TokenType::MINUS_ASSIGN:
    return "-";
  case TokenType::STAR_ASSIGN:
    return "*";
  case TokenType::SLASH_ASSIGN:
    return "/";
  case TokenType::PERCENT_ASSIGN:
    return "%";
  case TokenType::AMP_ASSIGN:
    return "&";
  case TokenType::PIPE_ASSIGN:
    return "|";
  case TokenType::CARET_ASSIGN:
    return "^";
  case TokenType::LSHIFT_ASSIGN:
    return "<<";
  case TokenType::RSHIFT_ASSIGN:
    return ">>";
  default:
    return "";
  }
}

} // namespace

std::unique_ptr<BodyNode> Parser::parseBody(bool allowEndResult) {
  DepthGuard depth(*this);
  auto body = _builder.makeBody();
  while (!isAtEnd() && peek().type != TokenType::RBRACE) {
    try {
      if (isStandaloneMacroInvocation()) {
        auto generated = parseMacroStatements();
        for (auto &statement : generated->statements)
          body->addStatement(std::move(statement));
        if (generated->result) {
          if (peek().type == TokenType::RBRACE ||
              (allowEndResult && isAtEnd())) {
            body->setResult(std::move(generated->result));
          } else {
            _diag.report(pointAfter(generated->result->span),
                         DiagnosticLevel::Error,
                         "Expected ';' after expression.");
            body->addStatement(std::move(generated->result));
          }
        }
      } else if (peek().type == TokenType::VAR) {
        body->addStatement(parseBindingDecl(BindingKind::Mutable));
      } else if (peek().type == TokenType::LET) {
        body->addStatement(parseBindingDecl(BindingKind::Immutable));
      } else if (peek().type == TokenType::CONST) {
        body->addStatement(parseBindingDecl(BindingKind::CompileTimeConstant));
      } else if (peek().type == TokenType::RETURN) {
        body->addStatement(parseReturnStmt());
      } else if (peek().type == TokenType::FAIL) {
        body->addStatement(parseFail());
      } else if (peek().type == TokenType::IF) {
        auto ifNode = parseIf();
        if (peek().type == TokenType::SEMICOLON) {
          eat(TokenType::SEMICOLON);
        }
        body->addStatement(std::move(ifNode));
      } else if (peek().type == TokenType::CASE) {
        auto caseNode = parseCase();
        if (peek().type == TokenType::SEMICOLON) {
          eat(TokenType::SEMICOLON);
        }
        body->addStatement(std::move(caseNode));
      } else if (peek().type == TokenType::IFTYPE) {
        auto ifTypeNode = parseIfType();
        if (peek().type == TokenType::SEMICOLON) {
          eat(TokenType::SEMICOLON);
        }
        body->addStatement(std::move(ifTypeNode));
      } else if (peek().type == TokenType::WHILE) {
        auto whileNode = parseWhile();
        if (peek().type == TokenType::SEMICOLON) {
          eat(TokenType::SEMICOLON);
        }
        body->addStatement(std::move(whileNode));
      } else if (peek().type == TokenType::DEFER) {
        body->addStatement(parseDefer());
      } else if (peek().type == TokenType::FOR) {
        bool isForIn = false;
        if (peek(1).type == TokenType::ID) {
          if (peek(2).type == TokenType::ID && peek(2).value == "in") {
            isForIn = true;
          } else if (peek(2).type == TokenType::COMMA &&
                     peek(3).type == TokenType::ID &&
                     peek(4).type == TokenType::ID && peek(4).value == "in") {
            isForIn = true;
          }
        } else if (peek(1).type == TokenType::LPAREN &&
                   peek(2).type == TokenType::ID) {
          if (peek(3).type == TokenType::ID && peek(3).value == "in") {
            isForIn = true;
          } else if (peek(3).type == TokenType::COMMA &&
                     peek(4).type == TokenType::ID &&
                     peek(5).type == TokenType::ID && peek(5).value == "in") {
            isForIn = true;
          }
        }
        if (isForIn) {
          auto forInNode = parseForIn();
          if (peek().type == TokenType::SEMICOLON) {
            eat(TokenType::SEMICOLON);
          }
          body->addStatement(std::move(forInNode));
        } else {
          auto forNode = parseFor();
          if (peek().type == TokenType::SEMICOLON) {
            eat(TokenType::SEMICOLON);
          }
          body->addStatement(std::move(forNode));
        }
      } else if (peek().type == TokenType::BREAK) {
        body->addStatement(parseBreak());
      } else if (peek().type == TokenType::CONTINUE) {
        body->addStatement(parseContinue());
      } else if (peek().type == TokenType::UNSAFE) {
        body->addStatement(parseUnsafeBlock());
      } else if (peek().type == TokenType::ASM) {
        body->addStatement(parseAsm());
      } else {
        auto expr = parseExpression();
        if (peek().type == TokenType::ASSIGN) {
          eat(TokenType::ASSIGN);
          auto value = parseExpression();
          Token semi = eat(TokenType::SEMICOLON);
          auto assign = _builder.makeAssign(std::move(expr), std::move(value));
          _builder.setSpan(assign.get(),
                           SourceSpan::merge(assign->target_->span, semi.span));
          body->addStatement(std::move(assign));
        } else if (!compoundAssignOp(peek().type).empty()) {
          std::string op = compoundAssignOp(peek().type);
          eat(peek().type);
          auto value = parseExpression();
          Token semi = eat(TokenType::SEMICOLON);
          auto assign = _builder.makeAssign(std::move(expr), std::move(value));
          assign->op_ = op;
          _builder.setSpan(assign.get(),
                           SourceSpan::merge(assign->target_->span, semi.span));
          body->addStatement(std::move(assign));
        } else if (peek().type == TokenType::INCREMENT ||
                   peek().type == TokenType::DECREMENT) {
          std::string op = peek().type == TokenType::INCREMENT ? "+" : "-";
          eat(peek().type);
          Token semi = eat(TokenType::SEMICOLON);
          auto one = _builder.makeConstInt(static_cast<int64_t>(1));
          _builder.setSpan(one.get(), semi.span);
          auto assign = _builder.makeAssign(std::move(expr), std::move(one));
          assign->op_ = op;
          _builder.setSpan(assign.get(),
                           SourceSpan::merge(assign->target_->span, semi.span));
          body->addStatement(std::move(assign));
        } else if (peek().type == TokenType::SEMICOLON) {
          eat(TokenType::SEMICOLON);
          if (!dynamic_cast<FunCall *>(expr.get()) &&
              !dynamic_cast<TryExpr *>(expr.get()) &&
              !dynamic_cast<FallbackExpr *>(expr.get()) &&
              !dynamic_cast<FailableHandleExpr *>(expr.get())) {
            _diag.report(expr->span, DiagnosticLevel::Warning,
                         "Expression result is unused.");
          }
          body->addStatement(std::move(expr));
        } else {
          if (peek().type == TokenType::RBRACE ||
              (allowEndResult && isAtEnd())) {
            body->setResult(std::move(expr));
          } else {
            _diag.report(pointAfter(expr->span), DiagnosticLevel::Error,
                         "Expected ';' after expression.");
            body->addStatement(std::move(expr));
          }
        }
      }
    } catch (const ParseError &e) {
      synchronize();
    }
  }
  return body;
}

std::unique_ptr<AssignNode> Parser::parseAssign() {
  auto target = parseExpression();
  eat(TokenType::ASSIGN);
  auto expr = parseExpression();
  Token semicolonToken = eat(TokenType::SEMICOLON);

  SourceSpan startSpan = target->span;
  auto node = _builder.makeAssign(std::move(target), std::move(expr));
  _builder.setSpan(node.get(),
                   SourceSpan::merge(startSpan, semicolonToken.span));
  return node;
}

std::unique_ptr<IfNode> Parser::parseIf() {
  DepthGuard depth(*this);
  Token ifKeyword = eat(TokenType::IF);

  bool oldAllow = _allowStructLiteral;
  _allowStructLiteral = false;
  if (peek().type == TokenType::LBRACE) {
    _diag.report(peek().span, DiagnosticLevel::Error,
                 "Expected condition expression after 'if'.");
    throw ParseError();
  }
  auto condition = parseExpression();
  _allowStructLiteral = oldAllow;

  eat(TokenType::LBRACE);
  auto thenBody = parseBody();
  eat(TokenType::RBRACE);

  std::unique_ptr<BodyNode> elseBody = nullptr;
  SourceSpan endSpan = _cursor.previous().span;

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

std::unique_ptr<IfTypeNode> Parser::parseIfType() {
  DepthGuard depth(*this);
  Token iftypeKeyword = eat(TokenType::IFTYPE);
  Token paramToken = eat(TokenType::ID);
  eat(TokenType::EQUAL);
  auto matchType = parseType();

  eat(TokenType::LBRACE);
  auto thenBody = parseBody();
  eat(TokenType::RBRACE);

  std::unique_ptr<BodyNode> elseBody = nullptr;
  SourceSpan endSpan = _cursor.previous().span;

  if (peek().type == TokenType::ELSE) {
    eat(TokenType::ELSE);
    if (peek().type == TokenType::IFTYPE) {
      auto nestedIfType = parseIfType();
      elseBody = _builder.makeBody();
      SourceSpan nestedSpan = nestedIfType->span;
      elseBody->addStatement(std::move(nestedIfType));
      _builder.setSpan(elseBody.get(), nestedSpan);
      endSpan = nestedSpan;
    } else {
      eat(TokenType::LBRACE);
      elseBody = parseBody();
      Token rbrace = eat(TokenType::RBRACE);
      endSpan = rbrace.span;
    }
  }

  auto ifTypeNode =
      _builder.makeIfType(paramToken.value, std::move(matchType),
                          std::move(thenBody), std::move(elseBody));
  _builder.setSpan(ifTypeNode.get(),
                   SourceSpan::merge(iftypeKeyword.span, endSpan));
  return ifTypeNode;
}

std::unique_ptr<WhileNode> Parser::parseWhile() {
  Token whileKeyword = eat(TokenType::WHILE);

  bool oldAllow = _allowStructLiteral;
  _allowStructLiteral = false;
  auto condition = parseExpression();
  _allowStructLiteral = oldAllow;

  eat(TokenType::LBRACE);
  auto body = parseBody();
  Token rbraceToken = eat(TokenType::RBRACE);

  auto whileNode = _builder.makeWhile(std::move(condition), std::move(body));
  _builder.setSpan(whileNode.get(),
                   SourceSpan::merge(whileKeyword.span, rbraceToken.span));
  return whileNode;
}

std::unique_ptr<AssignNode> Parser::parseForIncrementAssign() {
  auto target = parseExpression();

  if (peek().type == TokenType::INCREMENT ||
      peek().type == TokenType::DECREMENT) {
    std::string op = peek().type == TokenType::INCREMENT ? "+" : "-";
    Token opTok = eat(peek().type);
    auto one = _builder.makeConstInt(static_cast<int64_t>(1));
    _builder.setSpan(one.get(), opTok.span);
    SourceSpan startSpan = target->span;
    auto assign = _builder.makeAssign(std::move(target), std::move(one));
    assign->op_ = op;
    _builder.setSpan(assign.get(), SourceSpan::merge(startSpan, opTok.span));
    return assign;
  }

  std::string compoundOp = compoundAssignOp(peek().type);
  if (!compoundOp.empty()) {
    eat(peek().type);
    auto expr = parseExpression();
    SourceSpan startSpan = target->span;
    auto assign = _builder.makeAssign(std::move(target), std::move(expr));
    assign->op_ = compoundOp;
    _builder.setSpan(assign.get(),
                     SourceSpan::merge(startSpan, assign->expr_->span));
    return assign;
  }

  eat(TokenType::ASSIGN);
  auto expr = parseExpression();
  auto assign = _builder.makeAssign(std::move(target), std::move(expr));
  _builder.setSpan(assign.get(), SourceSpan::merge(assign->target_->span,
                                                   assign->expr_->span));
  return assign;
}

std::unique_ptr<ForNode> Parser::parseFor() {
  Token forKeyword = eat(TokenType::FOR);
  bool hasParen = false;
  if (peek().type == TokenType::LPAREN) {
    eat(TokenType::LPAREN);
    hasParen = true;
  }

  if (peek().type != TokenType::VAR) {
    _diag.report(peek().span, DiagnosticLevel::Error,
                 "For initializer must be a variable declaration.");
    throw ParseError();
  }
  auto initializer = parseForInitBindingDecl();
  eat(TokenType::SEMICOLON);

  bool oldAllow = _allowStructLiteral;
  _allowStructLiteral = false;
  auto condition = parseExpression();
  _allowStructLiteral = oldAllow;

  eat(TokenType::SEMICOLON);
  auto increment = parseForIncrementAssign();

  if (hasParen) {
    eat(TokenType::RPAREN);
  }

  eat(TokenType::LBRACE);
  auto body = parseBody();
  Token rbraceToken = eat(TokenType::RBRACE);

  auto forNode = _builder.makeFor(std::move(initializer), std::move(condition),
                                  std::move(increment), std::move(body));
  _builder.setSpan(forNode.get(),
                   SourceSpan::merge(forKeyword.span, rbraceToken.span));
  return forNode;
}

std::unique_ptr<ForInNode> Parser::parseForIn() {
  Token forKeyword = eat(TokenType::FOR);
  bool hasParen = false;
  if (peek().type == TokenType::LPAREN) {
    eat(TokenType::LPAREN);
    hasParen = true;
  }

  std::string indexName = "";
  Token itemToken = eat(TokenType::ID);
  Token indexToken = itemToken;
  if (peek().type == TokenType::COMMA) {
    eat(TokenType::COMMA);
    indexName = itemToken.value;
    itemToken = eat(TokenType::ID);
  }

  if (peek().type != TokenType::ID || peek().value != "in") {
    _diag.report(peek().span, DiagnosticLevel::Error,
                 "Expected 'in' in for-in loop.");
    throw ParseError();
  }
  eat(TokenType::ID); // "in"

  bool oldAllow = _allowStructLiteral;
  _allowStructLiteral = false;
  auto iterable = parseExpression();
  _allowStructLiteral = oldAllow;

  if (hasParen) {
    eat(TokenType::RPAREN);
  }

  eat(TokenType::LBRACE);
  auto body = parseBody();
  Token rbraceToken = eat(TokenType::RBRACE);

  auto forInNode = _builder.makeForIn(indexName, itemToken.value,
                                      std::move(iterable), std::move(body));
  forInNode->itemSyntaxName_ = SyntaxName(itemToken);
  if (!indexName.empty())
    forInNode->indexSyntaxName_ = SyntaxName(indexToken);
  _builder.setSpan(forInNode.get(),
                   SourceSpan::merge(forKeyword.span, rbraceToken.span));
  return forInNode;
}

std::unique_ptr<ReturnNode> Parser::parseReturnStmt() {
  Token returnKeyword = eat(TokenType::RETURN);
  std::unique_ptr<ExpressionNode> expr = nullptr;
  if (peek().type != TokenType::SEMICOLON) {
    expr = parseExpression();
  }

  Token semicolonToken = eat(TokenType::SEMICOLON);

  auto returnNode = _builder.makeReturn(std::move(expr));
  _builder.setSpan(returnNode.get(),
                   SourceSpan::merge(returnKeyword.span, semicolonToken.span));
  return returnNode;
}

std::unique_ptr<BreakNode> Parser::parseBreak() {
  Token breakKeyword = eat(TokenType::BREAK);
  Token semicolonToken = eat(TokenType::SEMICOLON);
  auto node = _builder.makeBreak();
  _builder.setSpan(node.get(),
                   SourceSpan::merge(breakKeyword.span, semicolonToken.span));
  return node;
}

std::unique_ptr<ContinueNode> Parser::parseContinue() {
  Token continueKeyword = eat(TokenType::CONTINUE);
  Token semicolonToken = eat(TokenType::SEMICOLON);
  auto node = _builder.makeContinue();
  _builder.setSpan(
      node.get(), SourceSpan::merge(continueKeyword.span, semicolonToken.span));
  return node;
}

std::unique_ptr<FailNode> Parser::parseFail() {
  Token failKeyword = eat(TokenType::FAIL);
  auto errorValue = parseExpression();
  Token semicolonToken = eat(TokenType::SEMICOLON);

  auto node = _builder.makeFail(std::move(errorValue));
  _builder.setSpan(node.get(),
                   SourceSpan::merge(failKeyword.span, semicolonToken.span));
  return node;
}

std::unique_ptr<UnsafeBlockNode> Parser::parseUnsafeBlock() {
  Token unsafeKeyword = eat(TokenType::UNSAFE);
  eat(TokenType::LBRACE);
  auto body = parseBody();
  Token rbraceToken = eat(TokenType::RBRACE);

  auto unsafeBlock = _builder.makeUnsafeBlock();
  unsafeBlock->statements = std::move(body->statements);
  unsafeBlock->result = std::move(body->result);
  _builder.setSpan(unsafeBlock.get(),
                   SourceSpan::merge(unsafeKeyword.span, rbraceToken.span));
  return unsafeBlock;
}

std::vector<AsmOperandNode> Parser::parseAsmOperandList() {
  std::vector<AsmOperandNode> operands;
  while (true) {
    AsmOperandNode operand;
    operand.constraint = eat(TokenType::STRING).value;
    eat(TokenType::LPAREN);
    operand.expr = parseExpression();
    eat(TokenType::RPAREN);
    operands.push_back(std::move(operand));
    if (peek().type != TokenType::COMMA)
      break;
    eat(TokenType::COMMA);
  }
  return operands;
}

std::unique_ptr<AsmStmtNode> Parser::parseAsm() {
  Token asmKeyword = eat(TokenType::ASM);
  eat(TokenType::LPAREN);
  Token templateToken = eat(TokenType::STRING);

  auto node = _builder.makeAsm(templateToken.value);

  // The lexer merges adjacent ':' into '::', so consume colon separators one
  // at a time, buffering the second half of a '::' token.
  bool pendingColon = false;
  auto consumeColon = [&]() -> bool {
    if (pendingColon) {
      pendingColon = false;
      return true;
    }
    if (peek().type == TokenType::COLON) {
      eat(TokenType::COLON);
      return true;
    }
    if (peek().type == TokenType::DOUBLECOLON) {
      eat(TokenType::DOUBLECOLON);
      pendingColon = true;
      return true;
    }
    return false;
  };
  auto sectionEmpty = [&]() -> bool {
    return pendingColon || peek().type == TokenType::COLON ||
           peek().type == TokenType::DOUBLECOLON ||
           peek().type == TokenType::RPAREN;
  };

  if (consumeColon()) {
    if (!sectionEmpty())
      node->outputs = parseAsmOperandList();

    if (consumeColon()) {
      if (!sectionEmpty())
        node->inputs = parseAsmOperandList();

      if (consumeColon()) {
        if (!sectionEmpty()) {
          node->clobbers.push_back(eat(TokenType::STRING).value);
          while (peek().type == TokenType::COMMA) {
            eat(TokenType::COMMA);
            node->clobbers.push_back(eat(TokenType::STRING).value);
          }
        }
      }
    }
  }

  eat(TokenType::RPAREN);
  Token semi = eat(TokenType::SEMICOLON);

  _builder.setSpan(node.get(), SourceSpan::merge(asmKeyword.span, semi.span));
  return node;
}

std::unique_ptr<DeferNode> Parser::parseDefer() {
  Token keyword = eat(TokenType::DEFER);
  std::unique_ptr<Node> stmt = nullptr;
  SourceSpan eSpan;

  if (peek().type == TokenType::LBRACE) {
    Token lbrace = eat(TokenType::LBRACE);
    auto body = parseBody();
    Token rbrace = eat(TokenType::RBRACE);
    _builder.setSpan(body.get(), SourceSpan::merge(lbrace.span, rbrace.span));
    stmt = std::move(body);
    eSpan = rbrace.span;
  } else {
    auto expr = parseExpression();
    if (peek().type == TokenType::ASSIGN) {
      eat(TokenType::ASSIGN);
      auto value = parseExpression();
      Token semi = eat(TokenType::SEMICOLON);
      auto assign = _builder.makeAssign(std::move(expr), std::move(value));
      _builder.setSpan(assign.get(),
                       SourceSpan::merge(assign->target_->span, semi.span));
      stmt = std::move(assign);
      eSpan = semi.span;
    } else {
      Token semi = eat(TokenType::SEMICOLON);
      eSpan = semi.span;
      stmt = std::move(expr);
    }
  }

  auto node = _builder.makeDefer(std::move(stmt));
  _builder.setSpan(node.get(), SourceSpan::merge(keyword.span, eSpan));
  return node;
}

} // namespace zap
