#pragma once
#include "body_node.hpp"
#include "expr_node.hpp"
#include "statement_node.hpp"
#include "visitor.hpp"
#include <memory>

class WhileNode : public StatementNode {
public:
  std::unique_ptr<ExpressionNode> condition_;
  std::unique_ptr<BodyNode> body_;
  WhileNode() = default;
  WhileNode(std::unique_ptr<ExpressionNode> condition,
            std::unique_ptr<BodyNode> body)
      : condition_(std::move(condition)), body_(std::move(body)) {}

  void accept(Visitor &v) override { v.visit(*this); }
};