#pragma once
#include "expr_node.hpp"
#include "statement_node.hpp"
#include "type_node.hpp"
#include "visitor.hpp"
#include <string>

class VarDecl : public StatementNode {
public:
  std::string name_;
  std::unique_ptr<TypeNode> type_;
  std::unique_ptr<ExpressionNode> initializer_;

  VarDecl() = default;
  VarDecl(std::string name, std::unique_ptr<TypeNode> type,
          std::unique_ptr<ExpressionNode> initializer)
      : name_(name), type_(std::move(type)),
        initializer_(std::move(initializer)) {}

  void accept(Visitor &v) override { v.visit(*this); }
};