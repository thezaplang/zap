#pragma once
#include "generic_constraint.hpp"
#include "fun_decl.hpp"
#include "parameter_node.hpp"
#include "top_level.hpp"
#include "type_node.hpp"
#include <memory>
#include <vector>

class ClassDecl : public TopLevel {
public:
  std::string name_;
  std::vector<std::unique_ptr<TypeNode>> genericParams_;
  std::vector<GenericConstraint> genericConstraints_;
  std::unique_ptr<TypeNode> baseType_;
  std::vector<std::unique_ptr<ParameterNode>> fields_;
  std::vector<std::unique_ptr<FunDecl>> methods_;

  ClassDecl() = default;
  explicit ClassDecl(std::string name) : name_(std::move(name)) {}

  void accept(Visitor &v) override { v.visit(*this); }
};
