#pragma once
#include "top_level.hpp"
#include "type_node.hpp"
#include <memory>
#include <string>

class TypeAliasDecl : public TopLevel {
public:
  std::string name_;
  std::unique_ptr<TypeNode> type_;

  TypeAliasDecl() = default;
  TypeAliasDecl(const std::string &name, std::unique_ptr<TypeNode> type)
      : name_(name), type_(std::move(type)) {}

  void accept(Visitor &v) override { v.visit(*this); }
};
