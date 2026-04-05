#pragma once

#include "../ast/root_node.hpp"
#include "../token/token.hpp"
#include "../visibility.hpp"
#include <memory>
#include <string>
#include <vector>

namespace sema {

struct ResolvedImport {
  struct Binding {
    std::string sourceName;
    std::string localName;
  };

  std::string rawPath;
  std::string moduleAlias;
  std::vector<std::string> targetModuleIds;
  std::vector<Binding> bindings;
  Visibility visibility = Visibility::Private;
  SourceSpan span;
};

struct ModuleInfo {
  std::string moduleId;
  std::string moduleName;
  std::string linkPath;
  std::string sourceName;
  bool isEntry = false;
  std::unique_ptr<RootNode> root;
  std::vector<ResolvedImport> imports;
};

} // namespace sema
