#pragma once
#include "node.hpp"
#include <vector>
#include <memory>
#include <iostream>

class ExpressionNode : public Node {
public:
    virtual ~ExpressionNode() = default;
    ExpressionNode() = default;
};