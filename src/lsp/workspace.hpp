#pragma once

#include "driver/args/argparse.hpp"
#include "frontend/module_loader.hpp"
#include "sema/module_info.hpp"
#include "workspace_types.hpp"
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>

namespace zap::lsp {

class Workspace {
  struct CachedFile {
    std::filesystem::file_time_type lastWriteTime;
    std::string content;
  };

  std::unordered_map<std::string, DocumentState> documentsByUri_;
  std::unordered_map<std::string, std::string> uriByCanonicalPath_;
  mutable std::unordered_map<std::string, CachedFile> fileContentCache_;

  zap::frontend::RuntimePaths runtimePaths() const;
  std::optional<std::string>
  sourceForPath(const std::filesystem::path &path) const;
  bool loadModuleGraph(
      const std::filesystem::path &modulePath,
      std::map<std::string, std::unique_ptr<sema::ModuleInfo>> &modules,
      std::set<std::string> &visiting, AnalysisResult &result,
      const std::string &entryUri, const zap::args::ImportMap &importMap,
      bool allowEntryErrors = false) const;

public:
  const DocumentState *document(const std::string &uri) const;
  void open(const std::string &uri, std::filesystem::path path,
            std::string text, int64_t version);
  void update(const std::string &uri, std::string text, int64_t version);
  void close(const std::string &uri);
  bool contains(const std::string &uri) const;
  std::optional<ProjectState> loadProject(const std::string &uri,
                                          bool allowEntryErrors = false) const;
  AnalysisResult analyze(const std::string &uri) const;
};

} // namespace zap::lsp
