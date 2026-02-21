#pragma once
#include "node.hpp"
#include <iostream>
#include <memory>
#include <vector>

class ExpressionNode : public virtual Node {
public:
  virtual ~ExpressionNode() = default;
  ExpressionNode() = default;
};