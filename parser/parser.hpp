#pragma once
#include "../ast/nodes.hpp"
#include "../token/token.hpp"
#include <vector>

class Parser {
public:
    unsigned long long int pos_;
    std::vector<Token> toks_;
    Parser() {

    }
    std::unique_ptr<RootNode>parse(std::vector<Token> toks);
    Token consume(TokenType expected, std::string err_msg="");
	void advance();
	Token next();
    //statements parsing
    std::unique_ptr<FunDecl>parseFun();
    std::vector<std::unique_ptr<ParameterNode>> parseParameters();
    std::unique_ptr<TypeNode> parseType();
    std::unique_ptr<BodyNode> parseBody();
    std::unique_ptr<ExpressionNode> parseExpression();
	std::unique_ptr<ExpressionNode> parsePrimary();
    std::unique_ptr<ExpressionNode> parseTerm();
	std::unique_ptr<ExpressionNode> parseFactor();
    std::unique_ptr<ExpressionNode> parseUnary();
    std::unique_ptr<ExpressionNode> parseIntConst(Token current);
    std::unique_ptr<ExpressionNode> parseConstId(Token current);

	std::unique_ptr<StatementNode> parseStatement();
    std::unique_ptr<VarDecl> parseVarDecl();
    std::unique_ptr<ReturnNode> parseReturn();
    Token peek();
    Token peek(int offset);
    bool isAtEnd();
};