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
  bool isRef = false;
  bool isVariadic = false;

  ParameterNode(const std::string &name, std::unique_ptr<TypeNode> type,
                bool isRef = false, bool isVariadic = false)
      : name(name), type(std::move(type)), isRef(isRef), isVariadic(isVariadic) {}

  void accept(Visitor &v) override { v.visit(*this); }
};
