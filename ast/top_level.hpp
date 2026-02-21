#pragma once
#include "node.hpp"
#include "visitor.hpp"

class TopLevel : public Node {
public:
  virtual ~TopLevel() = default;

  void accept(Visitor &v) override { v.visit(*this); }
};