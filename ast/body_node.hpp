#pragma once
#include "expr_node.hpp"
#include <iostream>
#include <memory>
#include <vector>

class BodyNode : public ExpressionNode {
public:
  std::vector<std::unique_ptr<Node>> statements;
  std::unique_ptr<ExpressionNode> result;

  BodyNode() = default;
  ~BodyNode() override = default;

  void addStatement(std::unique_ptr<Node> statement) {
    if (statement) {
      statements.push_back(std::move(statement));
    } else {
      std::cerr << "Cannot add a null statement to BodyNode" << std::endl;
    }
  }

  void setResult(std::unique_ptr<ExpressionNode> res) {
    result = std::move(res);
  }
};