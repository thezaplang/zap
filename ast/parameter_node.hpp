#pragma once
#include "node.hpp"
#include "type_node.hpp"
#include <vector>
#include <memory>
#include <iostream>

class ParameterNode : public Node {
public:
    std::string name;
    std::unique_ptr<TypeNode> type;
};