#pragma once
#include "expr_node.hpp"
#include "visitor.hpp"
#include <memory>
#include <string>

class MemberAccessNode : public ExpressionNode {
public:
  std::unique_ptr<ExpressionNode> left_;
  std::string member_;

  MemberAccessNode(std::unique_ptr<ExpressionNode> left, std::string member)
      : left_(std::move(left)), member_(std::move(member)) {}

  void accept(Visitor &v) override { v.visit(*this); }
};
