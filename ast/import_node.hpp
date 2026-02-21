#pragma once
#include "node.hpp"
#include "top_level.hpp"
#include "visitor.hpp"
#include <iostream>
#include <memory>
#include <string>
#include <vector>
class ImportNode : public TopLevel {
public:
  std::vector<std::string> path;
  ImportNode() = default;
  ImportNode(const std::vector<std::string> &importPath) : path(importPath) {}
  ~ImportNode() override = default;

  void accept(Visitor &v) override { v.visit(*this); }
};