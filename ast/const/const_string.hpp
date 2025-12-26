#pragma once
#include "../expr_node.hpp"

class ConstString : public ExpressionNode{
public:
    std::string value_;
    ConstString() = default;
    ConstString(std::string value) : value_(value){}
};