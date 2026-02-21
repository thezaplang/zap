#pragma once
#include "node.hpp"
#include "visitor.hpp"

class BreakNode : public StatementNode {
public:
  BreakNode() = default;

  void accept(Visitor &v) override { v.visit(*this); }
};