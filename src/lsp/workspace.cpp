#include "lsp/workspace.hpp"

#include "frontend/frontend_session.hpp"
#include "lsp/configuration.hpp"
#include "lsp/protocol_utils.hpp"
#include "sema/binder.hpp"
#include <utility>
#include <vector>

namespace zap::lsp {

Workspace::Workspace()
    : runtimePaths_{
          std::filesystem::path(), std::filesystem::path(ZAPC_CORE_DIR),
          std::filesystem::path(ZAPC_STDLIB_DIR), std::filesystem::path(),
          zap::frontend::EnvironmentOverrides::Ignore} {}

std::vector<std::string>
Workspace::configure(const std::filesystem::path &workspaceRoot,
                     const std::optional<std::string> &corePath,
                     const std::optional<std::string> &stdlibPath) {
  auto configuration =
      loadRuntimePathConfiguration(workspaceRoot, corePath, stdlibPath);
  if (configuration.coreDir) {
    runtimePaths_.coreDirOverride = std::move(*configuration.coreDir);
  }
  if (configuration.stdlibDir) {
    runtimePaths_.stdlibDirOverride = std::move(*configuration.stdlibDir);
  }
  return configuration.errors;
}

const SourceSnapshot *Workspace::document(const std::string &uri) const {
  return sourceManager_.document(uri);
}

void Workspace::open(const std::string &uri, std::filesystem::path path,
                     std::string text, int64_t version) {
  sourceManager_.open(uri, std::move(path), std::move(text), version);
  if (const auto *document = this->document(uri)) {
    invalidateSnapshotsForPath(document->path);
  }
}

void Workspace::update(const std::string &uri, std::string text,
                       int64_t version) {
  sourceManager_.update(uri, std::move(text), version);
  if (const auto *document = this->document(uri)) {
    invalidateSnapshotsForPath(document->path);
  }
}

void Workspace::close(const std::string &uri) {
  const auto *document = this->document(uri);
  const auto path = document ? std::optional<std::filesystem::path>(document->path)
                             : std::nullopt;
  sourceManager_.close(uri);
  if (path) {
    invalidateSnapshotsForPath(*path);
  } else {
    invalidateSnapshots(uri);
  }
}

bool Workspace::contains(const std::string &uri) const {
  return sourceManager_.contains(uri);
}

std::optional<std::string> Workspace::sourceForUri(const std::string &uri) {
  auto source = sourceManager_.sourceForUri(uri);
  return source ? std::optional<std::string>((*source)->text) : std::nullopt;
}

void Workspace::appendDiagnostics(
    AnalysisResult &result, const std::vector<zap::Diagnostic> &diagnostics,
    const std::string &fallbackUri) const {
  for (const auto &diagnostic : diagnostics) {
    std::string uri = fallbackUri;
    if (!diagnostic.fileName.empty()) {
      uri = sourceManager_.uriForPath(diagnostic.fileName);
    }
    result.diagnosticsByUri[uri].push_back(diagnostic);
  }
}

void Workspace::clearStaleDiagnostics(AnalysisResult &result) {
  std::set<std::string> currentUris;
  for (const auto &[uri, _] : result.diagnosticsByUri) {
    currentUris.insert(uri);
  }
  for (const auto &uri : publishedDiagnosticUris_) {
    if (currentUris.count(uri) == 0) {
      result.diagnosticsByUri[uri] = {};
    }
  }
  publishedDiagnosticUris_ = std::move(currentUris);
}

void Workspace::invalidateSnapshots(const std::string &uri) {
  strictSnapshots_.erase(uri);
  tolerantSnapshots_.erase(uri);
}

void Workspace::invalidateSnapshotsForPath(const std::filesystem::path &path) {
  const auto canonical = std::filesystem::weakly_canonical(path).string();
  auto invalidate = [&canonical](auto &snapshots) {
    for (auto it = snapshots.begin(); it != snapshots.end();) {
      if (it->second->project.dependencyModuleIds.count(canonical) != 0) {
        it = snapshots.erase(it);
      } else {
        ++it;
      }
    }
  };
  invalidate(strictSnapshots_);
  invalidate(tolerantSnapshots_);
}

std::shared_ptr<const SemanticSnapshot>
Workspace::buildSnapshot(const SourceSnapshot &document,
                         bool allowEntryErrors) {
  auto snapshot = std::make_shared<SemanticSnapshot>();
  snapshot->documentVersion = document.version;
  auto flags = findAndReadFlags(document.path);

  zap::frontend::FrontendSession session(
      {runtimePaths_, flags.importMap, true, allowEntryErrors},
      [this](const std::filesystem::path &path) -> std::optional<std::string> {
        auto source = sourceManager_.sourceForPath(path);
        return source ? std::optional<std::string>((*source)->text)
                      : std::nullopt;
      });
  auto project = session.load(document.path);
  snapshot->project.dependencyModuleIds = std::move(project.visitedModuleIds);
  if (!project.loaded) {
    appendDiagnostics(snapshot->project.analysis, project.diagnostics,
                      sourceManager_.uriForPath(document.path));
    return snapshot;
  }
  session.bind(project);
  appendDiagnostics(snapshot->project.analysis, project.diagnostics,
                    sourceManager_.uriForPath(document.path));
  snapshot->project.boundRoot = std::move(project.boundRoot);
  snapshot->project.semanticInfo = std::move(project.semanticInfo);
  snapshot->project.moduleMap = std::move(project.modules);
  for (const auto &[moduleId, _] : snapshot->project.moduleMap) {
    snapshot->project.uriByModuleId[moduleId] = sourceManager_.uriForPath(moduleId);
  }
  return snapshot;
}

std::shared_ptr<const ProjectState>
Workspace::loadProject(const std::string &uri, bool allowEntryErrors) {
  const auto *document = this->document(uri);
  if (!document) {
    return nullptr;
  }
  auto &snapshots = allowEntryErrors ? tolerantSnapshots_ : strictSnapshots_;
  auto snapshot = snapshots.find(uri);
  if (snapshot != snapshots.end() &&
      snapshot->second->documentVersion == document->version) {
    return std::shared_ptr<const ProjectState>(snapshot->second,
                                               &snapshot->second->project);
  }
  auto built = buildSnapshot(*document, allowEntryErrors);
  snapshots[uri] = std::move(built);
  return std::shared_ptr<const ProjectState>(snapshots[uri],
                                             &snapshots[uri]->project);
}

std::optional<SemanticQuery> Workspace::query(const std::string &uri,
                                               bool allowEntryErrors) {
  auto document = sourceManager_.sourceForUri(uri);
  auto project = loadProject(uri, allowEntryErrors);
  if (!document || !project) {
    return std::nullopt;
  }
  return SemanticQuery{*document, std::move(project)};
}

AnalysisResult Workspace::analyze(const std::string &uri) {
  AnalysisResult result;
  const auto *document = this->document(uri);
  if (!document) {
    return result;
  }

  auto project = loadProject(uri);
  if (!project) {
    return result;
  }
  result = project->analysis;
  if (result.diagnosticsByUri.find(uri) == result.diagnosticsByUri.end()) {
    result.diagnosticsByUri[uri] = {};
  }
  clearStaleDiagnostics(result);
  return result;
}

} // namespace zap::lsp
