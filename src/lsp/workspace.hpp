#pragma once

#include "frontend/module_loader.hpp"
#include "frontend/project_configuration.hpp"
#include "lsp/source_manager.hpp"
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
  SourceManager sourceManager_;
  std::set<std::string> publishedDiagnosticUris_;
  zap::frontend::RuntimePaths runtimePaths_;
  std::unordered_map<std::string, zap::frontend::ProjectConfigurationResult>
      projectConfigurations_;
  std::unordered_map<std::string, std::shared_ptr<const SemanticSnapshot>>
      strictSnapshots_;
  std::unordered_map<std::string, std::shared_ptr<const SemanticSnapshot>>
      tolerantSnapshots_;

  void appendDiagnostics(AnalysisResult &result,
                         const std::vector<zap::Diagnostic> &diagnostics,
                         const std::string &fallbackUri) const;
  void clearStaleDiagnostics(AnalysisResult &result);
  std::shared_ptr<const SemanticSnapshot>
  buildSnapshot(const SourceSnapshot &document, bool allowEntryErrors);
  const zap::frontend::ProjectConfigurationResult *
  projectConfigurationFor(const std::filesystem::path &documentPath);
  void invalidateSnapshots(const std::string &uri);
  void invalidateSnapshotsForPath(const std::filesystem::path &path);

public:
  Workspace();
  void configure();
  AnalysisResult workspaceFoldersChanged();
  const SourceSnapshot *document(const std::string &uri) const;
  void open(const std::string &uri, std::filesystem::path path,
            std::string text, int64_t version);
  void update(const std::string &uri, std::string text, int64_t version);
  void close(const std::string &uri);
  bool contains(const std::string &uri) const;
  std::shared_ptr<const ProjectState>
  loadProject(const std::string &uri, bool allowEntryErrors = false);
  std::optional<SemanticQuery> query(const std::string &uri,
                                     bool allowEntryErrors = true);
  std::optional<std::string> sourceForUri(const std::string &uri);
  AnalysisResult
  watchedFilesChanged(const std::vector<std::filesystem::path> &paths);
  AnalysisResult analyze(const std::string &uri);
};

} // namespace zap::lsp
