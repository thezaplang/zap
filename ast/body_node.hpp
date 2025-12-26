#pragma once
#include "node.hpp"
#include <vector>
#include <memory>
#include <iostream>

class BodyNode : public Node {
public:
    std::vector<std::unique_ptr<Node>> statements;
    BodyNode() = default;
    ~BodyNode() override = default;
    void addStatement(std::unique_ptr<Node> statement) {
        if(statement) {
            statements.push_back(std::move(statement));
        } else {
            std::cerr << "Cannot add a null statement to BodyNode" << std::endl;
        }
    }
};