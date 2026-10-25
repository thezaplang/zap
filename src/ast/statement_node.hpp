#pragma once
#include "node.hpp"
#include "visitor.hpp"

class StatementNode : public virtual Node {
public:
    virtual ~StatementNode() noexcept = default;

    void accept(Visitor& v) override { v.visit(*this); }
};
