#pragma once

#include "sema/module_info.hpp"
#include "sema/semantic_info.hpp"
#include "utils/diagnostics.hpp"
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace zap::lsp {

struct DocumentState {
  std::string uri;
  std::filesystem::path path;
  std::string text;
  int64_t version = 0;
};

struct AnalysisResult {
  std::unordered_map<std::string, std::vector<zap::Diagnostic>>
      diagnosticsByUri;
};

struct ProjectState {
  std::map<std::string, std::unique_ptr<sema::ModuleInfo>> moduleMap;
  std::unordered_map<std::string, std::string> uriByModuleId;
  sema::SemanticInfo semanticInfo;
  AnalysisResult analysis;
};

struct LspSignature {
  std::string label;
  std::vector<std::string> parameters;
};

struct CallContext {
  std::string callee;
  int64_t activeParameter = 0;
};

struct HoverInfo {
  std::string language = "zap";
  std::string value;
};

} // namespace zap::lsp
