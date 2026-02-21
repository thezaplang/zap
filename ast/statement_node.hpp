#pragma once
#include "node.hpp"
#include <iostream>
#include <memory>
#include <vector>

class StatementNode : public virtual Node {
public:
  virtual ~StatementNode() = default;
};