#pragma once
#include "../visitor.hpp"

class ConstNull : public ExpressionNode {
public:
  void accept(Visitor &v) override { v.visit(*this); }
};
