#include "lsp/workspace.hpp"

#include "frontend/frontend_session.hpp"
#include "frontend/project_configuration.hpp"
#include "lsp/protocol_utils.hpp"
#include "sema/binder.hpp"
#include <utility>
#include <vector>

namespace zap::lsp {

namespace {

void appendConfigurationDiagnostics(AnalysisResult &analysis,
                                    const SourceSnapshot &document,
                                    const std::vector<std::string> &errors) {
  auto &diagnostics = analysis.diagnosticsByUri[document.uri];
  for (const auto &error : errors) {
    diagnostics.push_back({zap::DiagnosticLevel::Error,
                           "",
                           error,
                           document.path.string(),
                           document.text,
                           {},
                           {}});
  }
}

} // namespace

Workspace::Workspace()
    : runtimePaths_{
          std::filesystem::path(), std::filesystem::path(ZAPC_CORE_DIR),
          std::filesystem::path(ZAPC_STDLIB_DIR), std::filesystem::path(),
          zap::frontend::EnvironmentOverrides::Ignore} {}

void Workspace::configure() {
  projectConfigurations_.clear();
}

AnalysisResult Workspace::workspaceFoldersChanged() {
  projectConfigurations_.clear();
  strictSnapshots_.clear();
  tolerantSnapshots_.clear();

  AnalysisResult result;
  for (const auto &uri : sourceManager_.openUris()) {
    auto project = loadProject(uri);
    if (!project) {
      continue;
    }
    for (const auto &[diagnosticUri, diagnostics] :
         project->analysis.diagnosticsByUri) {
      auto &merged = result.diagnosticsByUri[diagnosticUri];
      merged.insert(merged.end(), diagnostics.begin(), diagnostics.end());
    }
    if (result.diagnosticsByUri.count(uri) == 0) {
      result.diagnosticsByUri[uri] = {};
    }
  }
  clearStaleDiagnostics(result);
  return result;
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

AnalysisResult Workspace::watchedFilesChanged(
    const std::vector<std::filesystem::path> &paths) {
  std::set<std::string> changedProjects;
  bool changedUnconfiguredFile = false;
  for (const auto &path : paths) {
    const auto canonical = std::filesystem::weakly_canonical(path).string();
    sourceManager_.invalidatePath(path);
    invalidateSnapshotsForPath(path);
    if (path.filename() == "thor.toml") {
      projectConfigurations_.erase(canonical);
      changedProjects.insert(canonical);
      continue;
    }
    if (const auto manifest =
            zap::frontend::findProjectConfigurationManifest(path)) {
      changedProjects.insert(manifest->string());
    } else {
      changedUnconfiguredFile = true;
    }
  }

  std::vector<std::string> affectedUris;
  for (const auto &uri : sourceManager_.openUris()) {
    const auto *document = this->document(uri);
    if (!document) {
      continue;
    }
    const auto manifest =
        zap::frontend::findProjectConfigurationManifest(document->path);
    if ((manifest && changedProjects.count(manifest->string()) != 0) ||
        (!manifest && changedUnconfiguredFile)) {
      invalidateSnapshots(uri);
      affectedUris.push_back(uri);
    }
  }

  AnalysisResult result;
  for (const auto &uri : affectedUris) {
    auto project = loadProject(uri);
    if (!project) {
      continue;
    }
    for (const auto &[diagnosticUri, diagnostics] :
         project->analysis.diagnosticsByUri) {
      auto &merged = result.diagnosticsByUri[diagnosticUri];
      merged.insert(merged.end(), diagnostics.begin(), diagnostics.end());
    }
    if (result.diagnosticsByUri.count(uri) == 0) {
      result.diagnosticsByUri[uri] = {};
    }
  }
  clearStaleDiagnostics(result);
  return result;
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

const zap::frontend::ProjectConfigurationResult *
Workspace::projectConfigurationFor(const std::filesystem::path &documentPath) {
  const auto manifest =
      zap::frontend::findProjectConfigurationManifest(documentPath);
  if (!manifest) {
    return nullptr;
  }
  const auto key = manifest->string();
  auto configuration = projectConfigurations_.find(key);
  if (configuration == projectConfigurations_.end()) {
    configuration = projectConfigurations_
                        .emplace(key,
                                 zap::frontend::loadProjectConfiguration(
                                     *manifest))
                        .first;
  }
  return &configuration->second;
}

std::shared_ptr<const SemanticSnapshot>
Workspace::buildSnapshot(const SourceSnapshot &document,
                         bool allowEntryErrors) {
  auto snapshot = std::make_shared<SemanticSnapshot>();
  snapshot->documentVersion = document.version;
  const auto configuration = projectConfigurationFor(document.path);
  const auto importMap = configuration && configuration->configuration
                             ? configuration->configuration->importMap
                             : zap::frontend::ImportMap{};
  if (configuration) {
    appendConfigurationDiagnostics(snapshot->project.analysis, document,
                                   configuration->errors);
  }

  zap::frontend::FrontendSession session(
      {runtimePaths_, importMap, true, allowEntryErrors},
      [this](const std::filesystem::path &path) -> std::optional<std::string> {
        auto source = sourceManager_.sourceForPath(path);
        return source ? std::optional<std::string>((*source)->text)
                      : std::nullopt;
      });
  auto project = session.load(document.path);
  snapshot->project.dependencyModuleIds = std::move(project.visitedModuleIds);
  if (!project.modules.empty()) {
    session.bind(project);
  }
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
