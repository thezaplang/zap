#pragma once
#include "statement_node.hpp"
#include "visitor.hpp"

class ContinueNode : public StatementNode {
public:
  ContinueNode() = default;

  void accept(Visitor &v) override { v.visit(*this); }
};