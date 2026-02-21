#pragma once
#include "node.hpp"
#include "type_node.hpp"
#include "visitor.hpp"
#include <iostream>
#include <memory>
#include <vector>

class ParameterNode : public Node {
public:
  std::string name;
  std::unique_ptr<TypeNode> type;

  ParameterNode(const std::string &name, std::unique_ptr<TypeNode> type)
      : name(name), type(std::move(type)) {}

  void accept(Visitor &v) override { v.visit(*this); }
};