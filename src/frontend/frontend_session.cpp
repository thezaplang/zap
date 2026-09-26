#include "frontend/frontend_session.hpp"

#include "ast/import_node.hpp"
#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#include "sema/binder.hpp"

#include <functional>
#include <map>
#include <unordered_set>
#include <utility>

namespace {

struct PendingModule {
  std::string source;
  zap::DiagnosticEngine diagnostics;
  std::vector<Token> tokens;
  zap::frontend::ModuleOutline outline;
  std::vector<sema::ResolvedImport> imports;
  std::string logicalPath;

  PendingModule(std::string sourceText, const std::string &moduleId)
      : source(std::move(sourceText)), diagnostics(source, moduleId) {}
};

} // namespace

namespace zap::frontend {

FrontendSession::FrontendSession(FrontendSessionConfig config,
                                 SourceLoader sourceLoader)
    : config_(std::move(config)), sourceLoader_(std::move(sourceLoader)) {}

FrontendProject FrontendSession::load(const std::filesystem::path &entryPath) {
  FrontendProject project;
  const auto canonicalEntry = std::filesystem::weakly_canonical(entryPath);
  project.entryModuleId = canonicalEntry.string();
  std::map<std::string, std::unique_ptr<PendingModule>> pending;
  std::unordered_set<std::string> visiting;

  std::function<bool(const std::filesystem::path &)> discover =
      [&](const std::filesystem::path &modulePath) {
        const auto canonicalPath =
            std::filesystem::weakly_canonical(modulePath);
        const auto moduleId = canonicalPath.string();
        project.visitedModuleIds.insert(moduleId);
        if (visiting.count(moduleId) != 0) {
          project.errors.push_back("cyclic import detected involving " +
                                   moduleId);
          return false;
        }
        if (pending.count(moduleId) != 0)
          return true;

        auto source = sourceLoader_(canonicalPath);
        if (!source) {
          project.errors.push_back("couldn't open source file: " + moduleId);
          return false;
        }

        auto module =
            std::make_unique<PendingModule>(std::move(*source), moduleId);
        Lexer lexer(module->diagnostics);
        module->tokens = lexer.tokenize(module->source);
        module->outline =
            ModuleOutline::scan(module->tokens, module->diagnostics);
        module->logicalPath = computeLogicalModulePath(
            canonicalPath, config_.runtimePaths, config_.importMap);
        if (shouldIncludeImplicitPrelude(module->logicalPath,
                                         config_.includePrelude) &&
            !module->outline.hasImportPath("std/prelude")) {
          module->outline.imports.insert(
              module->outline.imports.begin(),
              std::make_unique<ImportNode>("std/prelude"));
        }

        visiting.insert(moduleId);
        auto &staged =
            *pending.emplace(moduleId, std::move(module)).first->second;
        bool complete =
            !staged.diagnostics.hadErrors() ||
            (config_.allowEntryErrors && moduleId == project.entryModuleId);
        for (const auto &import : staged.outline.imports) {
          std::vector<std::filesystem::path> targets;
          std::string error;
          if (!resolveImportTargets(canonicalPath, *import, targets,
                                    config_.importMap, config_.runtimePaths,
                                    &error)) {
            staged.diagnostics.report(import->span, DiagnosticLevel::Error,
                                      error);
            complete = false;
            continue;
          }
          staged.imports.push_back(makeResolvedImport(*import, targets));
        }

        for (const auto &import : staged.imports) {
          for (const auto &target : import.targetModuleIds) {
            if (!discover(target))
              complete = false;
          }
        }
        visiting.erase(moduleId);
        return complete;
      };

  const bool graphComplete = discover(canonicalEntry);
  bool parseComplete = true;
  for (auto &[moduleId, staged] : pending) {
    Parser parser(std::move(staged->tokens), staged->diagnostics);
    auto root = parser.parse();
    const bool isEntry = moduleId == project.entryModuleId;
    const bool accepted = root && (!staged->diagnostics.hadErrors() ||
                                   (config_.allowEntryErrors && isEntry));
    const auto &moduleDiagnostics = staged->diagnostics.diagnostics();
    project.diagnostics.insert(project.diagnostics.end(),
                               moduleDiagnostics.begin(),
                               moduleDiagnostics.end());
    project.outlines.emplace(moduleId, std::move(staged->outline));
    if (!accepted) {
      parseComplete = false;
      continue;
    }

    auto module = std::make_unique<sema::ModuleInfo>();
    module->moduleId = moduleId;
    module->moduleName = std::filesystem::path(moduleId).stem().string();
    module->linkPath = std::move(staged->logicalPath);
    module->sourceName = moduleId;
    module->sourceText = std::move(staged->source);
    module->isEntry = isEntry;
    module->root = std::move(root);
    module->imports = std::move(staged->imports);
    injectImplicitPreludeImportIfNeeded(*module, config_.includePrelude);
    project.modules.emplace(moduleId, std::move(module));
  }

  project.loaded = graphComplete && parseComplete &&
                   project.modules.count(project.entryModuleId) != 0;
  return project;
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
  project.diagnostics.insert(project.diagnostics.end(),
                             bindingDiagnostics.begin(),
                             bindingDiagnostics.end());
  return static_cast<bool>(project.boundRoot);
}

} // namespace zap::frontend
