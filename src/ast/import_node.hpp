#pragma once
#include "node.hpp"
#include "top_level.hpp"
#include "visitor.hpp"
#include <string>
#include <vector>

struct ImportBinding {
  std::string sourceName;
  std::string localName;
};

class ImportNode : public TopLevel {
public:
  std::string path;
  std::string moduleAlias;
  std::vector<ImportBinding> bindings;

  ImportNode() noexcept = default;
  ImportNode(std::string importPath, std::string alias = "",
             std::vector<ImportBinding> importBindings = {})
      : path(std::move(importPath)), moduleAlias(std::move(alias)),
        bindings(std::move(importBindings)) {}

  ~ImportNode() noexcept override = default;

  void accept(Visitor &v) override { v.visit(*this); }
};
