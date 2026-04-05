#include "parser.hpp"
#include <cstdlib>
#include <iostream>

namespace zap
{
  namespace {
    std::string qualifiedNameFromExpression(const ExpressionNode *expr) {
      if (auto id = dynamic_cast<const ConstId *>(expr)) {
        return id->value_;
      }
      if (auto member = dynamic_cast<const MemberAccessNode *>(expr)) {
        auto base = qualifiedNameFromExpression(member->left_.get());
        if (base.empty()) {
          return "";
        }
        return base + "." + member->member_;
      }
      return "";
    }
  } // namespace

  Parser::Parser(const std::vector<Token> &tokens, DiagnosticEngine &diag)
      : _diag(diag), _tokens(tokens), _pos(0) {}

  Parser::~Parser() {}

  std::unique_ptr<RootNode> Parser::parse()
  {
    auto root = _builder.makeRoot();
    while (!isAtEnd())
    {
      try
      {
        Visibility visibility = Visibility::Private;
        if (peek().type == TokenType::PUB || peek().type == TokenType::PRIV)
        {
          visibility = (eat(peek().type).type == TokenType::PUB)
                           ? Visibility::Public
                           : Visibility::Private;
        }

        auto applyVisibility = [visibility](Node *node) {
          if (auto topLevel = dynamic_cast<TopLevel *>(node))
          {
            topLevel->visibility_ = visibility;
          }
        };

        if (peek().type == TokenType::IMPORT)
        {
          auto importDecl = parseImportDecl();
          importDecl->visibility_ = visibility;
          root->addChild(std::move(importDecl));
        }
        else if (peek().type == TokenType::FUN)
        {
          auto decl = parseFunDecl();
          applyVisibility(decl.get());
          root->addChild(std::move(decl));
        }
        else if (peek().type == TokenType::EXTERN)
        {
          auto decl = parseExtDecl();
          applyVisibility(decl.get());
          root->addChild(std::move(decl));
        }
        else if (peek().type == TokenType::ENUM)
        {
          auto decl = parseEnumDecl();
          applyVisibility(decl.get());
          root->addChild(std::move(decl));
        }
        else if (peek().type == TokenType::ALIAS)
        {
          auto decl = parseTypeAliasDecl();
          applyVisibility(decl.get());
          root->addChild(std::move(decl));
        }
        else if (peek().type == TokenType::STRUCT)
        {
          auto decl = parseStructDecl();
          applyVisibility(decl.get());
          root->addChild(std::move(decl));
        }
        else if (peek().type == TokenType::RECORD)
        {
          auto decl = parseRecordDecl();
          applyVisibility(decl.get());
          root->addChild(std::move(decl));
        }
        else if (peek().type == TokenType::CONST)
        {
          auto decl = parseConstDecl();
          decl->visibility_ = visibility;
          root->addChild(std::move(decl));
        }
        else if (peek().type == TokenType::GLOBAL)
        {
          Token globalToken = eat(TokenType::GLOBAL);
          if (peek().type == TokenType::VAR)
          {
            auto varDecl = parseVarDecl();
            varDecl->isGlobal_ = true;
            varDecl->visibility_ = visibility;
            _builder.setSpan(varDecl.get(), SourceSpan::merge(globalToken.span, varDecl->span));
            root->addChild(std::move(varDecl));
          }
          else
          {
            _diag.report(peek().span, DiagnosticLevel::Error, "Expected 'var' after 'global'");
            _pos++;
            synchronize();
          }
        }
        else
        {
          _diag.report(peek().span, DiagnosticLevel::Error,
                       "Unexpected token " + peek().value);
          _pos++;
          synchronize();
        }
      }
      catch (const ParseError &e)
      {
        synchronize();
      }
    }
    return root;
  }

  std::unique_ptr<ImportNode> Parser::parseImportDecl()
  {
    Token importKeyword = eat(TokenType::IMPORT);
    Token pathToken = eat(TokenType::STRING);
    std::string moduleAlias;
    std::vector<ImportBinding> bindings;

    if (peek().type == TokenType::AS)
    {
      eat(TokenType::AS);
      moduleAlias = eat(TokenType::ID).value;
    }

    if (peek().type == TokenType::LBRACE)
    {
      eat(TokenType::LBRACE);
      if (peek().type != TokenType::RBRACE)
      {
        do
        {
          Token sourceToken = eat(TokenType::ID);
          std::string localName = sourceToken.value;
          if (peek().type == TokenType::AS)
          {
            eat(TokenType::AS);
            localName = eat(TokenType::ID).value;
          }
          bindings.push_back({sourceToken.value, localName});
        } while (peek().type == TokenType::COMMA &&
                 eat(TokenType::COMMA).type == TokenType::COMMA);
      }
      eat(TokenType::RBRACE);
    }

    Token semiToken = eat(TokenType::SEMICOLON);
    auto importDecl =
        _builder.makeImport(pathToken.value, std::move(moduleAlias), std::move(bindings));
    _builder.setSpan(importDecl.get(),
                     SourceSpan::merge(importKeyword.span, semiToken.span));
    return importDecl;
  }

  std::unique_ptr<FunDecl> Parser::parseFunDecl()
  {
    Token funKeyword = eat(TokenType::FUN);

    Token funNameToken = eat(TokenType::ID);
    auto funDecl = _builder.makeFunDecl(funNameToken.value);

    eat(TokenType::LPAREN);

    if (peek().type != TokenType::RPAREN)
    {
      do
      {
        funDecl->params_.push_back(parseParameter());
      } while (peek().type == TokenType::COMMA &&
               eat(TokenType::COMMA).type == TokenType::COMMA);
    }

    eat(TokenType::RPAREN);

    if (peek().type != TokenType::LBRACE)
    {
      funDecl->returnType_ = parseType();
    }
    else
    {
      funDecl->returnType_.reset();
    }

    eat(TokenType::LBRACE);

    funDecl->body_ = parseBody();

    Token rbraceToken = eat(TokenType::RBRACE);

    _builder.setSpan(funDecl.get(),
             SourceSpan::merge(funNameToken.span, rbraceToken.span));

    return funDecl;
  }

  std::unique_ptr<ExtDecl> Parser::parseExtDecl()
  {
    Token externKeyword = eat(TokenType::EXTERN);
    Token funKeyword = eat(TokenType::FUN);

    Token funNameToken = eat(TokenType::ID);
    auto extDecl = std::make_unique<ExtDecl>();
    extDecl->name_ = funNameToken.value;

    eat(TokenType::LPAREN);

    if (peek().type != TokenType::RPAREN)
    {
      do
      {
        extDecl->params_.push_back(parseParameter());
      } while (peek().type == TokenType::COMMA &&
               eat(TokenType::COMMA).type == TokenType::COMMA);
    }

    eat(TokenType::RPAREN);

    if (peek().type != TokenType::SEMICOLON)
    {
      extDecl->returnType_ = parseType();
    }
    else
    {
      extDecl->returnType_ = _builder.makeType("void");
      const auto &nextToken = peek();
      _builder.setSpan(extDecl->returnType_.get(),
                       SourceSpan(nextToken.span.line, nextToken.span.column,
                                  nextToken.span.offset, 0));
    }

    Token semiToken = eat(TokenType::SEMICOLON);

    _builder.setSpan(extDecl.get(),
             SourceSpan::merge(funNameToken.span, semiToken.span));

    return extDecl;
  }

  std::unique_ptr<BodyNode> Parser::parseBody()
  {
    auto body = _builder.makeBody();
    while (!isAtEnd() && peek().type != TokenType::RBRACE)
    {
      try
      {
        if (peek().type == TokenType::VAR)
        {
          body->addStatement(parseVarDecl());
        }
        else if (peek().type == TokenType::CONST)
        {
          body->addStatement(parseConstDecl());
        }
        else if (peek().type == TokenType::RETURN)
        {
          body->addStatement(parseReturnStmt());
        }
        else if (peek().type == TokenType::IF)
        {
          auto ifNode = parseIf();
          if (peek().type == TokenType::SEMICOLON)
          {
            eat(TokenType::SEMICOLON);
          }
          body->addStatement(std::move(ifNode));
        }
        else if (peek().type == TokenType::WHILE)
        {
          auto whileNode = parseWhile();
          if (peek().type == TokenType::SEMICOLON)
          {
            eat(TokenType::SEMICOLON);
          }
          body->addStatement(std::move(whileNode));
        }
        else if (peek().type == TokenType::BREAK)
        {
          body->addStatement(parseBreak());
        }
        else if (peek().type == TokenType::CONTINUE)
        {
          body->addStatement(parseContinue());
        }
        else
        {
          auto expr = parseExpression();
          if (peek().type == TokenType::ASSIGN)
          {
            eat(TokenType::ASSIGN);
            auto value = parseExpression();
            Token semi = eat(TokenType::SEMICOLON);
            auto assign = _builder.makeAssign(std::move(expr), std::move(value));
            _builder.setSpan(assign.get(), SourceSpan::merge(assign->target_->span, semi.span));
            body->addStatement(std::move(assign));
          }
          else if (peek().type == TokenType::SEMICOLON)
          {
            eat(TokenType::SEMICOLON);
            body->addStatement(std::move(expr));
          }
          else
          {
            if (peek().type == TokenType::RBRACE)
            {
              body->setResult(std::move(expr));
            }
            else
            {
              body->addStatement(std::move(expr));
            }
          }
        }
      }
      catch (const ParseError &e)
      {
        synchronize();
      }
    }
    return body;
  }

  std::unique_ptr<ParameterNode> Parser::parseParameter()
  {
    bool isRef = false;
    if (peek().type == TokenType::REF) {
      eat(TokenType::REF);
      isRef = true;
    }
    Token paramNameToken = eat(TokenType::ID);
    eat(TokenType::COLON);
    auto typeNode = parseType();
    auto paramNode =
        _builder.makeParam(paramNameToken.value, std::move(typeNode), isRef);
    _builder.setSpan(paramNode.get(), SourceSpan::merge(paramNameToken.span,
                                                        paramNode->type->span));
    return paramNode;
  }

  std::unique_ptr<VarDecl> Parser::parseVarDecl()
  {
    Token varKeyword = eat(TokenType::VAR);
    Token varNameToken = eat(TokenType::ID);

    eat(TokenType::COLON);

    auto typeNode = parseType();

    if (peek().type == TokenType::ASSIGN)
    {
      eat(TokenType::ASSIGN);
      auto expr = parseExpression();
      Token semicolonToken = eat(TokenType::SEMICOLON);

      auto varDecl = _builder.makeVarDecl(varNameToken.value, std::move(typeNode),
                                          std::move(expr));
      _builder.setSpan(varDecl.get(),
                       SourceSpan::merge(varKeyword.span, semicolonToken.span));
      return varDecl;
    }
    else
    {
      Token semicolonToken = eat(TokenType::SEMICOLON);
      auto varDecl =
          _builder.makeVarDecl(varNameToken.value, std::move(typeNode), nullptr);
      _builder.setSpan(varDecl.get(),
                       SourceSpan::merge(varKeyword.span, semicolonToken.span));
      return varDecl;
    }
  }

  std::unique_ptr<ConstDecl> Parser::parseConstDecl()
  {
    Token constKeyword = eat(TokenType::CONST);
    Token constNameToken = eat(TokenType::ID);

    eat(TokenType::COLON);

    auto typeNode = parseType();

    eat(TokenType::ASSIGN);
    auto expr = parseExpression();
    Token semicolonToken = eat(TokenType::SEMICOLON);

    auto constDecl = _builder.makeConstDecl(constNameToken.value, std::move(typeNode),
                                            std::move(expr));
    _builder.setSpan(constDecl.get(),
                     SourceSpan::merge(constKeyword.span, semicolonToken.span));
    return constDecl;
  }

  std::unique_ptr<AssignNode> Parser::parseAssign()
  {
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

  std::unique_ptr<TypeNode> Parser::parseType()
  {
    if (peek().type == TokenType::SQUARE_LBRACE &&
        (peek(1).type == TokenType::INTEGER || peek(1).type == TokenType::ID || peek(1).type == TokenType::SQUARE_LBRACE))
    {
      Token lbracket = eat(TokenType::SQUARE_LBRACE);
      auto size = parseExpression();
      Token rbracket = eat(TokenType::SQUARE_RBRACE);
      auto baseType = parseType();
      
      auto arrayType = _builder.makeType("");
      arrayType->isArray = true;
      arrayType->arraySize = std::move(size);
      arrayType->baseType = std::move(baseType);
      
      _builder.setSpan(arrayType.get(),
                       SourceSpan::merge(lbracket.span, arrayType->baseType->span));
      return arrayType;
    }
    Token startToken = peek();
    auto identifiers = parseQualifiedIdentifier();
    auto typeNode = _builder.makeType(identifiers.back());
    identifiers.pop_back();
    typeNode->qualifiers = std::move(identifiers);
    _builder.setSpan(typeNode.get(),
                     SourceSpan::merge(startToken.span, _tokens[_pos - 1].span));
    return typeNode;
  }

  std::unique_ptr<ArrayLiteralNode> Parser::parseArrayLiteral()
  {
    Token lbrace = eat(TokenType::LBRACE);
    std::vector<std::unique_ptr<ExpressionNode>> elements;
    if (peek().type != TokenType::RBRACE)
    {
      do
      {
        elements.push_back(parseExpression());
      } while (peek().type == TokenType::COMMA &&
               eat(TokenType::COMMA).type == TokenType::COMMA);
    }
    Token rbrace = eat(TokenType::RBRACE);
    auto node = _builder.makeArrayLiteral(std::move(elements));
    _builder.setSpan(node.get(), SourceSpan::merge(lbrace.span, rbrace.span));
    return node;
  }

  std::unique_ptr<IfNode> Parser::parseIf()
  {
    Token ifKeyword = eat(TokenType::IF);

    bool hasParen = false;
    if (peek().type == TokenType::LPAREN)
    {
      eat(TokenType::LPAREN);
      hasParen = true;
    }

    bool oldAllow = _allowStructLiteral;
    if (!hasParen) {
        _allowStructLiteral = false;
    }
    auto condition = parseExpression();
    _allowStructLiteral = oldAllow;

    if (hasParen)
    {
      eat(TokenType::RPAREN);
    }

    eat(TokenType::LBRACE);
    auto thenBody = parseBody();
    eat(TokenType::RBRACE);

    std::unique_ptr<BodyNode> elseBody = nullptr;
    SourceSpan endSpan = _tokens[_pos - 1].span;

    if (peek().type == TokenType::ELSE)
    {
      eat(TokenType::ELSE);
      if (peek().type == TokenType::IF)
      {
        auto nestedIf = parseIf();
        elseBody = _builder.makeBody();
        SourceSpan nestedSpan = nestedIf->span;
        elseBody->addStatement(std::move(nestedIf));
        _builder.setSpan(elseBody.get(), nestedSpan);
        endSpan = nestedSpan;
      }
      else
      {
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

  std::unique_ptr<WhileNode> Parser::parseWhile()
  {
    Token whileKeyword = eat(TokenType::WHILE);

    bool hasParen = false;
    if (peek().type == TokenType::LPAREN)
    {
      eat(TokenType::LPAREN);
      hasParen = true;
    }

    bool oldAllow = _allowStructLiteral;
    if (!hasParen) {
        _allowStructLiteral = false;
    }
    auto condition = parseExpression();
    _allowStructLiteral = oldAllow;

    if (hasParen)
    {
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

  std::unique_ptr<ReturnNode> Parser::parseReturnStmt()
  {
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

  std::unique_ptr<BreakNode> Parser::parseBreak()
  {
    Token breakKeyword = eat(TokenType::BREAK);
    Token semicolonToken = eat(TokenType::SEMICOLON);
    auto node = _builder.makeBreak();
    _builder.setSpan(node.get(), SourceSpan::merge(breakKeyword.span, semicolonToken.span));
    return node;
  }

  std::unique_ptr<ContinueNode> Parser::parseContinue()
  {
    Token continueKeyword = eat(TokenType::CONTINUE);
    Token semicolonToken = eat(TokenType::SEMICOLON);
    auto node = _builder.makeContinue();
    _builder.setSpan(node.get(), SourceSpan::merge(continueKeyword.span, semicolonToken.span));
    return node;
  }

  std::unique_ptr<ExpressionNode> Parser::parseExpression()
  {
    return parseTernaryExpression();
  }

  std::unique_ptr<ExpressionNode> Parser::parseTernaryExpression()
  {
    auto condition = parseBinaryExpression(0);

    if (peek().type != TokenType::QUESTION)
      return condition;

    eat(TokenType::QUESTION);
    auto thenExpr = parseTernaryExpression();
    eat(TokenType::COLON);
    auto elseExpr = parseTernaryExpression();

    SourceSpan conditionSpan = condition->span;
    SourceSpan elseSpan = elseExpr->span;
    auto ternary = _builder.makeTernaryExpr(std::move(condition),
                                            std::move(thenExpr),
                                            std::move(elseExpr));
    _builder.setSpan(ternary.get(), SourceSpan::merge(conditionSpan, elseSpan));
    return ternary;
  }

  std::unique_ptr<ExpressionNode>
  Parser::parseBinaryExpression(int minPrecedence)
  {
    auto left = parseUnaryExpression();

    while (true)
    {
      if (isAtEnd())
        break;
      Token opToken = peek();
      int precedence = getPrecedence(opToken.type);

      if (precedence < minPrecedence)
      {
        break;
      }

      eat(opToken.type);

      int nextMinPrecedence =
          (opToken.type == TokenType::POW) ? precedence : precedence + 1;

      auto right = parseBinaryExpression(nextMinPrecedence);

      SourceSpan leftSpan = left->span;
      SourceSpan rightSpan = right->span;
      left = _builder.makeBinExpr(std::move(left), opToken.value,
                                  std::move(right));
      _builder.setSpan(static_cast<BinExpr *>(left.get()),
                       SourceSpan::merge(leftSpan, rightSpan));
    }
    return left;
  }

  std::unique_ptr<ExpressionNode> Parser::parseUnaryExpression()
  {
    if (peek().type == TokenType::NOT || peek().type == TokenType::MINUS)
    {
      Token opToken = eat(peek().type);
      auto expr = parseUnaryExpression();
      SourceSpan endSpan = expr->span;
      auto node = _builder.makeUnaryExpr(opToken.value, std::move(expr));
      _builder.setSpan(node.get(), SourceSpan::merge(opToken.span, endSpan));
      return node;
    }
    return parsePostfixExpression();
  }

  std::unique_ptr<ExpressionNode> Parser::parsePostfixExpression()
  {
    auto left = parsePrimaryExpression();

    while (true)
    {
      if (isAtEnd())
        break;
      Token opToken = peek();

      if (opToken.type == TokenType::DOT)
      {
        eat(TokenType::DOT);
        Token memberToken = eat(TokenType::ID);
        SourceSpan leftSpan = left->span;
        left = std::move(_builder.makeMemberAccess(std::move(left), memberToken.value));
        _builder.setSpan(left.get(), SourceSpan::merge(leftSpan, memberToken.span));
      }
      else if (opToken.type == TokenType::SQUARE_LBRACE)
      {
        eat(TokenType::SQUARE_LBRACE);
        auto index = parseExpression();
        Token rbracket = eat(TokenType::SQUARE_RBRACE);
        SourceSpan leftSpan = left->span;
        left = _builder.makeIndexAccess(std::move(left), std::move(index));
        _builder.setSpan(left.get(), SourceSpan::merge(leftSpan, rbracket.span));
      }
      else if (opToken.type == TokenType::LPAREN)
      {
        SourceSpan leftSpan = left->span;
        auto funCall = _builder.makeFunCall(std::move(left));
        eat(TokenType::LPAREN);

        if (peek().type != TokenType::RPAREN)
        {
          do
          {
            std::string argName = "";
            bool argIsRef = false;
            if (peek().type == TokenType::ID &&
                peek(1).type == TokenType::ASSIGN)
            {
              argName = eat(TokenType::ID).value;
              eat(TokenType::ASSIGN);
            }
            if (peek().type == TokenType::REF) {
              eat(TokenType::REF);
              argIsRef = true;
            }
            auto argValue = parseExpression();
            funCall->params_.push_back(
                std::make_unique<Argument>(argName, std::move(argValue), argIsRef));
          } while (peek().type == TokenType::COMMA &&
                   eat(TokenType::COMMA).type == TokenType::COMMA);
        }

        Token rparenToken = eat(TokenType::RPAREN);
        _builder.setSpan(funCall.get(),
                         SourceSpan::merge(leftSpan, rparenToken.span));
        left = std::move(funCall);
      }
      else if (_allowStructLiteral && opToken.type == TokenType::LBRACE)
      {
        auto qualifiedTypeName = qualifiedNameFromExpression(left.get());
        if (qualifiedTypeName.empty())
        {
          break;
        }
        auto structLiteral = parseStructLiteral(qualifiedTypeName);
        left = std::move(structLiteral);
      }
      else
      {
        break;
      }
    }
    return left;
  }

  std::unique_ptr<ExpressionNode> Parser::parsePrimaryExpression()
  {
    Token current = peek();
    if (current.type == TokenType::INTEGER)
    {
      eat(TokenType::INTEGER);
      int64_t val = static_cast<int64_t>(std::stoull(current.value));
      auto constInt = _builder.makeConstInt(val);
      _builder.setSpan(constInt.get(), current.span);
      return constInt;
    }
    else if (current.type == TokenType::FLOAT)
    {
      eat(TokenType::FLOAT);
      auto constFloat = _builder.makeConstFloat(std::stod(current.value));
      _builder.setSpan(constFloat.get(), current.span);
      return constFloat;
    }
    else if (current.type == TokenType::STRING)
    {
      eat(TokenType::STRING);
      auto constStr = _builder.makeConstString(current.value);
      _builder.setSpan(constStr.get(), current.span);
      return constStr;
    }
    else if (current.type == TokenType::CHAR)
    {
      eat(TokenType::CHAR);
      auto constChar = _builder.makeConstChar(current.value);
      _builder.setSpan(constChar.get(), current.span);
      return constChar;
    }
    else if (current.type == TokenType::BOOL)
    {
      eat(TokenType::BOOL);
      auto constBool = _builder.makeConstBool(current.value == "true");
      _builder.setSpan(constBool.get(), current.span);
      return constBool;
    }
    else if (current.type == TokenType::ID)
    {
      Token idToken = eat(TokenType::ID);
      if (_allowStructLiteral && peek().type == TokenType::LBRACE)
      {
        return parseStructLiteral(idToken.value);
      }
      else
      {
        auto constId = _builder.makeConstId(idToken.value);
        _builder.setSpan(constId.get(), idToken.span);
        return constId;
      }
    }
    else if (current.type == TokenType::LPAREN)
    {
      eat(TokenType::LPAREN);
      bool oldAllow = _allowStructLiteral;
      _allowStructLiteral = true;
      auto expr = parseExpression();
      _allowStructLiteral = oldAllow;
      Token rparenToken = eat(TokenType::RPAREN);
      _builder.setSpan(static_cast<ExpressionNode *>(expr.get()),
                       SourceSpan::merge(current.span, rparenToken.span));
      return expr;
    }
    else if (current.type == TokenType::LBRACE)
    {
      if (!_allowStructLiteral) {
          _diag.report(current.span, DiagnosticLevel::Error,
                       "Struct literal not allowed in this context");
          exit(EXIT_FAILURE);
      }
      return parseArrayLiteral();
    }
    _diag.report(current.span, DiagnosticLevel::Error,
                 "Expected primary expression, got " + current.value);
    exit(EXIT_FAILURE);
  }
  int Parser::getPrecedence(TokenType type)
  {
    switch (type)
    {
    case TokenType::OR:
      return 2;
    case TokenType::AND:
      return 3;
    case TokenType::CONCAT:
      return 1;
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

  const Token &Parser::peek(size_t offset) const
  {
    if (_pos + offset >= _tokens.size())
    {
      static const Token dummy(TokenType::SEMICOLON, "", SourceSpan(0, 0, 0, 0));
      return dummy;
    }
    return _tokens[_pos + offset];
  }

  Token Parser::eat(TokenType expectedType)
  {
    if (isAtEnd())
    {
      _diag.report(peek().span, DiagnosticLevel::Error,
                   "Expected " + tokenTypeToString(expectedType) +
                       " but reached end of file.");
      throw ParseError();
    }
    Token current = _tokens[_pos];
    if (current.type == expectedType)
    {
      _pos++;
      return current;
    }
    else
    {
      _diag.report(current.span, DiagnosticLevel::Error,
                   "Expected " + tokenTypeToString(expectedType) + ", but got '" +
                       current.value + "'");
      throw ParseError();
    }
  }

  void Parser::synchronize()
  {
    while (!isAtEnd())
    {
      switch (peek().type)
      {
      case TokenType::SEMICOLON:
        _pos++;
        return;
      case TokenType::FUN:
      case TokenType::IMPORT:
      case TokenType::ENUM:
      case TokenType::STRUCT:
      case TokenType::RECORD:
      case TokenType::ALIAS:
      case TokenType::EXTERN:
      case TokenType::GLOBAL:
      case TokenType::CONST:
      case TokenType::PUB:
      case TokenType::PRIV:
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

  std::vector<std::string> Parser::parseQualifiedIdentifier()
  {
    std::vector<std::string> parts;
    parts.push_back(eat(TokenType::ID).value);
    while (peek().type == TokenType::DOT)
    {
      eat(TokenType::DOT);
      parts.push_back(eat(TokenType::ID).value);
    }
    return parts;
  }

  bool Parser::isAtEnd() const { return _pos >= _tokens.size(); }

  std::unique_ptr<EnumDecl> Parser::parseEnumDecl()
  {
    Token enumKeyword = eat(TokenType::ENUM);
    Token enumNameToken = eat(TokenType::ID);

    std::vector<std::string> entries;
    eat(TokenType::LBRACE);

    if (peek().type != TokenType::RBRACE)
    {
      do
      {
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

  std::unique_ptr<TypeAliasDecl> Parser::parseTypeAliasDecl()
  {
    Token aliasToken = eat(TokenType::ALIAS);
    Token nameToken = eat(TokenType::ID);
    eat(TokenType::ASSIGN);
    auto type = parseType();
    Token semiToken = eat(TokenType::SEMICOLON);

    auto aliasDecl = _builder.makeTypeAliasDecl(nameToken.value, std::move(type));
    _builder.setSpan(aliasDecl.get(), SourceSpan::merge(aliasToken.span, semiToken.span));
    return aliasDecl;
  }

  std::unique_ptr<RecordDecl> Parser::parseRecordDecl()
  {
    Token recordKeyword = eat(TokenType::RECORD);
    Token recordNameToken = eat(TokenType::ID);

    std::vector<std::unique_ptr<ParameterNode>> fields;
    eat(TokenType::LBRACE);

    while (peek().type != TokenType::RBRACE)
    {
      fields.push_back(parseParameter());
      if (peek().type == TokenType::COMMA ||
          peek().type == TokenType::SEMICOLON)
      {
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

  std::unique_ptr<StructDeclarationNode> Parser::parseStructDecl()
  {
    Token structKeyword = eat(TokenType::STRUCT);
    Token structNameToken = eat(TokenType::ID);

    std::vector<std::unique_ptr<ParameterNode>> fields;
    eat(TokenType::LBRACE);

    if (peek().type != TokenType::RBRACE)
    {
      do
      {
        fields.push_back(parseParameter());
        
        if (peek().type == TokenType::COMMA || peek().type == TokenType::SEMICOLON)
        {
          eat(peek().type);
        }
        else
        {
          break;
        }
      } while (peek().type != TokenType::RBRACE);
    }

    eat(TokenType::RBRACE);
    return std::make_unique<StructDeclarationNode>(structNameToken.value, std::move(fields));
  }

  std::unique_ptr<StructLiteralNode> Parser::parseStructLiteral(const std::string& type_name)
  {
    eat(TokenType::LBRACE);
    std::vector<StructFieldInit> fields;

    if (peek().type != TokenType::RBRACE)
    {
      do
      {
        Token fieldName = eat(TokenType::ID);
        eat(TokenType::COLON);
        auto value = parseExpression();
        fields.emplace_back(fieldName.value, std::move(value));
        
        if (peek().type == TokenType::COMMA || peek().type == TokenType::SEMICOLON)
        {
          eat(peek().type);
        }
        else
        {
          break;
        }
      } while (peek().type != TokenType::RBRACE);
    }

    eat(TokenType::RBRACE);
    return std::make_unique<StructLiteralNode>(type_name, std::move(fields));
  }

} // namespace zap
