#pragma once
#include "expr_node.hpp"
#include "visitor.hpp"
#include <memory>

class IndexAccessNode : public ExpressionNode {
public:
  std::unique_ptr<ExpressionNode> left_;
  std::unique_ptr<ExpressionNode> index_;

  IndexAccessNode(std::unique_ptr<ExpressionNode> left,
                  std::unique_ptr<ExpressionNode> index)
      : left_(std::move(left)), index_(std::move(index)) {}

  void accept(Visitor &v) override { v.visit(*this); }
};
