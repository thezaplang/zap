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
  bool isUnsafe_ = false;

  StructDeclarationNode() = default;
  StructDeclarationNode(std::string name, std::vector<std::unique_ptr<ParameterNode>> fields,
                        bool isUnsafe = false)
      : name_(std::move(name)), fields_(std::move(fields)), isUnsafe_(isUnsafe) {}

  void accept(Visitor &v) override { v.visit(*this); }
};
