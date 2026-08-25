#pragma once
#include "fun_decl.hpp"
#include "top_level.hpp"
#include <memory>
#include <string>
#include <vector>

class InterfaceDecl : public TopLevel {
public:
  std::string name_;
  std::vector<std::unique_ptr<FunDecl>> methods_;

  InterfaceDecl() = default;
  explicit InterfaceDecl(std::string name) : name_(std::move(name)) {}

  void accept(Visitor &v) override { v.visit(*this); }
};
