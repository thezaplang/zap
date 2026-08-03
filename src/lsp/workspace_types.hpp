#pragma once

#include "sema/bound_nodes.hpp"
#include "sema/module_info.hpp"
#include "sema/semantic_info.hpp"
#include "lsp/source_manager.hpp"
#include "utils/diagnostics.hpp"
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zap::lsp {

struct AnalysisResult {
  std::unordered_map<std::string, std::vector<zap::Diagnostic>>
      diagnosticsByUri;
};

struct ProjectState {
  std::map<std::string, std::unique_ptr<sema::ModuleInfo>> moduleMap;
  std::unique_ptr<sema::BoundRootNode> boundRoot;
  std::unordered_map<std::string, std::string> uriByModuleId;
  std::unordered_set<std::string> dependencyModuleIds;
  sema::SemanticInfo semanticInfo;
  AnalysisResult analysis;
};

struct SemanticSnapshot {
  int64_t documentVersion = 0;
  ProjectState project;
};

struct SemanticQuery {
  SourceManager::Snapshot document;
  std::shared_ptr<const ProjectState> project;
};

struct LspSignature {
  std::string label;
  std::vector<std::string> parameters;
};

struct CallContext {
  std::string callee;
  int64_t activeParameter = 0;
  bool isConstructor = false;
};

struct HoverInfo {
  std::string language = "zap";
  std::string value;
};

} // namespace zap::lsp
