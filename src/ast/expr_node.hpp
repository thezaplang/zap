#pragma once
#include "node.hpp"
#include "visitor.hpp"

class ExpressionNode : public virtual Node {
public:
    virtual ~ExpressionNode() noexcept = default;
    ExpressionNode() noexcept = default;

    void accept(Visitor& v) override { v.visit(*this); }
};
