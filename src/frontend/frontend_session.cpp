#include "frontend/frontend_session.hpp"

#include "ast/import_node.hpp"
#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#include "sema/binder.hpp"

namespace zap::frontend {

FrontendSession::FrontendSession(FrontendSessionConfig config,
                                 SourceLoader sourceLoader)
    : config_(std::move(config)), sourceLoader_(std::move(sourceLoader)) {}

FrontendProject FrontendSession::load(const std::filesystem::path &entryPath) {
  FrontendProject project;
  const auto canonicalEntry = std::filesystem::weakly_canonical(entryPath);
  project.entryModuleId = canonicalEntry.string();
  std::unordered_map<std::string, bool> visiting;
  project.loaded = loadModule(canonicalEntry, canonicalEntry.string(), project,
                              visiting);
  auto entry = project.modules.find(canonicalEntry.string());
  if (entry != project.modules.end()) {
    entry->second->isEntry = true;
  }
  return project;
}

bool FrontendSession::loadModule(
    const std::filesystem::path &modulePath, const std::string &entryModuleId,
    FrontendProject &project, std::unordered_map<std::string, bool> &visiting) {
  const auto canonicalPath = std::filesystem::weakly_canonical(modulePath);
  const auto moduleId = canonicalPath.string();
  project.visitedModuleIds.insert(moduleId);
  if (project.modules.count(moduleId) != 0) {
    return true;
  }
  if (visiting[moduleId]) {
    project.errors.push_back("cyclic import detected involving " + moduleId);
    return false;
  }

  auto source = sourceLoader_(canonicalPath);
  if (!source) {
    project.errors.push_back("couldn't open source file: " + moduleId);
    return false;
  }

  visiting[moduleId] = true;
  DiagnosticEngine diagnostics(*source, moduleId);
  Lexer lexer(diagnostics);
  Parser parser(lexer.tokenize(*source), diagnostics);
  auto root = parser.parse();
  const bool isEntry = moduleId == entryModuleId;
  if (!root ||
      (diagnostics.hadErrors() && !(config_.allowEntryErrors && isEntry))) {
    const auto &moduleDiagnostics = diagnostics.diagnostics();
    project.diagnostics.insert(project.diagnostics.end(),
                               moduleDiagnostics.begin(),
                               moduleDiagnostics.end());
    visiting.erase(moduleId);
    return false;
  }

  auto module = std::make_unique<sema::ModuleInfo>();
  module->moduleId = moduleId;
  module->moduleName = canonicalPath.stem().string();
  module->linkPath =
      computeLogicalModulePath(canonicalPath, config_.runtimePaths, config_.importMap);
  module->sourceName = moduleId;
  module->sourceText = std::move(*source);
  module->root = std::move(root);
  injectImplicitPreludeImportIfNeeded(*module, config_.includePrelude);

  bool complete = true;
  for (const auto &child : module->root->children) {
    auto *importNode = dynamic_cast<ImportNode *>(child.get());
    if (!importNode) {
      continue;
    }
    std::vector<std::filesystem::path> targets;
    std::string error;
    if (!resolveImportTargets(canonicalPath, *importNode, targets,
                              config_.importMap, config_.runtimePaths,
                              &error)) {
      diagnostics.report(importNode->span, DiagnosticLevel::Error, error);
      complete = false;
      continue;
    }
    module->imports.push_back(makeResolvedImport(*importNode, targets));
  }

  for (const auto &import : module->imports) {
    for (const auto &target : import.targetModuleIds) {
      if (!loadModule(target, entryModuleId, project, visiting)) {
        complete = false;
      }
    }
  }

  const auto &moduleDiagnostics = diagnostics.diagnostics();
  project.diagnostics.insert(project.diagnostics.end(),
                             moduleDiagnostics.begin(),
                             moduleDiagnostics.end());
  visiting.erase(moduleId);
  project.modules[moduleId] = std::move(module);
  return complete;
}

bool FrontendSession::bind(FrontendProject &project) {
  if (project.modules.empty()) {
    return false;
  }

  const auto entryIt = project.modules.find(project.entryModuleId);
  if (entryIt == project.modules.end()) {
    return false;
  }
  const auto &entry = *entryIt->second;
  DiagnosticEngine diagnostics(entry.sourceText, entry.sourceName);
  std::vector<sema::ModuleInfo *> modules;
  modules.reserve(project.modules.size());
  for (auto &[_, module] : project.modules) {
    diagnostics.registerSource(module->sourceName, module->sourceText);
    modules.push_back(module.get());
  }

  sema::Binder binder(diagnostics, true, &project.semanticInfo,
                      config_.targetInfo);
  project.boundRoot = binder.bind(std::move(modules));
  const auto &bindingDiagnostics = diagnostics.diagnostics();
  project.diagnostics.insert(project.diagnostics.end(), bindingDiagnostics.begin(),
                             bindingDiagnostics.end());
  return static_cast<bool>(project.boundRoot);
}

} // namespace zap::frontend
