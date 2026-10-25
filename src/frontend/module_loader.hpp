#pragma once

#include "ast/import_node.hpp"
#include "sema/module_info.hpp"
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace zap::frontend {

using ImportMap = std::unordered_map<std::string, std::string>;

enum class EnvironmentOverrides {
    Allow,
    Ignore
};

struct RuntimePaths {
    std::filesystem::path executablePath;
    std::filesystem::path configuredCoreDir;
    std::filesystem::path configuredStdlibDir;
    std::filesystem::path configuredStdlibObject;
    EnvironmentOverrides environmentOverrides;
    std::optional<std::filesystem::path> coreDirOverride;
    std::optional<std::filesystem::path> stdlibDirOverride;

    RuntimePaths(
        std::filesystem::path executablePath,
        std::filesystem::path configuredCoreDir,
        std::filesystem::path configuredStdlibDir,
        std::filesystem::path configuredStdlibObject,
        EnvironmentOverrides environmentOverrides = EnvironmentOverrides::Allow
    )
        : executablePath(std::move(executablePath)),
          configuredCoreDir(std::move(configuredCoreDir)),
          configuredStdlibDir(std::move(configuredStdlibDir)),
          configuredStdlibObject(std::move(configuredStdlibObject)),
          environmentOverrides(environmentOverrides) {}
};

std::optional<std::filesystem::path> currentExecutablePath(const std::filesystem::path& argv0Hint);

std::filesystem::path stdlibRootPath(const RuntimePaths& paths);
std::filesystem::path coreRootPath(const RuntimePaths& paths);
std::filesystem::path stdlibObjectPath(const RuntimePaths& paths);

std::string stripSourceExtension(const std::filesystem::path& path);
std::string computeLogicalModulePath(
    const std::filesystem::path& canonicalPath,
    const RuntimePaths& paths,
    const ImportMap& importMap = {}
);

bool hasImplicitImport(const RootNode& root, std::string_view path);
bool shouldIncludeImplicitPrelude(std::string_view logicalPath, bool includePrelude);
void injectImplicitPreludeImportIfNeeded(sema::ModuleInfo& module, bool includePrelude);

bool resolveImportTargets(
    const std::filesystem::path& modulePath,
    const ImportNode& importNode,
    std::vector<std::filesystem::path>& targets,
    const ImportMap& importMap,
    const RuntimePaths& paths,
    std::string* errorMessage = nullptr
);

sema::ResolvedImport makeResolvedImport(
    const ImportNode& importNode,
    const std::vector<std::filesystem::path>& targets
);

} // namespace zap::frontend
