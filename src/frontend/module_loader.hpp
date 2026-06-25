#pragma once

#include "ast/import_node.hpp"
#include "sema/module_info.hpp"
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace zap::frontend {

using ImportMap = std::unordered_map<std::string, std::string>;

struct RuntimePaths {
  std::filesystem::path executablePath;
  std::filesystem::path configuredCoreDir;
  std::filesystem::path configuredStdlibDir;
  std::filesystem::path configuredStdlibObject;
};

std::optional<std::filesystem::path>
currentExecutablePath(const std::filesystem::path &argv0Hint);

std::filesystem::path stdlibRootPath(const RuntimePaths &paths);
std::filesystem::path coreRootPath(const RuntimePaths &paths);
std::filesystem::path stdlibObjectPath(const RuntimePaths &paths);

std::string stripSourceExtension(const std::filesystem::path &path);
std::string computeLogicalModulePath(const std::filesystem::path &canonicalPath,
                                     const RuntimePaths &paths,
                                     const ImportMap &importMap = {});

bool hasImplicitImport(const RootNode &root, std::string_view path);
void injectImplicitPreludeImportIfNeeded(sema::ModuleInfo &module,
                                         bool includePrelude);

bool resolveImportTargets(const std::filesystem::path &modulePath,
                          const ImportNode &importNode,
                          std::vector<std::filesystem::path> &targets,
                          const ImportMap &importMap, const RuntimePaths &paths,
                          std::string *errorMessage = nullptr);

sema::ResolvedImport
makeResolvedImport(const ImportNode &importNode,
                   const std::vector<std::filesystem::path> &targets);

} // namespace zap::frontend
