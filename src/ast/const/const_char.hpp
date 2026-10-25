#pragma once
#include "../visitor.hpp"

class ConstChar : public ExpressionNode {
public:
    std::string value_;
    ConstChar() = default;

    ConstChar(std::string v)
        : value_(std::move(v)) {}

    void accept(Visitor& v) override { v.visit(*this); }
};
