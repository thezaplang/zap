#pragma once
#include "expr_node.hpp"
#include <memory>
#include <vector>

class ArrayLiteralNode : public ExpressionNode {
public:
  std::vector<std::unique_ptr<ExpressionNode>> elements_;

  ArrayLiteralNode() = default;
  explicit ArrayLiteralNode(
      std::vector<std::unique_ptr<ExpressionNode>> elements)
      : elements_(std::move(elements)) {}
};
