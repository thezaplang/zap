#pragma once
#include "type_node.hpp"
#include <memory>
#include <string>

struct GenericConstraint {
  std::string parameterName;
  std::unique_ptr<TypeNode> boundType;
};
