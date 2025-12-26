#pragma once
#include "node.hpp"
#include <vector>
#include <memory>
#include <iostream>

class StatementNode : public Node {
public:
    virtual ~StatementNode() = default;
};