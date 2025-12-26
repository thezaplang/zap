#pragma once
#include "statement_node.hpp"
#include "expr_node.hpp"
#include "body_node.hpp"
#include <memory>

class WhileNode : public Node {
public:
    std::unique_ptr<ExpressionNode> condition_;
    std::unique_ptr<BodyNode> body_;
    WhileNode() = default;
    WhileNode(std::unique_ptr<ExpressionNode> condition, std::unique_ptr<BodyNode> body): 
    condition_(std::move(condition)), 
    body_(std::move(body)){}
};