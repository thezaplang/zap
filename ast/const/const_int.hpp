#pragma once
#include "../expr_node.hpp"

class ConstInt : public ExpressionNode {
public:
    int value_;
    std::string typeName_ = "i32";
    ConstInt() = default;
    ConstInt(int value, std::string typeName = "i32") : value_(value), typeName_(typeName){}
};