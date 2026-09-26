#pragma once

#include "frontend/macro_registry.hpp"
#include "frontend/module_loader.hpp"
#include "frontend/module_outline.hpp"
#include "sema/bound_nodes.hpp"
#include "sema/module_info.hpp"
#include "sema/semantic_info.hpp"
#include "sema/target_info.hpp"
#include "utils/diagnostics.hpp"
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zap::frontend {

struct FrontendSessionConfig {
  RuntimePaths runtimePaths;
  ImportMap importMap;
  bool includePrelude = true;
  bool allowEntryErrors = false;
  sema::TargetInfo targetInfo{};
};

struct FrontendProject {
  std::string entryModuleId;
  std::map<std::string, std::unique_ptr<sema::ModuleInfo>> modules;
  std::map<std::string, ModuleOutline> outlines;
  MacroRegistrySet macros;
  std::unordered_set<std::string> visitedModuleIds;
  std::vector<Diagnostic> diagnostics;
  std::vector<std::string> errors;
  sema::SemanticInfo semanticInfo;
  std::unique_ptr<sema::BoundRootNode> boundRoot;
  bool loaded = false;
};

using SourceLoader = std::function<std::optional<std::string>(
    const std::filesystem::path &canonicalPath)>;

class FrontendSession {
public:
  FrontendSession(FrontendSessionConfig config, SourceLoader sourceLoader);

  FrontendProject load(const std::filesystem::path &entryPath);
  bool bind(FrontendProject &project);

private:
  FrontendSessionConfig config_;
  SourceLoader sourceLoader_;
};

} // namespace zap::frontend
