#include "lexer/lexer.hpp"
#include "lsp.hpp"
#include "parser/parser.hpp"
#include "sema/binder.hpp"
#include "sema/module_info.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>

using namespace zap::lsp;

namespace {

using JsonObject = zap::lsp::JsonObject;

struct DocumentState {
  std::string uri;
  std::filesystem::path path;
  std::string text;
  int64_t version = 0;
};

const JsonObject *getField(const JsonObject &object, std::string_view key) {
  if (!object.isObject()) {
    return nullptr;
  }
  auto it = object.getAsObject().find(std::string(key));
  return it == object.getAsObject().end() ? nullptr : &it->second;
}

const JsonObject *getPath(const JsonObject &object,
                          std::initializer_list<std::string_view> path) {
  const JsonObject *current = &object;
  for (std::string_view key : path) {
    current = getField(*current, key);
    if (!current) {
      return nullptr;
    }
  }
  return current;
}

std::optional<std::string> getStringField(const JsonObject &object,
                                          std::initializer_list<std::string_view> path) {
  const JsonObject *value = getPath(object, path);
  if (!value || !value->isString()) {
    return std::nullopt;
  }
  return value->getAsString();
}

std::optional<int64_t> getIntegerField(const JsonObject &object,
                                       std::initializer_list<std::string_view> path) {
  const JsonObject *value = getPath(object, path);
  if (!value || !value->isInteger()) {
    return std::nullopt;
  }
  return value->getAsInteger();
}

std::string stripSourceExtension(const std::filesystem::path &path) {
  auto normalized = path.generic_string();
  if (path.extension() == ".zp" && normalized.size() >= 4) {
    normalized.resize(normalized.size() - 3);
  }
  return normalized;
}

std::filesystem::path stdlibRootPath() {
  if (const char *configured = std::getenv("ZAPC_STDLIB_DIR")) {
    if (*configured != '\0') {
      return std::filesystem::path(configured);
    }
  }
  return std::filesystem::path(ZAPC_STDLIB_DIR);
}

std::string computeLogicalModulePath(const std::filesystem::path &canonicalPath) {
  auto stdRoot = std::filesystem::weakly_canonical(stdlibRootPath());
  auto cwdRoot = std::filesystem::weakly_canonical(std::filesystem::current_path());

  auto buildRelative = [&](const std::filesystem::path &root,
                           const std::string &prefix = "")
      -> std::optional<std::string> {
    auto rootText = root.generic_string();
    auto pathText = canonicalPath.generic_string();
    if (pathText == rootText || pathText.rfind(rootText + "/", 0) != 0) {
      return std::nullopt;
    }

    auto rel = std::filesystem::relative(canonicalPath, root);
    auto stripped = stripSourceExtension(rel);
    if (prefix.empty()) {
      return stripped;
    }
    return prefix + "/" + stripped;
  };

  if (auto logical = buildRelative(stdRoot, "std")) {
    return *logical;
  }
  if (auto logical = buildRelative(cwdRoot)) {
    return *logical;
  }
  return stripSourceExtension(canonicalPath.filename());
}

std::optional<std::filesystem::path> uriToPath(std::string_view uri) {
  constexpr std::string_view prefix = "file://";
  if (uri.rfind(prefix, 0) != 0) {
    return std::nullopt;
  }

  std::string decoded;
  decoded.reserve(uri.size() - prefix.size());
  for (size_t i = prefix.size(); i < uri.size(); ++i) {
    char ch = uri[i];
    if (ch == '%' && i + 2 < uri.size()) {
      auto hex = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return 10 + value - 'a';
        if (value >= 'A' && value <= 'F') return 10 + value - 'A';
        return -1;
      };
      int hi = hex(uri[i + 1]);
      int lo = hex(uri[i + 2]);
      if (hi >= 0 && lo >= 0) {
        decoded.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    decoded.push_back(ch);
  }

#ifdef _WIN32
  if (decoded.size() >= 3 && decoded[0] == '/' &&
      std::isalpha(static_cast<unsigned char>(decoded[1])) &&
      decoded[2] == ':') {
    decoded.erase(decoded.begin());
  }
#endif

  return std::filesystem::path(decoded).lexically_normal();
}

bool readSourceFile(const std::filesystem::path &path, std::string &content) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    return false;
  }

  auto size = file.tellg();
  content.assign(static_cast<size_t>(std::max<std::streamsize>(size, 0)), '\0');
  if (size > 0) {
    file.seekg(0);
    file.read(content.data(), size);
  }
  return true;
}

bool resolveImportTargets(const std::filesystem::path &modulePath,
                          const ImportNode &importNode,
                          std::vector<std::filesystem::path> &targets) {
  std::filesystem::path resolvedPath;
  if (importNode.path.rfind("std/", 0) == 0) {
    resolvedPath =
        stdlibRootPath() / importNode.path.substr(std::string("std/").size());
  } else {
    resolvedPath = modulePath.parent_path() / importNode.path;
  }
  resolvedPath = resolvedPath.lexically_normal();

  auto tryFile = [&](const std::filesystem::path &path) -> bool {
    if (std::filesystem::exists(path) && std::filesystem::is_regular_file(path)) {
      targets.push_back(std::filesystem::weakly_canonical(path));
      return true;
    }
    return false;
  };

  if (tryFile(resolvedPath)) {
    return true;
  }
  if (resolvedPath.extension() != ".zp" && tryFile(resolvedPath.string() + ".zp")) {
    return true;
  }
  if (std::filesystem::exists(resolvedPath) &&
      std::filesystem::is_directory(resolvedPath)) {
    for (const auto &entry : std::filesystem::directory_iterator(resolvedPath)) {
      if (entry.is_regular_file() && entry.path().extension() == ".zp") {
        targets.push_back(std::filesystem::weakly_canonical(entry.path()));
      }
    }
    return !targets.empty();
  }
  return false;
}

struct AnalysisResult {
  std::unordered_map<std::string, std::vector<zap::Diagnostic>> diagnosticsByUri;
};

struct ProjectState {
  std::map<std::string, std::unique_ptr<sema::ModuleInfo>> moduleMap;
  std::unordered_map<std::string, std::string> uriByModuleId;
  AnalysisResult analysis;
};

struct LspSymbol {
  std::string name;
  std::string uri;
  SourceSpan span;
  int64_t completionKind = 6;
  Visibility visibility = Visibility::Private;
};

size_t offsetFromPosition(const std::string &text, int64_t line, int64_t character) {
  size_t offset = 0;
  int64_t currentLine = 0;
  while (offset < text.size() && currentLine < line) {
    if (text[offset++] == '\n') {
      ++currentLine;
    }
  }

  int64_t currentChar = 0;
  while (offset < text.size() && currentChar < character && text[offset] != '\n') {
    ++offset;
    ++currentChar;
  }
  return offset;
}

bool containsOffset(const SourceSpan &span, size_t offset) {
  return offset >= span.offset && offset <= span.offset + span.length;
}

bool isIdentifierChar(char ch) {
  return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
}

std::string pathToUri(const std::filesystem::path &path) {
  std::string uri = "file://";
  std::string text = path.generic_string();
  for (unsigned char ch : text) {
    bool unreserved = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                      (ch >= '0' && ch <= '9') || ch == '/' || ch == '-' ||
                      ch == '_' || ch == '.' || ch == '~';
    if (unreserved) {
      uri.push_back(static_cast<char>(ch));
    } else {
      static constexpr char hex[] = "0123456789ABCDEF";
      uri.push_back('%');
      uri.push_back(hex[(ch >> 4) & 0x0F]);
      uri.push_back(hex[ch & 0x0F]);
    }
  }
  return uri;
}

std::optional<std::string> identifierAt(const std::string &source, size_t offset) {
  if (source.empty()) {
    return std::nullopt;
  }
  size_t pos = offset < source.size() ? offset : source.size() - 1;
  if (!isIdentifierChar(source[pos])) {
    if (pos == 0 || !isIdentifierChar(source[pos - 1])) {
      return std::nullopt;
    }
    --pos;
  }

  size_t start = pos;
  while (start > 0 && isIdentifierChar(source[start - 1])) {
    --start;
  }
  size_t end = pos + 1;
  while (end < source.size() && isIdentifierChar(source[end])) {
    ++end;
  }
  return source.substr(start, end - start);
}

std::optional<std::pair<std::string, std::string>>
memberAccessBeforeCursor(const std::string &source, size_t offset) {
  if (offset == 0 || offset > source.size()) {
    return std::nullopt;
  }
  size_t pos = offset;
  while (pos > 0 && std::isspace(static_cast<unsigned char>(source[pos - 1]))) {
    --pos;
  }
  if (pos == 0 || source[pos - 1] != '.') {
    return std::nullopt;
  }

  size_t end = pos - 1;
  size_t start = end;
  while (start > 0 && isIdentifierChar(source[start - 1])) {
    --start;
  }
  if (start == end) {
    return std::nullopt;
  }
  std::string base = source.substr(start, end - start);

  size_t left = start;
  while (left > 1) {
    size_t dot = left;
    while (dot > 0 && std::isspace(static_cast<unsigned char>(source[dot - 1]))) {
      --dot;
    }
    if (dot == 0 || source[dot - 1] != '.') {
      break;
    }
    size_t nameEnd = dot - 1;
    size_t nameStart = nameEnd;
    while (nameStart > 0 && isIdentifierChar(source[nameStart - 1])) {
      --nameStart;
    }
    if (nameStart == nameEnd) {
      break;
    }
    base = source.substr(nameStart, end - nameStart);
    left = nameStart;
  }

  size_t split = base.find('.');
  if (split == std::string::npos) {
    return std::make_pair(base, std::string());
  }
  return std::make_pair(base.substr(0, split), base.substr(split + 1));
}

std::optional<std::pair<std::string, std::string>>
qualifiedIdentifierAtOffset(const std::string &source, size_t offset) {
  if (source.empty()) {
    return std::nullopt;
  }
  size_t pos = offset < source.size() ? offset : source.size() - 1;
  if (!isIdentifierChar(source[pos])) {
    if (pos == 0 || !isIdentifierChar(source[pos - 1])) {
      return std::nullopt;
    }
    --pos;
  }

  size_t start = pos;
  while (start > 0 && isIdentifierChar(source[start - 1])) {
    --start;
  }
  size_t end = pos + 1;
  while (end < source.size() && isIdentifierChar(source[end])) {
    ++end;
  }

  while (start > 1) {
    size_t dot = start;
    while (dot > 0 && std::isspace(static_cast<unsigned char>(source[dot - 1]))) {
      --dot;
    }
    if (dot == 0 || source[dot - 1] != '.') {
      break;
    }
    size_t nameEnd = dot - 1;
    size_t nameStart = nameEnd;
    while (nameStart > 0 && isIdentifierChar(source[nameStart - 1])) {
      --nameStart;
    }
    if (nameStart == nameEnd) {
      break;
    }
    start = nameStart;
  }

  while (end < source.size()) {
    size_t dot = end;
    while (dot < source.size() && std::isspace(static_cast<unsigned char>(source[dot]))) {
      ++dot;
    }
    if (dot >= source.size() || source[dot] != '.') {
      break;
    }
    size_t nameStart = dot + 1;
    while (nameStart < source.size() &&
           std::isspace(static_cast<unsigned char>(source[nameStart]))) {
      ++nameStart;
    }
    size_t nameEnd = nameStart;
    while (nameEnd < source.size() && isIdentifierChar(source[nameEnd])) {
      ++nameEnd;
    }
    if (nameEnd == nameStart) {
      break;
    }
    end = nameEnd;
  }

  std::string text = source.substr(start, end - start);
  size_t split = text.find('.');
  if (split == std::string::npos) {
    return std::make_pair(text, std::string());
  }
  return std::make_pair(text.substr(0, split), text.substr(split + 1));
}

std::optional<LspSymbol> makeSymbol(const std::string &uri, const std::string &name,
                                    const SourceSpan &span, int64_t kind,
                                    Visibility visibility = Visibility::Private) {
  return LspSymbol{name, uri, span, kind, visibility};
}

std::string effectiveImportAlias(const sema::ResolvedImport &import,
                                 const sema::ModuleInfo &target) {
  return import.moduleAlias.empty() ? target.moduleName : import.moduleAlias;
}

JsonObject makeRange(const zap::DiagnosticRange &range);

std::optional<LspSymbol> topLevelSymbolForNode(const Node *node, const std::string &uri) {
  if (auto fun = dynamic_cast<const FunDecl *>(node)) {
    return makeSymbol(uri, fun->name_, fun->span, 3, fun->visibility_);
  }
  if (auto ext = dynamic_cast<const ExtDecl *>(node)) {
    return makeSymbol(uri, ext->name_, ext->span, 3, ext->visibility_);
  }
  if (auto var = dynamic_cast<const VarDecl *>(node)) {
    return makeSymbol(uri, var->name_, var->span, 6, var->visibility_);
  }
  if (auto cnst = dynamic_cast<const ConstDecl *>(node)) {
    return makeSymbol(uri, cnst->name_, cnst->span, 21, cnst->visibility_);
  }
  if (auto record = dynamic_cast<const RecordDecl *>(node)) {
    return makeSymbol(uri, record->name_, record->span, 22, record->visibility_);
  }
  if (auto strukt = dynamic_cast<const StructDeclarationNode *>(node)) {
    return makeSymbol(uri, strukt->name_, strukt->span, 22, strukt->visibility_);
  }
  if (auto enm = dynamic_cast<const EnumDecl *>(node)) {
    return makeSymbol(uri, enm->name_, enm->span, 13, enm->visibility_);
  }
  if (auto alias = dynamic_cast<const TypeAliasDecl *>(node)) {
    return makeSymbol(uri, alias->name_, alias->span, 22, alias->visibility_);
  }
  return std::nullopt;
}

std::vector<LspSymbol> collectTopLevelSymbols(const sema::ModuleInfo &module,
                                              const std::string &uri,
                                              bool publicOnly = false) {
  std::vector<LspSymbol> symbols;
  for (const auto &child : module.root->children) {
    auto symbol = topLevelSymbolForNode(child.get(), uri);
    if (!symbol) {
      continue;
    }
    if (publicOnly && symbol->visibility != Visibility::Public) {
      continue;
    }
    symbols.push_back(*symbol);
  }
  return symbols;
}

void collectLocalsInBody(const BodyNode *body, size_t offset, const std::string &uri,
                         std::vector<LspSymbol> &symbols);

void collectLocalsFromNode(const Node *node, size_t offset, const std::string &uri,
                           std::vector<LspSymbol> &symbols) {
  if (!node) {
    return;
  }

  if (auto var = dynamic_cast<const VarDecl *>(node)) {
    if (!containsOffset(var->span, offset) && var->span.offset > offset) {
      return;
    }
    if (var->span.offset <= offset) {
      symbols.push_back(*makeSymbol(uri, var->name_, var->span, 6));
    }
    return;
  }
  if (auto cnst = dynamic_cast<const ConstDecl *>(node)) {
    if (!containsOffset(cnst->span, offset) && cnst->span.offset > offset) {
      return;
    }
    if (cnst->span.offset <= offset) {
      symbols.push_back(*makeSymbol(uri, cnst->name_, cnst->span, 21));
    }
    return;
  }
  if (auto body = dynamic_cast<const BodyNode *>(node)) {
    collectLocalsInBody(body, offset, uri, symbols);
    return;
  }
  if (auto ifNode = dynamic_cast<const IfNode *>(node)) {
    if (!containsOffset(ifNode->span, offset)) {
      return;
    }
    if (ifNode->thenBody_ &&
        (!ifNode->thenBody_->statements.empty() || ifNode->thenBody_->result) &&
        (ifNode->thenBody_->statements.empty() ||
         offset >= ifNode->thenBody_->statements.front()->span.offset)) {
      collectLocalsInBody(ifNode->thenBody_.get(), offset, uri, symbols);
    } else if (ifNode->elseBody_) {
      collectLocalsInBody(ifNode->elseBody_.get(), offset, uri, symbols);
    }
    return;
  }
  if (auto whileNode = dynamic_cast<const WhileNode *>(node)) {
    if (containsOffset(whileNode->span, offset) && whileNode->body_) {
      collectLocalsInBody(whileNode->body_.get(), offset, uri, symbols);
    }
    return;
  }
}

void collectLocalsInBody(const BodyNode *body, size_t offset, const std::string &uri,
                         std::vector<LspSymbol> &symbols) {
  for (const auto &statement : body->statements) {
    if (!statement || statement->span.offset > offset) {
      break;
    }
    if (auto var = dynamic_cast<const VarDecl *>(statement.get())) {
      symbols.push_back(*makeSymbol(uri, var->name_, var->span, 6));
    } else if (auto cnst = dynamic_cast<const ConstDecl *>(statement.get())) {
      symbols.push_back(*makeSymbol(uri, cnst->name_, cnst->span, 21));
    }
    collectLocalsFromNode(statement.get(), offset, uri, symbols);
  }
}

std::vector<LspSymbol> collectLocalSymbols(const RootNode &root, size_t offset,
                                           const std::string &uri) {
  std::vector<LspSymbol> symbols;
  for (const auto &child : root.children) {
    auto fun = dynamic_cast<const FunDecl *>(child.get());
    if (!fun || !fun->body_ || !containsOffset(fun->span, offset)) {
      continue;
    }
    for (const auto &param : fun->params_) {
      if (param) {
        symbols.push_back(*makeSymbol(uri, param->name, param->span, 6));
      }
    }
    collectLocalsInBody(fun->body_.get(), offset, uri, symbols);
    break;
  }
  return symbols;
}

JsonObject makeLocation(const std::string &uri, const SourceSpan &span) {
  zap::DiagnosticRange range{
      {span.line, span.column, span.offset},
      {span.line, span.column + std::max<size_t>(span.length, 1), span.offset + span.length}};
  JsonObject::Object object;
  object.emplace("uri", JsonObject(uri));
  object.emplace("range", makeRange(range));
  return JsonObject(std::move(object));
}

JsonObject makeCompletionItem(const LspSymbol &symbol, const std::string &detail = "") {
  JsonObject::Object item;
  item.emplace("label", JsonObject(symbol.name));
  item.emplace("kind", JsonObject(symbol.completionKind));
  if (!detail.empty()) {
    item.emplace("detail", JsonObject(detail));
  }
  return JsonObject(std::move(item));
}

class Workspace {
  std::unordered_map<std::string, DocumentState> documentsByUri_;
  std::unordered_map<std::string, std::string> uriByCanonicalPath_;

  std::optional<std::string> sourceForPath(const std::filesystem::path &path) const {
    std::string key = std::filesystem::weakly_canonical(path).string();
    auto uriIt = uriByCanonicalPath_.find(key);
    if (uriIt != uriByCanonicalPath_.end()) {
      auto docIt = documentsByUri_.find(uriIt->second);
      if (docIt != documentsByUri_.end()) {
        return docIt->second.text;
      }
    }

    std::string content;
    if (!readSourceFile(path, content)) {
      return std::nullopt;
    }
    return content;
  }

  bool loadModuleGraph(
      const std::filesystem::path &modulePath,
      std::map<std::string, std::unique_ptr<sema::ModuleInfo>> &modules,
      std::set<std::string> &visiting, AnalysisResult &result,
      const std::string &entryUri, bool allowEntryErrors = false) const {
    std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(modulePath);
    std::string moduleId = canonicalPath.string();
    std::string entryModuleId =
        std::filesystem::weakly_canonical(documentsByUri_.at(entryUri).path).string();
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
                               documentsByUri_.at(entryUri).path).string()) {
      result.diagnosticsByUri[entryUri] = diagnostics.diagnostics();
    }

    if (!ast || (diagnostics.hadErrors() && !(allowEntryErrors && isEntryModule))) {
      visiting.erase(moduleId);
      return false;
    }

    auto module = std::make_unique<sema::ModuleInfo>();
    module->moduleId = moduleId;
    module->moduleName = canonicalPath.stem().string();
    module->linkPath = computeLogicalModulePath(canonicalPath);
    module->sourceName = canonicalPath.string();
    module->root = std::move(ast);

    for (const auto &child : module->root->children) {
      auto importNode = dynamic_cast<ImportNode *>(child.get());
      if (!importNode) {
        continue;
      }

      sema::ResolvedImport resolved;
      resolved.rawPath = importNode->path;
      resolved.moduleAlias = importNode->moduleAlias;
      resolved.visibility = importNode->visibility_;
      resolved.span = importNode->span;
      for (const auto &binding : importNode->bindings) {
        resolved.bindings.push_back({binding.sourceName, binding.localName});
      }

      std::vector<std::filesystem::path> importTargets;
      if (!resolveImportTargets(canonicalPath, *importNode, importTargets)) {
        continue;
      }

      for (const auto &target : importTargets) {
        resolved.targetModuleIds.push_back(target.string());
      }

      module->imports.push_back(std::move(resolved));
    }

    for (const auto &import : module->imports) {
      for (const auto &targetId : import.targetModuleIds) {
        if (!loadModuleGraph(targetId, modules, visiting, result, entryUri,
                             allowEntryErrors)) {
          visiting.erase(moduleId);
          return false;
        }
      }
    }

    visiting.erase(moduleId);
    modules[moduleId] = std::move(module);
    return true;
  }

public:
  const DocumentState *document(const std::string &uri) const {
    auto it = documentsByUri_.find(uri);
    return it == documentsByUri_.end() ? nullptr : &it->second;
  }

  void open(const std::string &uri, std::filesystem::path path, std::string text,
            int64_t version) {
    std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(path);
    uriByCanonicalPath_[canonicalPath.string()] = uri;
    documentsByUri_[uri] = DocumentState{uri, std::move(canonicalPath), std::move(text),
                                         version};
  }

  void update(const std::string &uri, std::string text, int64_t version) {
    auto it = documentsByUri_.find(uri);
    if (it == documentsByUri_.end()) {
      return;
    }
    it->second.text = std::move(text);
    it->second.version = version;
  }

  void close(const std::string &uri) {
    auto it = documentsByUri_.find(uri);
    if (it == documentsByUri_.end()) {
      return;
    }
    uriByCanonicalPath_.erase(it->second.path.string());
    documentsByUri_.erase(it);
  }

  bool contains(const std::string &uri) const {
    return documentsByUri_.find(uri) != documentsByUri_.end();
  }

  std::optional<ProjectState> loadProject(const std::string &uri,
                                          bool allowEntryErrors = false) const {
    auto docIt = documentsByUri_.find(uri);
    if (docIt == documentsByUri_.end()) {
      return std::nullopt;
    }

    ProjectState state;
    std::set<std::string> visiting;
    if (!loadModuleGraph(docIt->second.path, state.moduleMap, visiting, state.analysis, uri,
                         allowEntryErrors)) {
      if (state.analysis.diagnosticsByUri.find(uri) == state.analysis.diagnosticsByUri.end()) {
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
    return state;
  }

  AnalysisResult analyze(const std::string &uri) const {
    AnalysisResult result;
    auto project = loadProject(uri);
    if (!project) {
      return result;
    }
    result = project->analysis;

    auto docIt = documentsByUri_.find(uri);
    if (docIt == documentsByUri_.end()) {
      return result;
    }
    std::string entryId = std::filesystem::weakly_canonical(docIt->second.path).string();
    auto entrySource = sourceForPath(docIt->second.path);
    if (!entrySource) {
      result.diagnosticsByUri[uri] = {};
      return result;
    }

    auto entryModuleIt = project->moduleMap.find(entryId);
    if (entryModuleIt == project->moduleMap.end()) {
      return result;
    }

    entryModuleIt->second->isEntry = true;

    zap::DiagnosticEngine diagnostics(*entrySource, docIt->second.path.string());
    std::vector<sema::ModuleInfo> modules;
    modules.reserve(project->moduleMap.size());
    for (auto &[_, module] : project->moduleMap) {
      modules.push_back(std::move(*module));
    }

    sema::Binder binder(diagnostics);
    auto boundAst = binder.bind(modules);
    (void)boundAst;

    result.diagnosticsByUri[uri] = diagnostics.diagnostics();
    return result;
  }
};

JsonObject makeResponse(const JsonObject *id, JsonObject result) {
  JsonObject::Object object;
  object.emplace("jsonrpc", JsonObject("2.0"));
  object.emplace("id", id ? *id : JsonObject(nullptr));
  object.emplace("result", std::move(result));
  return JsonObject(std::move(object));
}

JsonObject makeNotification(std::string method, JsonObject params) {
  JsonObject::Object object;
  object.emplace("jsonrpc", JsonObject("2.0"));
  object.emplace("method", JsonObject(std::move(method)));
  object.emplace("params", std::move(params));
  return JsonObject(std::move(object));
}

JsonObject makeRange(const zap::DiagnosticRange &range) {
  JsonObject::Object start;
  start.emplace("line", JsonObject(static_cast<int64_t>(range.start.line > 0 ? range.start.line - 1 : 0)));
  start.emplace("character",
                JsonObject(static_cast<int64_t>(range.start.column > 0 ? range.start.column - 1 : 0)));

  JsonObject::Object end;
  end.emplace("line", JsonObject(static_cast<int64_t>(range.end.line > 0 ? range.end.line - 1 : 0)));
  end.emplace("character",
              JsonObject(static_cast<int64_t>(range.end.column > 0 ? range.end.column - 1 : 0)));

  JsonObject::Object object;
  object.emplace("start", JsonObject(std::move(start)));
  object.emplace("end", JsonObject(std::move(end)));
  return JsonObject(std::move(object));
}

std::vector<LspSymbol> collectImportedSymbols(const ProjectState &project,
                                             const sema::ModuleInfo &module) {
  std::vector<LspSymbol> symbols;
  for (const auto &import : module.imports) {
    for (const auto &targetId : import.targetModuleIds) {
      auto moduleIt = project.moduleMap.find(targetId);
      if (moduleIt == project.moduleMap.end()) {
        continue;
      }
      const auto &target = *moduleIt->second;
      auto alias = effectiveImportAlias(import, target);
      auto targetUriIt = project.uriByModuleId.find(targetId);
      std::string targetUri =
          targetUriIt == project.uriByModuleId.end() ? pathToUri(targetId) : targetUriIt->second;

      symbols.push_back(*makeSymbol(project.uriByModuleId.at(module.moduleId), alias,
                                    import.span, 9, Visibility::Public));

      auto exported = collectTopLevelSymbols(target, targetUri, true);
      if (import.bindings.empty()) {
        continue;
      }
      for (const auto &binding : import.bindings) {
        auto exportIt =
            std::find_if(exported.begin(), exported.end(), [&](const LspSymbol &symbol) {
              return symbol.name == binding.sourceName;
            });
        if (exportIt == exported.end()) {
          continue;
        }
        LspSymbol local = *exportIt;
        local.name = binding.localName;
        symbols.push_back(std::move(local));
      }
    }
  }
  return symbols;
}

std::vector<LspSymbol> collectCompletionSymbols(const std::string &uri,
                                                const ProjectState &project,
                                                size_t offset) {
  std::vector<LspSymbol> symbols;
  auto path = uriToPath(uri);
  if (!path) {
    return symbols;
  }
  auto docIt = project.moduleMap.find(std::filesystem::weakly_canonical(*path).string());
  if (docIt == project.moduleMap.end()) {
    return symbols;
  }

  const sema::ModuleInfo &module = *docIt->second;
  auto topLevel = collectTopLevelSymbols(module, uri);
  symbols.insert(symbols.end(), topLevel.begin(), topLevel.end());

  auto imported = collectImportedSymbols(project, module);
  symbols.insert(symbols.end(), imported.begin(), imported.end());

  auto locals = collectLocalSymbols(*module.root, offset, uri);
  symbols.insert(symbols.end(), locals.begin(), locals.end());

  static constexpr std::pair<const char *, int64_t> builtinTypes[] = {
      {"Int", 22},   {"Int8", 22},   {"Int16", 22}, {"Int32", 22},
      {"Int64", 22}, {"UInt", 22},   {"UInt8", 22}, {"UInt16", 22},
      {"UInt32", 22},{"UInt64", 22}, {"Float", 22}, {"Float32", 22},
      {"Float64", 22},{"Bool", 22},  {"Void", 22},  {"Char", 22},
      {"String", 22}};
  for (const auto &[name, kind] : builtinTypes) {
    symbols.push_back(*makeSymbol(uri, name, SourceSpan(), kind, Visibility::Public));
  }

  return symbols;
}

std::optional<LspSymbol> resolveDefinition(const Workspace &workspace,
                                           const std::string &uri,
                                           const ProjectState &project,
                                           size_t offset) {
  const DocumentState *document = workspace.document(uri);
  if (!document) {
    return std::nullopt;
  }
  auto path = uriToPath(uri);
  if (!path) {
    return std::nullopt;
  }
  std::string moduleId = std::filesystem::weakly_canonical(*path).string();
  auto moduleIt = project.moduleMap.find(moduleId);
  if (moduleIt == project.moduleMap.end()) {
    return std::nullopt;
  }
  const sema::ModuleInfo &module = *moduleIt->second;

  if (auto qualified = qualifiedIdentifierAtOffset(document->text, offset)) {
    const auto &[base, rest] = *qualified;
    if (!rest.empty()) {
      std::string memberName = rest;
      size_t dot = memberName.rfind('.');
      if (dot != std::string::npos) {
        memberName = memberName.substr(dot + 1);
      }
      for (const auto &import : module.imports) {
        for (const auto &targetId : import.targetModuleIds) {
          auto targetIt = project.moduleMap.find(targetId);
          if (targetIt == project.moduleMap.end()) {
            continue;
          }
          if (effectiveImportAlias(import, *targetIt->second) != base) {
            continue;
          }
          auto targetUriIt = project.uriByModuleId.find(targetId);
          std::string targetUri = targetUriIt == project.uriByModuleId.end()
                                      ? pathToUri(targetId)
                                      : targetUriIt->second;
          auto exported = collectTopLevelSymbols(*targetIt->second, targetUri, true);
          auto exportIt =
              std::find_if(exported.begin(), exported.end(), [&](const LspSymbol &symbol) {
                return symbol.name == memberName;
              });
          if (exportIt != exported.end()) {
            return *exportIt;
          }
        }
      }
    }
  }

  auto name = identifierAt(document->text, offset);
  if (!name) {
    return std::nullopt;
  }

  auto locals = collectLocalSymbols(*module.root, offset, uri);
  for (auto it = locals.rbegin(); it != locals.rend(); ++it) {
    if (it->name == *name) {
      return *it;
    }
  }

  auto imported = collectImportedSymbols(project, module);
  for (const auto &symbol : imported) {
    if (symbol.name == *name) {
      return symbol;
    }
  }

  auto topLevel = collectTopLevelSymbols(module, uri);
  for (const auto &symbol : topLevel) {
    if (symbol.name == *name) {
      return symbol;
    }
  }

  return std::nullopt;
}

int64_t toLspSeverity(zap::DiagnosticLevel level) {
  switch (level) {
  case zap::DiagnosticLevel::Error:
    return 1;
  case zap::DiagnosticLevel::Warning:
    return 2;
  case zap::DiagnosticLevel::Note:
    return 3;
  }
  return 1;
}

JsonObject makeDiagnostic(const zap::Diagnostic &diagnostic) {
  JsonObject::Object object;
  object.emplace("range", makeRange(diagnostic.range));
  object.emplace("severity", JsonObject(toLspSeverity(diagnostic.level)));
  object.emplace("source", JsonObject("zap-lsp"));
  object.emplace("message", JsonObject(diagnostic.message));
  return JsonObject(std::move(object));
}

JsonObject makePublishDiagnostics(std::string uri,
                                  const std::vector<zap::Diagnostic> &diagnostics) {
  JsonObject::List items;
  items.reserve(diagnostics.size());
  for (const auto &diagnostic : diagnostics) {
    items.push_back(makeDiagnostic(diagnostic));
  }

  JsonObject::Object params;
  params.emplace("uri", JsonObject(std::move(uri)));
  params.emplace("diagnostics", JsonObject(std::move(items)));
  return makeNotification("textDocument/publishDiagnostics", JsonObject(std::move(params)));
}

void publishAnalysis(Server &server, const AnalysisResult &result) {
  for (const auto &[uri, diagnostics] : result.diagnosticsByUri) {
    server.sendMessage(makePublishDiagnostics(uri, diagnostics));
  }
}

} // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  Server server;
  Workspace workspace;
  bool running = true;
  bool shutdownRequested = false;
  std::string line;

  while (running) {
    std::string message = server.processMessage(line);
    if (message.empty()) {
      break;
    }

    JsonRPC rpc(message);
    const JsonObject &request = rpc.object();
    auto method = getStringField(request, {"method"});
    const JsonObject *id = getField(request, "id");

    if (!method) {
      continue;
    }

    if (*method == "initialize") {
      shutdownRequested = false;

      JsonObject::Object syncOptions;
      syncOptions.emplace("openClose", JsonObject(true));
      syncOptions.emplace("change", JsonObject(int64_t(1)));

      JsonObject::Object capabilities;
      capabilities.emplace("textDocumentSync", JsonObject(std::move(syncOptions)));
      capabilities.emplace("definitionProvider", JsonObject(true));

      JsonObject::Object completionOptions;
      completionOptions.emplace("resolveProvider", JsonObject(false));
      completionOptions.emplace("triggerCharacters",
                                JsonObject(JsonObject::List{
                                    JsonObject("."), JsonObject("_"),
                                    JsonObject("a"), JsonObject("b"), JsonObject("c"),
                                    JsonObject("d"), JsonObject("e"), JsonObject("f"),
                                    JsonObject("g"), JsonObject("h"), JsonObject("i"),
                                    JsonObject("j"), JsonObject("k"), JsonObject("l"),
                                    JsonObject("m"), JsonObject("n"), JsonObject("o"),
                                    JsonObject("p"), JsonObject("q"), JsonObject("r"),
                                    JsonObject("s"), JsonObject("t"), JsonObject("u"),
                                    JsonObject("v"), JsonObject("w"), JsonObject("x"),
                                    JsonObject("y"), JsonObject("z"), JsonObject("A"),
                                    JsonObject("B"), JsonObject("C"), JsonObject("D"),
                                    JsonObject("E"), JsonObject("F"), JsonObject("G"),
                                    JsonObject("H"), JsonObject("I"), JsonObject("J"),
                                    JsonObject("K"), JsonObject("L"), JsonObject("M"),
                                    JsonObject("N"), JsonObject("O"), JsonObject("P"),
                                    JsonObject("Q"), JsonObject("R"), JsonObject("S"),
                                    JsonObject("T"), JsonObject("U"), JsonObject("V"),
                                    JsonObject("W"), JsonObject("X"), JsonObject("Y"),
                                    JsonObject("Z")}));
      capabilities.emplace("completionProvider", JsonObject(std::move(completionOptions)));

      JsonObject::Object serverInfo;
      serverInfo.emplace("name", JsonObject("zap-lsp"));

      JsonObject::Object result;
      result.emplace("capabilities", JsonObject(std::move(capabilities)));
      result.emplace("serverInfo", JsonObject(std::move(serverInfo)));

      server.sendMessage(makeResponse(id, JsonObject(std::move(result))));
    } else if (*method == "initialized") {
      continue;
    } else if (*method == "shutdown") {
      shutdownRequested = true;
      server.sendMessage(makeResponse(id, JsonObject(nullptr)));
    } else if (*method == "exit") {
      running = false;
    } else if (*method == "textDocument/didOpen") {
      auto uri = getStringField(request, {"params", "textDocument", "uri"});
      auto text = getStringField(request, {"params", "textDocument", "text"});
      auto version = getIntegerField(request, {"params", "textDocument", "version"}).value_or(0);

      if (uri && text) {
        auto path = uriToPath(*uri);
        if (path) {
          workspace.open(*uri, *path, *text, version);
          publishAnalysis(server, workspace.analyze(*uri));
        }
      }
    } else if (*method == "textDocument/didChange") {
      auto uri = getStringField(request, {"params", "textDocument", "uri"});
      auto version = getIntegerField(request, {"params", "textDocument", "version"}).value_or(0);
      const JsonObject *changes = getPath(request, {"params", "contentChanges"});

      if (uri && changes && changes->isList() && !changes->getAsList().empty()) {
        const JsonObject &lastChange = changes->getAsList().back();
        auto text = getStringField(lastChange, {"text"});
        if (text && workspace.contains(*uri)) {
          workspace.update(*uri, *text, version);
          publishAnalysis(server, workspace.analyze(*uri));
        }
      }
    } else if (*method == "textDocument/didClose") {
      auto uri = getStringField(request, {"params", "textDocument", "uri"});
      if (uri) {
        workspace.close(*uri);
        server.sendMessage(makePublishDiagnostics(*uri, {}));
      }
    } else if (*method == "textDocument/completion") {
      auto uri = getStringField(request, {"params", "textDocument", "uri"});
      auto line = getIntegerField(request, {"params", "position", "line"});
      auto character = getIntegerField(request, {"params", "position", "character"});
      if (id && uri && line && character) {
        JsonObject::List items;
        if (const auto *document = workspace.document(*uri)) {
          auto project = workspace.loadProject(*uri, true);
          if (project) {
            size_t offset = offsetFromPosition(document->text, *line, *character);
            if (auto member = memberAccessBeforeCursor(document->text, offset)) {
              const auto &[base, _] = *member;
              std::string moduleId;
              auto path = uriToPath(*uri);
              if (path) {
                moduleId = std::filesystem::weakly_canonical(*path).string();
              }
              auto moduleIt = project->moduleMap.find(moduleId);
              if (moduleIt != project->moduleMap.end()) {
                for (const auto &import : moduleIt->second->imports) {
                  for (const auto &targetId : import.targetModuleIds) {
                    auto targetIt = project->moduleMap.find(targetId);
                    if (targetIt == project->moduleMap.end()) {
                      continue;
                    }
                    if (effectiveImportAlias(import, *targetIt->second) != base) {
                      continue;
                    }
                    auto targetUriIt = project->uriByModuleId.find(targetId);
                    std::string targetUri = targetUriIt == project->uriByModuleId.end()
                                                ? pathToUri(targetId)
                                                : targetUriIt->second;
                    for (const auto &symbol :
                         collectTopLevelSymbols(*targetIt->second, targetUri, true)) {
                      items.push_back(makeCompletionItem(symbol, "imported member"));
                    }
                  }
                }
              }
            } else {
              std::vector<LspSymbol> symbols = collectCompletionSymbols(*uri, *project, offset);
              std::set<std::string> seen;
              static constexpr const char *keywords[] = {
                  "fun",   "return", "if",    "else",  "while", "var",
                  "const", "import", "pub",   "priv",  "struct","record",
                  "enum",  "alias",  "extern","global","break", "continue",
                  "ref",   "as"};
              for (const char *keyword : keywords) {
                if (seen.insert(keyword).second) {
                  items.push_back(
                      makeCompletionItem(*makeSymbol(*uri, keyword, SourceSpan(), 14),
                                         "keyword"));
                }
              }
              for (const auto &symbol : symbols) {
                if (seen.insert(symbol.name).second) {
                  items.push_back(makeCompletionItem(symbol));
                }
              }
            }
          }
        }
        server.sendMessage(makeResponse(id, JsonObject(std::move(items))));
      }
    } else if (*method == "textDocument/definition") {
      auto uri = getStringField(request, {"params", "textDocument", "uri"});
      auto line = getIntegerField(request, {"params", "position", "line"});
      auto character = getIntegerField(request, {"params", "position", "character"});
      if (id && uri && line && character) {
        JsonObject result(nullptr);
        if (const auto *document = workspace.document(*uri)) {
          auto project = workspace.loadProject(*uri, true);
          if (project) {
            size_t offset = offsetFromPosition(document->text, *line, *character);
            auto symbol = resolveDefinition(workspace, *uri, *project, offset);
            if (symbol) {
              result = makeLocation(symbol->uri, symbol->span);
            }
          }
        }
        server.sendMessage(makeResponse(id, std::move(result)));
      }
    } else {
      if (id) {
        JsonObject::Object error;
        error.emplace("code", JsonObject(int64_t(JsonRPC::MethodNotFound)));
        error.emplace("message", JsonObject("Method not found"));

        JsonObject::Object response;
        response.emplace("jsonrpc", JsonObject("2.0"));
        response.emplace("id", *id);
        response.emplace("error", JsonObject(std::move(error)));
        server.sendMessage(JsonObject(std::move(response)));
      }
    }

    server.send();
  }

  return shutdownRequested ? 0 : 0;
}
