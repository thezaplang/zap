#include "lsp/workspace.hpp"

#include "lexer/lexer.hpp"
#include "lsp/protocol_utils.hpp"
#include "parser/parser.hpp"
#include "sema/binder.hpp"
#include <utility>
#include <vector>

namespace zap::lsp {

zap::frontend::RuntimePaths Workspace::runtimePaths() const {
  return zap::frontend::RuntimePaths{
      std::filesystem::path(), std::filesystem::path(ZAPC_CORE_DIR),
      std::filesystem::path(ZAPC_STDLIB_DIR), std::filesystem::path()};
}

std::optional<std::string>
Workspace::sourceForPath(const std::filesystem::path &path) const {
  std::string key = std::filesystem::weakly_canonical(path).string();
  auto uriIt = uriByCanonicalPath_.find(key);
  if (uriIt != uriByCanonicalPath_.end()) {
    auto docIt = documentsByUri_.find(uriIt->second);
    if (docIt != documentsByUri_.end()) {
      return docIt->second.text;
    }
  }

  std::error_code ec;
  auto writeTime = std::filesystem::last_write_time(path, ec);
  if (!ec) {
    auto cacheIt = fileContentCache_.find(key);
    if (cacheIt != fileContentCache_.end() &&
        cacheIt->second.lastWriteTime == writeTime) {
      return cacheIt->second.content;
    }
  }

  std::string content;
  if (!readSourceFile(path, content)) {
    return std::nullopt;
  }
  if (!ec) {
    fileContentCache_[key] = CachedFile{writeTime, content};
  }
  return content;
}

bool Workspace::loadModuleGraph(
    const std::filesystem::path &modulePath,
    std::map<std::string, std::unique_ptr<sema::ModuleInfo>> &modules,
    std::set<std::string> &visiting, AnalysisResult &result,
    const std::string &entryUri, const zap::args::ImportMap &importMap,
    bool allowEntryErrors) const {
  std::filesystem::path canonicalPath =
      std::filesystem::weakly_canonical(modulePath);
  std::string moduleId = canonicalPath.string();
  std::string entryModuleId =
      std::filesystem::weakly_canonical(documentsByUri_.at(entryUri).path)
          .string();
  bool isEntryModule = moduleId == entryModuleId;
  if (modules.find(moduleId) != modules.end()) {
    return true;
  }
  if (visiting.count(moduleId)) {
    return false;
  }

  visiting.insert(moduleId);

  auto source = sourceForPath(canonicalPath);
  if (!source) {
    visiting.erase(moduleId);
    return false;
  }

  zap::DiagnosticEngine diagnostics(*source, canonicalPath.string());
  Lexer lex(diagnostics);
  auto tokens = lex.tokenize(*source);
  zap::Parser parser(tokens, diagnostics);
  auto ast = parser.parse();

  auto uriIt = uriByCanonicalPath_.find(moduleId);
  if (uriIt != uriByCanonicalPath_.end()) {
    result.diagnosticsByUri[uriIt->second] = diagnostics.diagnostics();
  } else if (moduleId == std::filesystem::weakly_canonical(
                             documentsByUri_.at(entryUri).path)
                             .string()) {
    result.diagnosticsByUri[entryUri] = diagnostics.diagnostics();
  }

  if (!ast ||
      (diagnostics.hadErrors() && !(allowEntryErrors && isEntryModule))) {
    visiting.erase(moduleId);
    return false;
  }

  auto module = std::make_unique<sema::ModuleInfo>();
  module->moduleId = moduleId;
  module->moduleName = canonicalPath.stem().string();
  module->linkPath =
      zap::frontend::computeLogicalModulePath(canonicalPath, runtimePaths());
  module->sourceName = canonicalPath.string();
  module->root = std::move(ast);

  zap::frontend::injectImplicitPreludeImportIfNeeded(*module, true);

  for (const auto &child : module->root->children) {
    auto importNode = dynamic_cast<ImportNode *>(child.get());
    if (!importNode) {
      continue;
    }

    std::vector<std::filesystem::path> importTargets;
    if (!zap::frontend::resolveImportTargets(canonicalPath, *importNode,
                                             importTargets, importMap,
                                             runtimePaths())) {
      continue;
    }

    module->imports.push_back(
        zap::frontend::makeResolvedImport(*importNode, importTargets));
  }

  for (const auto &import : module->imports) {
    for (const auto &targetId : import.targetModuleIds) {
      if (!loadModuleGraph(targetId, modules, visiting, result, entryUri,
                           importMap, allowEntryErrors)) {
        visiting.erase(moduleId);
        return false;
      }
    }
  }

  visiting.erase(moduleId);
  modules[moduleId] = std::move(module);
  return true;
}

const DocumentState *Workspace::document(const std::string &uri) const {
  auto it = documentsByUri_.find(uri);
  return it == documentsByUri_.end() ? nullptr : &it->second;
}

void Workspace::open(const std::string &uri, std::filesystem::path path,
                     std::string text, int64_t version) {
  std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(path);
  std::string canonicalKey = canonicalPath.string();

  auto existingUriIt = uriByCanonicalPath_.find(canonicalKey);
  if (existingUriIt != uriByCanonicalPath_.end() &&
      existingUriIt->second != uri) {
    documentsByUri_.erase(existingUriIt->second);
    existingUriIt->second = uri;
  } else {
    uriByCanonicalPath_[canonicalKey] = uri;
  }

  documentsByUri_[uri] =
      DocumentState{uri, std::move(canonicalPath), std::move(text), version};
}

void Workspace::update(const std::string &uri, std::string text,
                       int64_t version) {
  auto it = documentsByUri_.find(uri);
  if (it == documentsByUri_.end()) {
    return;
  }
  it->second.text = std::move(text);
  it->second.version = version;
}

void Workspace::close(const std::string &uri) {
  auto it = documentsByUri_.find(uri);
  if (it == documentsByUri_.end()) {
    return;
  }

  std::string canonicalKey = it->second.path.string();
  auto pathIt = uriByCanonicalPath_.find(canonicalKey);
  if (pathIt != uriByCanonicalPath_.end() && pathIt->second == uri) {
    uriByCanonicalPath_.erase(pathIt);
  }

  documentsByUri_.erase(it);
}

bool Workspace::contains(const std::string &uri) const {
  return documentsByUri_.find(uri) != documentsByUri_.end();
}

std::optional<ProjectState>
Workspace::loadProject(const std::string &uri, bool allowEntryErrors) const {
  auto docIt = documentsByUri_.find(uri);
  if (docIt == documentsByUri_.end()) {
    return std::nullopt;
  }

  auto flags = findAndReadFlags(docIt->second.path);

  ProjectState state;
  std::set<std::string> visiting;
  if (!loadModuleGraph(docIt->second.path, state.moduleMap, visiting,
                       state.analysis, uri, flags.importMap,
                       allowEntryErrors)) {
    if (state.analysis.diagnosticsByUri.find(uri) ==
        state.analysis.diagnosticsByUri.end()) {
      state.analysis.diagnosticsByUri[uri] = {};
    }
    return state;
  }

  for (const auto &[moduleId, _] : state.moduleMap) {
    auto uriIt = uriByCanonicalPath_.find(moduleId);
    if (uriIt != uriByCanonicalPath_.end()) {
      state.uriByModuleId[moduleId] = uriIt->second;
    } else {
      state.uriByModuleId[moduleId] = pathToUri(moduleId);
    }
  }

  auto entrySource = sourceForPath(docIt->second.path);
  if (entrySource) {
    auto entryId =
        std::filesystem::weakly_canonical(docIt->second.path).string();
    auto entryModuleIt = state.moduleMap.find(entryId);
    if (entryModuleIt != state.moduleMap.end()) {
      entryModuleIt->second->isEntry = true;
    }

    zap::DiagnosticEngine diagnostics(*entrySource,
                                      docIt->second.path.string());
    std::vector<sema::ModuleInfo *> modules;
    modules.reserve(state.moduleMap.size());
    for (auto &[_, modulePtr] : state.moduleMap) {
      modules.push_back(modulePtr.get());
    }

    sema::Binder binder(diagnostics, true, &state.semanticInfo);
    auto boundAst = binder.bind(std::move(modules));
    (void)boundAst;
  }
  return state;
}

AnalysisResult Workspace::analyze(const std::string &uri) const {
  AnalysisResult result;
  auto docIt = documentsByUri_.find(uri);
  if (docIt == documentsByUri_.end()) {
    return result;
  }

  auto flags = findAndReadFlags(docIt->second.path);

  std::map<std::string, std::unique_ptr<sema::ModuleInfo>> moduleMap;
  std::set<std::string> visiting;
  if (!loadModuleGraph(docIt->second.path, moduleMap, visiting, result, uri,
                       flags.importMap, false)) {
    if (result.diagnosticsByUri.find(uri) == result.diagnosticsByUri.end()) {
      result.diagnosticsByUri[uri] = {};
    }
    return result;
  }

  std::string entryId =
      std::filesystem::weakly_canonical(docIt->second.path).string();
  auto entrySource = sourceForPath(docIt->second.path);
  if (!entrySource) {
    result.diagnosticsByUri[uri] = {};
    return result;
  }

  auto entryModuleIt = moduleMap.find(entryId);
  if (entryModuleIt == moduleMap.end()) {
    return result;
  }

  entryModuleIt->second->isEntry = true;

  zap::DiagnosticEngine diagnostics(*entrySource, docIt->second.path.string());
  std::vector<sema::ModuleInfo> modules;
  modules.reserve(moduleMap.size());
  for (auto &[_, modulePtr] : moduleMap) {
    modules.push_back(std::move(*modulePtr));
  }

  sema::Binder binder(diagnostics);
  auto boundAst = binder.bind(modules);
  (void)boundAst;

  result.diagnosticsByUri[uri] = diagnostics.diagnostics();
  return result;
}

} // namespace zap::lsp
