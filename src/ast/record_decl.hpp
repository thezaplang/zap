#pragma once
#include "generic_constraint.hpp"
#include "node.hpp"
#include "parameter_node.hpp"
#include "top_level.hpp"
#include "type_node.hpp"
#include "visitor.hpp"
#include <memory>
#include <string>
#include <vector>

class RecordDecl : public TopLevel {
public:
  std::string name_;
  std::vector<std::unique_ptr<TypeNode>> genericParams_;
  std::vector<GenericConstraint> genericConstraints_;
  std::vector<std::unique_ptr<ParameterNode>> fields_;

  RecordDecl() = default;
  RecordDecl(const std::string &name,
             std::vector<std::unique_ptr<TypeNode>> genericParams,
             std::vector<std::unique_ptr<ParameterNode>> fields)
      : name_(name), genericParams_(std::move(genericParams)),
        fields_(std::move(fields)) {}

  void accept(Visitor &v) override { v.visit(*this); }
};
