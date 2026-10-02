#include "parser.hpp"

namespace zap {

std::unique_ptr<CaseNode> Parser::parseCase() {
  Token caseKeyword = eat(TokenType::CASE);

  bool oldAllow = _allowStructLiteral;
  _allowStructLiteral = false;
  if (peek().type == TokenType::LBRACE) {
    _diag.report(peek().span, DiagnosticLevel::Error,
                 "Expected expression after 'case'.");
    throw ParseError();
  }
  std::unique_ptr<ExpressionNode> scrutinee;
  try {
    scrutinee = parseExpression();
  } catch (...) {
    _allowStructLiteral = oldAllow;
    throw;
  }
  _allowStructLiteral = oldAllow;

  eat(TokenType::LBRACE);
  std::vector<CaseArm> arms;
  while (!isAtEnd() && peek().type != TokenType::RBRACE) {
    try {
      arms.push_back(parseCaseArm());
    } catch (const ParseError &) {
      synchronizeCaseArm();
    }
  }
  Token rbrace = eat(TokenType::RBRACE);

  auto caseNode = _builder.makeCase(std::move(scrutinee), std::move(arms));
  _builder.setSpan(caseNode.get(),
                   SourceSpan::merge(caseKeyword.span, rbrace.span));
  return caseNode;
}

CaseArm Parser::parseCaseArm() {
  CaseArm arm;
  SourceSpan startSpan = peek().span;

  if (peek().type == TokenType::ELSE) {
    eat(TokenType::ELSE);
    arm.isElse = true;
    if (peek().type == TokenType::COMMA) {
      _diag.report(peek().span, DiagnosticLevel::Error,
                   "'else' case arm cannot have patterns.");
      throw ParseError();
    }
  } else {
    arm.patterns.push_back(parseCasePattern());
    while (peek().type == TokenType::COMMA) {
      eat(TokenType::COMMA);
      arm.patterns.push_back(parseCasePattern());
    }
  }

  eat(TokenType::LBRACE);
  arm.body = parseBody();
  Token rbrace = eat(TokenType::RBRACE);
  arm.span = SourceSpan::merge(startSpan, rbrace.span);
  return arm;
}

CasePattern Parser::parseCasePattern() {
  CasePattern pattern;
  Token startToken = peek();

  switch (startToken.type) {
  case TokenType::INTEGER:
  case TokenType::STRING:
  case TokenType::CHAR:
  case TokenType::BOOL:
    pattern.kind = CasePatternKind::Literal;
    pattern.literal = parsePrimaryExpression();
    pattern.span = pattern.literal->span;
    return pattern;

  case TokenType::MINUS:
  case TokenType::PLUS:
    if (peek(1).type == TokenType::INTEGER) {
      pattern.kind = CasePatternKind::Literal;
      pattern.literal = parseUnaryExpression();
      pattern.span = pattern.literal->span;
      return pattern;
    }
    _diag.report(startToken.span, DiagnosticLevel::Error,
                 "Expected an integer literal after sign in case pattern.");
    throw ParseError();

  case TokenType::FLOAT:
    _diag.report(startToken.span, DiagnosticLevel::Error,
                 "Float literals are not supported as case patterns.");
    throw ParseError();

  case TokenType::ID: {
    pattern.variantPath = parseQualifiedIdentifier();
    pattern.pathSyntaxName = SyntaxName(startToken);
    const bool startsRecordFields = peek().type == TokenType::LBRACE &&
                                    ((pattern.variantPath.size() == 1 &&
                                      peek(1).type == TokenType::RBRACE) ||
                                     (peek(1).type == TokenType::ID &&
                                      (peek(2).type == TokenType::COLON ||
                                       peek(2).type == TokenType::COMMA ||
                                       peek(2).type == TokenType::RBRACE)));
    if (startsRecordFields) {
      auto record = parseCaseRecordPattern(std::move(pattern.variantPath),
                                           startToken.span);
      record.pathSyntaxName = SyntaxName(startToken);
      return record;
    }
    pattern.kind = CasePatternKind::Variant;
    if (peek().type == TokenType::LPAREN) {
      eat(TokenType::LPAREN);
      if (peek().type == TokenType::RPAREN) {
        eat(TokenType::RPAREN);
        pattern.payloadKind = CasePayloadPatternKind::Empty;
      } else {
        size_t payloadPathLength = 0;
        if (peek().type == TokenType::ID) {
          payloadPathLength = 1;
          while (peek(payloadPathLength).type == TokenType::DOT &&
                 peek(payloadPathLength + 1).type == TokenType::ID) {
            payloadPathLength += 2;
          }
        }
        if (payloadPathLength != 0 &&
            peek(payloadPathLength).type == TokenType::LBRACE) {
          pattern.payloadKind = CasePayloadPatternKind::Pattern;
          pattern.payloadPattern =
              std::make_unique<CasePattern>(parseCasePattern());
        } else if (peek().type == TokenType::ID) {
          Token bindingToken = eat(TokenType::ID);
          if (bindingToken.value == "_") {
            pattern.payloadKind = CasePayloadPatternKind::Wildcard;
          } else {
            pattern.payloadKind = CasePayloadPatternKind::Binding;
            pattern.payloadSyntaxName = SyntaxName(bindingToken);
            pattern.payloadBinding = std::move(bindingToken.value);
            pattern.payloadBindingSpan = bindingToken.span;
          }
        } else {
          pattern.payloadKind = CasePayloadPatternKind::Literal;
          if ((peek().type == TokenType::MINUS ||
               peek().type == TokenType::PLUS) &&
              peek(1).type == TokenType::INTEGER) {
            pattern.payloadLiteral = parseUnaryExpression();
          } else if (peek().type == TokenType::INTEGER ||
                     peek().type == TokenType::STRING ||
                     peek().type == TokenType::CHAR ||
                     peek().type == TokenType::BOOL) {
            pattern.payloadLiteral = parsePrimaryExpression();
          } else {
            _diag.report(
                peek().span, DiagnosticLevel::Error,
                "Expected a literal, binding, or '_' in enum payload pattern.");
            throw ParseError();
          }
        }
        eat(TokenType::RPAREN);
      }
    }
    pattern.span = SourceSpan::merge(startToken.span, _cursor.previous().span);
    return pattern;
  }

  default:
    _diag.report(startToken.span, DiagnosticLevel::Error,
                 "Expected a case pattern.");
    throw ParseError();
  }
}

CasePattern Parser::parseCaseRecordPattern(std::vector<std::string> typePath,
                                           SourceSpan startSpan) {
  CasePattern pattern;
  pattern.kind = CasePatternKind::Record;
  pattern.recordPath = std::move(typePath);
  eat(TokenType::LBRACE);
  while (peek().type != TokenType::RBRACE) {
    Token name = eat(TokenType::ID);
    CaseRecordFieldPattern field;
    field.name = name.value;
    field.span = name.span;
    if (peek().type == TokenType::COLON) {
      eat(TokenType::COLON);
      if (peek().type == TokenType::ID && peek().value != "_" &&
          (peek(1).type == TokenType::COMMA ||
           peek(1).type == TokenType::RBRACE)) {
        Token binding = eat(TokenType::ID);
        field.binding = binding.value;
        field.bindingSyntaxName = SyntaxName(binding);
        field.span = SourceSpan::merge(field.span, binding.span);
      } else {
        field.nested = std::make_unique<CasePattern>(parseCasePattern());
        field.span = SourceSpan::merge(field.span, field.nested->span);
      }
    } else {
      field.binding = name.value;
      field.bindingSyntaxName = SyntaxName(name);
      field.isShorthand = true;
    }
    pattern.recordFields.push_back(std::move(field));
    if (peek().type != TokenType::COMMA)
      break;
    eat(TokenType::COMMA);
  }
  Token end = eat(TokenType::RBRACE);
  pattern.span = SourceSpan::merge(startSpan, end.span);
  return pattern;
}

} // namespace zap
