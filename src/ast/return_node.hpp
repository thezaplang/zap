#pragma once
#include "expr_node.hpp"
#include "statement_node.hpp"
#include "visitor.hpp"
#include <iostream>
#include <memory>
#include <vector>

class ReturnNode : public StatementNode {
public:
  std::unique_ptr<ExpressionNode> returnValue;

  ReturnNode() noexcept = default;
  ReturnNode(std::unique_ptr<ExpressionNode> value)
      : returnValue(std::move(value)) {}

  ~ReturnNode() noexcept override = default;

  void accept(Visitor &v) override { v.visit(*this); }
};
