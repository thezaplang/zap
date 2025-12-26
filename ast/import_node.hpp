#pragma once
#include "node.hpp"
#include "top_level.hpp"
#include <vector>
#include <memory>
#include <iostream>
#include <string>
class ImportNode : public TopLevel {
public:
    std::vector<std::string> path;
    ImportNode() = default;
    ImportNode(const std::vector<std::string>& importPath) : path(importPath) {}
    ~ImportNode() override = default;
};