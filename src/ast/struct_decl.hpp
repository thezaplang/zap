#pragma once
#include "top_level.hpp"
#include "parameter_node.hpp"
#include <memory>
#include <string>
#include <vector>

class StructDeclarationNode : public TopLevel {
public:
  std::string name_;
  std::vector<std::unique_ptr<ParameterNode>> fields_;

  StructDeclarationNode() = default;
  StructDeclarationNode(std::string name, std::vector<std::unique_ptr<ParameterNode>> fields)
      : name_(std::move(name)), fields_(std::move(fields)) {}

  void accept(Visitor &v) override { v.visit(*this); }
};
