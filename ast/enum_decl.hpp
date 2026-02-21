// ast/enum_decl.hpp
#pragma once
#include "node.hpp"
#include "top_level.hpp"
#include "visitor.hpp"
#include <memory>
#include <string>
#include <vector>

class EnumDecl : public TopLevel {
public:
  std::string name_;
  std::vector<std::string> entries_;

  EnumDecl(const std::string &name, std::vector<std::string> entries)
      : name_(name), entries_(std::move(entries)) {}

  void accept(Visitor &v) override { v.visit(*this); }
};
