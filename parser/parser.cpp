// parser/parser.cpp
#include "parser.hpp"
#include <iostream>
#include <cstdlib>

namespace zap {

Parser::Parser(const std::vector<Token>& tokens) : _tokens(tokens), _pos(0) {}

Parser::~Parser() {}

std::unique_ptr<RootNode> Parser::parse() {
    auto root = _builder.makeRoot();
    while (!isAtEnd()) {
        if (peek().type == TokenType::FUN) {
            root->addChild(parseFunDecl());
        } else {
            std::cerr << "Parser Error: Unexpected token " << peek().value << " at " << peek().pos << std::endl;
            exit(EXIT_FAILURE);
        }
    }
    return root;
}

    std::unique_ptr<FunDecl> Parser::parseFunDecl() {
        Token funKeyword = eat(TokenType::FUN);

        Token funNameToken = eat(TokenType::ID);
        auto funDecl = _builder.makeFunDecl(funNameToken.value);

        eat(TokenType::LPAREN);

        // Parse parameters
        if (peek().type != TokenType::RPAREN) {
            do {
                funDecl->params_.push_back(parseParameter());
            } while (peek().type == TokenType::COMMA && eat(TokenType::COMMA).type == TokenType::COMMA);
        }

        eat(TokenType::RPAREN);

        eat(TokenType::LBRACE);

        funDecl->body_ = parseBody();

        Token rbraceToken = eat(TokenType::RBRACE);

        _builder.setSpan(funDecl.get(), funKeyword.pos, rbraceToken.pos + rbraceToken.value.length());

        return funDecl;
    }

    std::unique_ptr<BodyNode> Parser::parseBody() {
        auto body = _builder.makeBody();
        while (!isAtEnd() && peek().type != TokenType::RBRACE) {
            if (peek().type == TokenType::VAR) {
                body->addStatement(parseVarDecl());
            } else if (peek().type == TokenType::RETURN) {
                body->addStatement(parseReturnStmt());
            } else {
                std::cerr << "Parser Error: Unexpected token in body " << peek().value << " at " << peek().pos << std::endl;
                exit(EXIT_FAILURE);
            }
        }
        return body;
    }

    std::unique_ptr<ParameterNode> Parser::parseParameter() {
        Token paramNameToken = eat(TokenType::ID);
        eat(TokenType::COLON);
        Token paramTypeToken = eat(TokenType::ID);
        auto typeNode = _builder.makeType(paramTypeToken.value);
        auto paramNode = _builder.makeParam(paramNameToken.value, std::move(typeNode));
        _builder.setSpan(paramNode.get(), paramNameToken.pos, paramTypeToken.pos + paramTypeToken.value.length());
        return paramNode;
    }

    std::unique_ptr<VarDecl> Parser::parseVarDecl() {
        Token varKeyword = eat(TokenType::VAR);
    Token varNameToken = eat(TokenType::ID);

    eat(TokenType::COLON);

    Token typeNameToken = eat(TokenType::ID);
    auto typeNode = _builder.makeType(typeNameToken.value);

    eat(TokenType::ASSIGN);

    auto expr = parseExpression();

    Token semicolonToken = eat(TokenType::SEMICOLON);

    auto varDecl = _builder.makeVarDecl(varNameToken.value, std::move(typeNode), std::move(expr));
    _builder.setSpan(varDecl.get(), varKeyword.pos, semicolonToken.pos + semicolonToken.value.length());
    return varDecl;
}

std::unique_ptr<ReturnNode> Parser::parseReturnStmt() {
    Token returnKeyword = eat(TokenType::RETURN);

    auto expr = parseExpression();

    Token semicolonToken = eat(TokenType::SEMICOLON);

    auto returnNode = _builder.makeReturn(std::move(expr));
    _builder.setSpan(returnNode.get(), returnKeyword.pos, semicolonToken.pos + semicolonToken.value.length());
    return returnNode;
}

std::unique_ptr<ExpressionNode> Parser::parseExpression() {
    return parseBinaryExpression(0);
}

std::unique_ptr<ExpressionNode> Parser::parseBinaryExpression(int minPrecedence) {
    auto left = parsePrimaryExpression();

    while (true) {
        if (isAtEnd()) break;
        Token opToken = peek();
        int precedence = getPrecedence(opToken.type);

        if (precedence < minPrecedence) {
            break;
        }

        eat(opToken.type); // Consume the operator

        int nextMinPrecedence = (opToken.type == TokenType::POW) ? precedence : precedence + 1;
        auto right = parseBinaryExpression(nextMinPrecedence);

        left = _builder.makeBinExpr(std::move(left), opToken.value, std::move(right));
        // Update span for the new binary expression node
        _builder.setSpan(static_cast<BinExpr*>(left.get()), left->span.start, _tokens[_pos-1].pos + _tokens[_pos-1].value.length());
    }
    return left;
}

    std::unique_ptr<ExpressionNode> Parser::parsePrimaryExpression() {
        Token current = peek();
        if (current.type == TokenType::INTEGER) {
            eat(TokenType::INTEGER);
            auto constInt = _builder.makeConstInt(std::stoi(current.value));
            _builder.setSpan(constInt.get(), current.pos, current.pos + current.value.length());
            return constInt;
        } else if (current.type == TokenType::ID) {
            eat(TokenType::ID);
            auto constId = _builder.makeConstId(current.value);
            _builder.setSpan(constId.get(), current.pos, current.pos + current.value.length());
            return constId;
        } else if (current.type == TokenType::LPAREN) {
            eat(TokenType::LPAREN);
            auto expr = parseExpression();
            Token rparenToken = eat(TokenType::RPAREN);
            _builder.setSpan(static_cast<ExpressionNode*>(expr.get()), current.pos, rparenToken.pos + rparenToken.value.length());
            return expr;
        }
        std::cerr << "Parser Error: Expected primary expression, got " << current.value << " at " << current.pos << std::endl;
        exit(EXIT_FAILURE);
    }
int Parser::getPrecedence(TokenType type) {
    switch (type) {
        case TokenType::PLUS:
        case TokenType::MINUS:
            return 10;
        case TokenType::MULTIPLY:
        case TokenType::DIVIDE:
        case TokenType::MODULO:
            return 20;
                    case TokenType::POW:
                        return 30;        default:
            return -1; // Not an operator
    }
}

const Token& Parser::peek() const {
    if (isAtEnd()) {
        static const Token dummy(0, TokenType::SEMICOLON, "");
        return dummy;
    }
    return _tokens[_pos];
}

Token Parser::eat(TokenType expectedType) {
    if (isAtEnd()) {
        std::cerr << "Parser Error: Expected token type " << expectedType << " but reached end of file." << std::endl;
        exit(EXIT_FAILURE);
    }
    Token current = _tokens[_pos];
    if (current.type == expectedType) {
        _pos++;
        return current;
    } else {
        std::cerr << "Parser Error: Expected token type " << expectedType << " (" << static_cast<int>(expectedType) << "), but got " << current.type << " ('" << current.value << "') at position " << current.pos << std::endl;
        exit(EXIT_FAILURE);
    }
}

bool Parser::isAtEnd() const {
    return _pos >= _tokens.size();
}

} // namespace zap
