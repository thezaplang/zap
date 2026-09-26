#pragma once

#include "ast/import_node.hpp"
#include "macros/macro_definition.hpp"
#include "token/token.hpp"
#include "utils/diagnostics.hpp"

#include <memory>
#include <string>
#include <vector>

namespace zap::frontend {

struct ModuleOutline {
  std::vector<std::unique_ptr<ImportNode>> imports;
  std::vector<MacroDefinition> macros;

  bool hasImportPath(const std::string &path) const;
  static ModuleOutline scan(const std::vector<Token> &tokens,
                            DiagnosticEngine &diagnostics);
};

} // namespace zap::frontend
