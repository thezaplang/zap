#pragma once
#include "statement_node.hpp"
#include "expr_node.hpp"
#include <vector>
#include <memory>
#include <iostream>


class ReturnNode : public StatementNode {
public:
    std::unique_ptr<ExpressionNode> returnValue;
    ReturnNode() = default;
    ReturnNode(std::unique_ptr<ExpressionNode> value) : returnValue(std::move(value)) {}
    ~ReturnNode() override = default;
};
