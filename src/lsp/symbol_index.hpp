#pragma once

#include "ast/root_node.hpp"
#include "lsp.hpp"
#include <string>
#include <vector>

namespace zap::lsp {

std::vector<LspSymbol> collectLocalSymbols(const RootNode &root, size_t offset,
                                           const std::string &uri);

} // namespace zap::lsp
