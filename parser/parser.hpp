#pragma once
#include "../token/token.hpp"
#include "../ast/root_node.hpp"
#include "../ast/fun_decl.hpp" // Needed for parseFunDecl return type
#include "../ast/body_node.hpp" // Needed for parseBody return type
#include "../ast/var_decl.hpp" // Needed for parseVarDecl return type
#include "../ast/return_node.hpp" // Needed for parseReturnStmt return type
#include "../ast/expr_node.hpp" // Needed for parseExpression return type
#include "../ast/bin_expr.hpp" // For span setting
#include "../ast/const/const_int.hpp" // For span setting
#include "ast_builder.hpp"
#include <vector>
#include <memory>

namespace zap{

    class Parser{
      public:
        Parser(const std::vector<Token>& toks); // Modified constructor
        ~Parser();
        std::unique_ptr<RootNode> parse(); // Returns the root of the AST

      private:
        std::vector<Token> _tokens;
        unsigned int _pos;
        AstBuilder _builder;

        // Helper methods
        const Token& peek() const;
        Token eat(TokenType expectedType);
        bool isAtEnd() const;

        // Parsing rules
        std::unique_ptr<FunDecl> parseFunDecl();
        std::unique_ptr<BodyNode> parseBody();
        std::unique_ptr<VarDecl> parseVarDecl();
        std::unique_ptr<ReturnNode> parseReturnStmt();
        std::unique_ptr<ExpressionNode> parseExpression();
        std::unique_ptr<ExpressionNode> parseBinaryExpression(int minPrecedence);
        std::unique_ptr<ExpressionNode> parsePrimaryExpression();
                    int getPrecedence(TokenType type);
                    std::unique_ptr<ParameterNode> parseParameter(); // New helper function
    };
}

