#pragma once
#include "../expr_node.hpp"

class ConstFloat : public ExpressionNode {
public:
    float value_;
    ConstFloat() = default;
    ConstFloat(float value) : value_(value){}
};