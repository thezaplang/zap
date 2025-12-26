#pragma once
#include "node.hpp"
#include <iostream>
#include <memory>
#include <vector>

class ExpressionNode : public Node {
public:
  virtual ~ExpressionNode() = default;
  ExpressionNode() = default;
};