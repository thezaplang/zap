#pragma once
#include "../ast/bin_expr.hpp"
#include "../ast/body_node.hpp"
#include "../ast/const/const_int.hpp"
#include "../ast/enum_decl.hpp"
#include "../ast/array_literal.hpp"
#include "../ast/assign_node.hpp"
#include "../ast/expr_node.hpp"
#include "../ast/fun_decl.hpp"
#include "../ast/if_node.hpp"
#include "../ast/record_decl.hpp"
#include "../ast/return_node.hpp"
#include "../ast/root_node.hpp"
#include "../ast/var_decl.hpp"
#include "../ast/while_node.hpp"
#include "../token/token.hpp"
#include "ast_builder.hpp"
#include <memory>
#include <vector>

namespace zap {

class Parser {
public:
  Parser(const std::vector<Token> &toks);
  ~Parser();
  std::unique_ptr<RootNode> parse(); // Returns the root of the AST

private:
  std::vector<Token> _tokens;
  size_t _pos;
  AstBuilder _builder;

  // Helper methods
  const Token &peek(size_t offset = 0) const;
  Token eat(TokenType expectedType);
  bool isAtEnd() const;

  // Parsing rules
  std::unique_ptr<FunDecl> parseFunDecl();
  std::unique_ptr<BodyNode> parseBody();
  std::unique_ptr<VarDecl> parseVarDecl();
  std::unique_ptr<AssignNode> parseAssign();
  std::unique_ptr<TypeNode> parseType();
  std::unique_ptr<ArrayLiteralNode> parseArrayLiteral();
  std::unique_ptr<IfNode> parseIf();
  std::unique_ptr<WhileNode> parseWhile();
  std::unique_ptr<ReturnNode> parseReturnStmt();
  std::unique_ptr<ExpressionNode> parseExpression();
  std::unique_ptr<ExpressionNode> parseBinaryExpression(int minPrecedence);
  std::unique_ptr<ExpressionNode> parsePrimaryExpression();
  int getPrecedence(TokenType type);
  std::unique_ptr<ParameterNode> parseParameter();
  std::unique_ptr<EnumDecl> parseEnumDecl();
  std::unique_ptr<RecordDecl> parseRecordDecl();
};
} // namespace zap
