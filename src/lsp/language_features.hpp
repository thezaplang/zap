#pragma once

#include "lsp.hpp"
#include "workspace.hpp"
#include "workspace_types.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace zap::lsp {

JsonObject::List makeCompletionItems(const std::string &uri,
                                     const std::string &source,
                                     const ProjectState &project,
                                     size_t offset);
std::optional<LspSymbol> resolveDefinition(const Workspace &workspace,
                                           const std::string &uri,
                                           const ProjectState &project,
                                           size_t offset);
std::optional<HoverInfo> resolveHover(const std::string &source,
                                      const std::string &uri,
                                      const ProjectState &project,
                                      size_t offset);
std::vector<LspSignature> resolveSignatures(const std::string &source,
                                            const std::string &uri,
                                            const ProjectState &project,
                                            size_t offset,
                                            int64_t &activeParameter);
int64_t chooseActiveSignature(const std::vector<LspSignature> &signatures,
                              int64_t activeParameter);

} // namespace zap::lsp
