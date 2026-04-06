#pragma once
#include "body_node.hpp"
#include "visitor.hpp"

class UnsafeBlockNode : public BodyNode {
public:
  void accept(Visitor &v) override { v.visit(*this); }
};
