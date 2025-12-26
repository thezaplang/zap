#pragma once
#include "../expr_node.hpp"

class ConstId : public ExpressionNode{
public:
    std::string value_;
    ConstId() = default;
    ConstId(std::string value) : value_(value) {}
};