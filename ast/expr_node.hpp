#pragma once
#include "node.hpp"
#include "visitor.hpp"

class ExpressionNode : public virtual Node {
public:
  virtual ~ExpressionNode() = default;
  ExpressionNode() = default;

  void accept(Visitor &v) override { v.visit(*this); }
};