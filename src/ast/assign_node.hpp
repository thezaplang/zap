#pragma once
#include "expr_node.hpp"
#include "statement_node.hpp"
#include <string>

#include "visitor.hpp"

class AssignNode : public StatementNode {
public:
  std::unique_ptr<ExpressionNode> target_;
  std::unique_ptr<ExpressionNode> expr_;
  AssignNode() noexcept = default;

  AssignNode(std::unique_ptr<ExpressionNode> target,
             std::unique_ptr<ExpressionNode> expr)
      : target_(std::move(target)), expr_(std::move(expr)) {}

  void accept(Visitor &v) override { v.visit(*this); }
};