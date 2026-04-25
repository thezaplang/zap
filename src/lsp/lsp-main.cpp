#include "lexer/lexer.hpp"
#include "lsp.hpp"
#include "parser/parser.hpp"
#include "sema/binder.hpp"
#include "sema/module_info.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unistd.h>

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

std::optional<std::string>
getStringField(const JsonObject &object,
               std::initializer_list<std::string_view> path) {
  const JsonObject *value = getPath(object, path);
  if (!value || !value->isString()) {
    return std::nullopt;
  }
  return value->getAsString();
}

std::optional<int64_t>
getIntegerField(const JsonObject &object,
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

  std::error_code ec;
  auto exePath = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (!ec && !exePath.empty()) {
    auto siblingStd =
        std::filesystem::weakly_canonical(exePath).parent_path() / "std";
    if (std::filesystem::exists(siblingStd) &&
        std::filesystem::is_directory(siblingStd)) {
      return siblingStd;
    }
  }

  return std::filesystem::path(ZAPC_STDLIB_DIR);
}

std::string
computeLogicalModulePath(const std::filesystem::path &canonicalPath) {
  auto stdRoot = std::filesystem::weakly_canonical(stdlibRootPath());
  auto cwdRoot =
      std::filesystem::weakly_canonical(std::filesystem::current_path());

  auto buildRelative = [&](const std::filesystem::path &root,
                           const std::string &prefix =
                               "") -> std::optional<std::string> {
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
        if (value >= '0' && value <= '9')
          return value - '0';
        if (value >= 'a' && value <= 'f')
          return 10 + value - 'a';
        if (value >= 'A' && value <= 'F')
          return 10 + value - 'A';
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

struct LspFlags {
  bool allowUnsafe = false;
};

LspFlags readFlagsFromFile(const std::filesystem::path &path) {
  LspFlags flags;
  std::ifstream file(path);
  if (!file) {
    return flags;
  }

  std::string line;
  while (std::getline(file, line)) {
    if (line == "--allow-unsafe") {
      flags.allowUnsafe = true;
    }
  }
  return flags;
}

LspFlags findAndReadFlags(std::filesystem::path startPath) {
  if (std::filesystem::is_regular_file(startPath)) {
    startPath = startPath.parent_path();
  }

  while (true) {
    auto flagsPath = startPath / "zap_flags.txt";
    if (std::filesystem::exists(flagsPath)) {
      return readFlagsFromFile(flagsPath);
    }
    if (startPath == startPath.parent_path()) {
      break;
    }
    startPath = startPath.parent_path();
  }
  return {};
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
    if (std::filesystem::exists(path) &&
        std::filesystem::is_regular_file(path)) {
      targets.push_back(std::filesystem::weakly_canonical(path));
      return true;
    }
    return false;
  };

  if (tryFile(resolvedPath)) {
    return true;
  }
  if (resolvedPath.extension() != ".zp" &&
      tryFile(resolvedPath.string() + ".zp")) {
    return true;
  }
  if (std::filesystem::exists(resolvedPath) &&
      std::filesystem::is_directory(resolvedPath)) {
    for (const auto &entry :
         std::filesystem::directory_iterator(resolvedPath)) {
      if (entry.is_regular_file() && entry.path().extension() == ".zp") {
        targets.push_back(std::filesystem::weakly_canonical(entry.path()));
      }
    }
    return !targets.empty();
  }
  return false;
}

struct AnalysisResult {
  std::unordered_map<std::string, std::vector<zap::Diagnostic>>
      diagnosticsByUri;
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

struct LspSignature {
  std::string label;
  std::vector<std::string> parameters;
};

class Workspace;

std::string effectiveImportAlias(const sema::ResolvedImport &import,
                                 const sema::ModuleInfo &target);

size_t offsetFromPosition(const std::string &text, int64_t line,
                          int64_t character) {
  size_t offset = 0;
  int64_t currentLine = 0;
  while (offset < text.size() && currentLine < line) {
    if (text[offset++] == '\n') {
      ++currentLine;
    }
  }

  int64_t currentChar = 0;
  while (offset < text.size() && currentChar < character &&
         text[offset] != '\n') {
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

std::optional<std::string> identifierAt(const std::string &source,
                                        size_t offset) {
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
    while (dot > 0 &&
           std::isspace(static_cast<unsigned char>(source[dot - 1]))) {
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
    while (dot > 0 &&
           std::isspace(static_cast<unsigned char>(source[dot - 1]))) {
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
    while (dot < source.size() &&
           std::isspace(static_cast<unsigned char>(source[dot]))) {
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

std::optional<LspSymbol>
makeSymbol(const std::string &uri, const std::string &name,
           const SourceSpan &span, int64_t kind,
           Visibility visibility = Visibility::Private) {
  return LspSymbol{name, uri, span, kind, visibility};
}

std::string renderType(const TypeNode *type) {
  if (!type) {
    return "Void";
  }
  if (type->isVarArgs && type->baseType) {
    return "..." + renderType(type->baseType.get());
  }
  if (type->isPointer && type->baseType) {
    return "*" + renderType(type->baseType.get());
  }
  if (type->isArray && type->baseType) {
    std::string size = type->arraySize ? "?" : "?";
    return "[" + size + "]" + renderType(type->baseType.get());
  }

  std::string qualified = type->qualifiedName();
  if (!type->genericArgs.empty()) {
    qualified += "<";
    for (size_t i = 0; i < type->genericArgs.size(); ++i) {
      if (i != 0) {
        qualified += ", ";
      }
      qualified += renderType(type->genericArgs[i].get());
    }
    qualified += ">";
  }

  if (type->isWeak) {
    return "weak " + qualified;
  }
  return qualified;
}

std::string renderParameter(const ParameterNode *param) {
  if (!param) {
    return "";
  }
  std::string prefix = param->isRef ? "ref " : "";
  if (param->isVariadic) {
    return prefix + param->name + ": ..." + renderType(param->type.get());
  }
  return prefix + param->name + ": " + renderType(param->type.get());
}

std::optional<LspSignature> signatureForNode(const Node *node) {
  auto renderGenericParams = [](const std::vector<std::unique_ptr<TypeNode>> &genericParams) {
    if (genericParams.empty()) {
      return std::string();
    }
    std::string out = "<";
    for (size_t i = 0; i < genericParams.size(); ++i) {
      if (i != 0) {
        out += ", ";
      }
      out += genericParams[i] ? genericParams[i]->qualifiedName() : "?";
    }
    out += ">";
    return out;
  };

  auto renderConstraints =
      [](const std::vector<GenericConstraint> &constraints) {
        if (constraints.empty()) {
          return std::string();
        }
        std::string out = " where ";
        for (size_t i = 0; i < constraints.size(); ++i) {
          if (i != 0) {
            out += ", ";
          }
          out += constraints[i].parameterName + ": ";
          out += constraints[i].boundType
                     ? renderType(constraints[i].boundType.get())
                     : std::string("?");
        }
        return out;
      };

  if (auto fun = dynamic_cast<const FunDecl *>(node)) {
    std::vector<std::string> params;
    for (const auto &param : fun->params_) {
      params.push_back(renderParameter(param.get()));
    }
    std::string label = fun->name_ + renderGenericParams(fun->genericParams_) + "(";
    for (size_t i = 0; i < params.size(); ++i) {
      if (i != 0) {
        label += ", ";
      }
      label += params[i];
    }
    label += ") " + renderType(fun->returnType_.get());
    label += renderConstraints(fun->genericConstraints_);
    return LspSignature{std::move(label), std::move(params)};
  }
  if (auto ext = dynamic_cast<const ExtDecl *>(node)) {
    std::vector<std::string> params;
    for (const auto &param : ext->params_) {
      params.push_back(renderParameter(param.get()));
    }
    if (ext->isCVariadic_) {
      params.push_back("...");
    }
    std::string label = ext->name_ + "(";
    for (size_t i = 0; i < params.size(); ++i) {
      if (i != 0) {
        label += ", ";
      }
      label += params[i];
    }
    label += ") " + renderType(ext->returnType_.get());
    return LspSignature{std::move(label), std::move(params)};
  }
  return std::nullopt;
}

std::vector<LspSignature> findTopLevelSignatures(const sema::ModuleInfo &module,
                                                 std::string_view name,
                                                 bool publicOnly = false) {
  std::vector<LspSignature> signatures;
  for (const auto &child : module.root->children) {
    const TopLevel *topLevel = dynamic_cast<const TopLevel *>(child.get());
    std::optional<std::string> childName;
    if (auto fun = dynamic_cast<const FunDecl *>(child.get())) {
      childName = fun->name_;
    } else if (auto ext = dynamic_cast<const ExtDecl *>(child.get())) {
      childName = ext->name_;
    }
    if (!childName || *childName != name) {
      continue;
    }
    if (publicOnly &&
        (!topLevel || topLevel->visibility_ != Visibility::Public)) {
      continue;
    }
    if (auto signature = signatureForNode(child.get())) {
      signatures.push_back(*signature);
    }
  }
  return signatures;
}

struct CallContext {
  std::string callee;
  int64_t activeParameter = 0;
};

struct HoverInfo {
  std::string language = "zap";
  std::string value;
};

std::optional<std::string> topLevelNameForNode(const Node *node) {
  if (auto fun = dynamic_cast<const FunDecl *>(node)) {
    return fun->name_;
  }
  if (auto ext = dynamic_cast<const ExtDecl *>(node)) {
    return ext->name_;
  }
  if (auto var = dynamic_cast<const VarDecl *>(node)) {
    return var->name_;
  }
  if (auto cnst = dynamic_cast<const ConstDecl *>(node)) {
    return cnst->name_;
  }
  if (auto record = dynamic_cast<const RecordDecl *>(node)) {
    return record->name_;
  }
  if (auto strukt = dynamic_cast<const StructDeclarationNode *>(node)) {
    return strukt->name_;
  }
  if (auto cls = dynamic_cast<const ClassDecl *>(node)) {
    return cls->name_;
  }
  if (auto enm = dynamic_cast<const EnumDecl *>(node)) {
    return enm->name_;
  }
  if (auto alias = dynamic_cast<const TypeAliasDecl *>(node)) {
    return alias->name_;
  }
  return std::nullopt;
}

const ClassDecl *findClassDecl(const sema::ModuleInfo &module,
                               std::string_view name, bool publicOnly = false) {
  for (const auto &child : module.root->children) {
    auto cls = dynamic_cast<const ClassDecl *>(child.get());
    auto topLevel = dynamic_cast<const TopLevel *>(child.get());
    if (!cls || cls->name_ != name) {
      continue;
    }
    if (publicOnly &&
        (!topLevel || topLevel->visibility_ != Visibility::Public)) {
      continue;
    }
    return cls;
  }
  return nullptr;
}

const sema::ModuleInfo *
findImportedModuleByAlias(const ProjectState &project,
                          const sema::ModuleInfo &module,
                          std::string_view alias) {
  for (const auto &import : module.imports) {
    for (const auto &targetId : import.targetModuleIds) {
      auto targetIt = project.moduleMap.find(targetId);
      if (targetIt == project.moduleMap.end()) {
        continue;
      }
      if (effectiveImportAlias(import, *targetIt->second) == alias) {
        return targetIt->second.get();
      }
    }
  }
  return nullptr;
}

std::pair<std::string, std::string>
splitQualifiedName(std::string_view qualifiedName) {
  size_t dot = qualifiedName.rfind('.');
  if (dot == std::string_view::npos) {
    return {std::string(), std::string(qualifiedName)};
  }
  return {std::string(qualifiedName.substr(0, dot)),
          std::string(qualifiedName.substr(dot + 1))};
}

const ClassDecl *resolveClassByTypeName(const ProjectState &project,
                                        const sema::ModuleInfo &module,
                                        std::string_view typeName,
                                        bool publicOnly = false) {
  auto [qualifier, localName] = splitQualifiedName(typeName);
  if (localName.empty()) {
    return nullptr;
  }
  if (qualifier.empty()) {
    return findClassDecl(module, localName, publicOnly);
  }
  auto importedModule = findImportedModuleByAlias(project, module, qualifier);
  if (!importedModule) {
    return nullptr;
  }
  return findClassDecl(*importedModule, localName, publicOnly);
}

bool isClassTypeName(const ProjectState &project,
                     const sema::ModuleInfo &module,
                     std::string_view typeName) {
  return resolveClassByTypeName(project, module, typeName) != nullptr;
}

std::optional<std::string>
resolveClassTypeNameFromTypeNode(const ProjectState &project,
                                 const sema::ModuleInfo &module,
                                 const TypeNode *type) {
  if (!type) {
    return std::nullopt;
  }
  std::string qualified = renderType(type);
  if (!isClassTypeName(project, module, qualified)) {
    return std::nullopt;
  }
  return qualified;
}

const ClassDecl *enclosingClassAtOffset(const RootNode &root, size_t offset) {
  for (const auto &child : root.children) {
    auto cls = dynamic_cast<const ClassDecl *>(child.get());
    if (!cls || !containsOffset(cls->span, offset)) {
      continue;
    }
    return cls;
  }
  return nullptr;
}

std::optional<std::string> lookupTypeInBody(const ProjectState &project,
                                            const sema::ModuleInfo &module,
                                            const BodyNode *body, size_t offset,
                                            std::string_view name);

std::optional<std::string> lookupTypeInNode(const ProjectState &project,
                                            const sema::ModuleInfo &module,
                                            const Node *node, size_t offset,
                                            std::string_view name) {
  if (!node) {
    return std::nullopt;
  }
  if (auto var = dynamic_cast<const VarDecl *>(node)) {
    if (var->name_ == name && var->span.offset <= offset) {
      return resolveClassTypeNameFromTypeNode(project, module,
                                              var->type_.get());
    }
    return std::nullopt;
  }
  if (auto cnst = dynamic_cast<const ConstDecl *>(node)) {
    if (cnst->name_ == name && cnst->span.offset <= offset) {
      return resolveClassTypeNameFromTypeNode(project, module,
                                              cnst->type_.get());
    }
    return std::nullopt;
  }
  if (auto body = dynamic_cast<const BodyNode *>(node)) {
    return lookupTypeInBody(project, module, body, offset, name);
  }
  if (auto ifNode = dynamic_cast<const IfNode *>(node)) {
    if (!containsOffset(ifNode->span, offset)) {
      return std::nullopt;
    }
    if (ifNode->thenBody_) {
      if (auto found = lookupTypeInBody(
              project, module, ifNode->thenBody_.get(), offset, name)) {
        return found;
      }
    }
    if (ifNode->elseBody_) {
      if (auto found = lookupTypeInBody(
              project, module, ifNode->elseBody_.get(), offset, name)) {
        return found;
      }
    }
    return std::nullopt;
  }
  if (auto whileNode = dynamic_cast<const WhileNode *>(node)) {
    if (containsOffset(whileNode->span, offset) && whileNode->body_) {
      return lookupTypeInBody(project, module, whileNode->body_.get(), offset,
                              name);
    }
  }
  return std::nullopt;
}

std::optional<std::string> lookupTypeInBody(const ProjectState &project,
                                            const sema::ModuleInfo &module,
                                            const BodyNode *body, size_t offset,
                                            std::string_view name) {
  if (!body) {
    return std::nullopt;
  }
  for (const auto &statement : body->statements) {
    if (!statement || statement->span.offset > offset) {
      break;
    }
    if (auto found =
            lookupTypeInNode(project, module, statement.get(), offset, name)) {
      return found;
    }
  }
  if (body->result && body->result->span.offset <= offset) {
    if (auto found = lookupTypeInNode(project, module, body->result.get(),
                                      offset, name)) {
      return found;
    }
  }
  return std::nullopt;
}

std::optional<std::string>
resolveVariableClassType(const ProjectState &project,
                         const sema::ModuleInfo &module, size_t offset,
                         std::string_view name) {
  if (name == "self") {
    if (auto cls = enclosingClassAtOffset(*module.root, offset)) {
      return cls->name_;
    }
  }

  for (const auto &child : module.root->children) {
    if (auto var = dynamic_cast<const VarDecl *>(child.get())) {
      if (var->name_ == name) {
        return resolveClassTypeNameFromTypeNode(project, module,
                                                var->type_.get());
      }
    }
    if (auto cnst = dynamic_cast<const ConstDecl *>(child.get())) {
      if (cnst->name_ == name) {
        return resolveClassTypeNameFromTypeNode(project, module,
                                                cnst->type_.get());
      }
    }
    if (auto fun = dynamic_cast<const FunDecl *>(child.get())) {
      if (!containsOffset(fun->span, offset)) {
        continue;
      }
      for (const auto &param : fun->params_) {
        if (param && param->name == name) {
          return resolveClassTypeNameFromTypeNode(project, module,
                                                  param->type.get());
        }
      }
      return lookupTypeInBody(project, module, fun->body_.get(), offset, name);
    }
    if (auto cls = dynamic_cast<const ClassDecl *>(child.get())) {
      if (!containsOffset(cls->span, offset)) {
        continue;
      }
      for (const auto &field : cls->fields_) {
        if (field && field->name == name) {
          return resolveClassTypeNameFromTypeNode(project, module,
                                                  field->type.get());
        }
      }
      for (const auto &method : cls->methods_) {
        if (!method || !containsOffset(method->span, offset)) {
          continue;
        }
        for (const auto &param : method->params_) {
          if (param && param->name == name) {
            return resolveClassTypeNameFromTypeNode(project, module,
                                                    param->type.get());
          }
        }
        return lookupTypeInBody(project, module, method->body_.get(), offset,
                                name);
      }
    }
  }

  return std::nullopt;
}

const ParameterNode *findClassField(const ClassDecl *cls,
                                    const ProjectState &project,
                                    const sema::ModuleInfo &module,
                                    std::string_view member,
                                    bool publicOnly = false) {
  if (!cls) {
    return nullptr;
  }
  for (const auto &field : cls->fields_) {
    if (!field || field->name != member) {
      continue;
    }
    if (publicOnly && field->visibility_ != Visibility::Public) {
      continue;
    }
    return field.get();
  }
  if (cls->baseType_) {
    if (auto base = resolveClassByTypeName(
            project, module, cls->baseType_->qualifiedName(), publicOnly)) {
      return findClassField(base, project, module, member, publicOnly);
    }
  }
  return nullptr;
}

const FunDecl *findClassMethod(const ClassDecl *cls,
                               const ProjectState &project,
                               const sema::ModuleInfo &module,
                               std::string_view member,
                               bool publicOnly = false) {
  if (!cls) {
    return nullptr;
  }
  for (const auto &method : cls->methods_) {
    if (!method || method->name_ != member) {
      continue;
    }
    if (publicOnly && method->visibility_ != Visibility::Public) {
      continue;
    }
    return method.get();
  }
  if (cls->baseType_) {
    if (auto base = resolveClassByTypeName(
            project, module, cls->baseType_->qualifiedName(), publicOnly)) {
      return findClassMethod(base, project, module, member, publicOnly);
    }
  }
  return nullptr;
}

std::optional<HoverInfo> hoverForClassMember(const ClassDecl *cls,
                                             const ProjectState &project,
                                             const sema::ModuleInfo &module,
                                             std::string_view member,
                                             bool publicOnly = false) {
  if (auto method = findClassMethod(cls, project, module, member, publicOnly)) {
    if (auto signature = signatureForNode(method)) {
      return HoverInfo{"zap", signature->label};
    }
  }
  if (auto field = findClassField(cls, project, module, member, publicOnly)) {
    return HoverInfo{"zap", field->name + ": " + renderType(field->type.get())};
  }
  return std::nullopt;
}

std::optional<CallContext> callContextAtOffset(const std::string &source,
                                               size_t offset) {
  if (source.empty()) {
    return std::nullopt;
  }

  size_t pos = std::min(offset, source.size());
  int nestedParens = 0;
  int nestedBrackets = 0;
  int nestedBraces = 0;
  int64_t activeParameter = 0;
  bool foundOpenParen = false;
  size_t openParen = 0;

  while (pos > 0) {
    --pos;
    char ch = source[pos];
    if (ch == ')') {
      ++nestedParens;
    } else if (ch == '(') {
      if (nestedParens == 0 && nestedBrackets == 0 && nestedBraces == 0) {
        openParen = pos;
        foundOpenParen = true;
        break;
      }
      if (nestedParens > 0) {
        --nestedParens;
      }
    } else if (ch == ']') {
      ++nestedBrackets;
    } else if (ch == '[') {
      if (nestedBrackets > 0) {
        --nestedBrackets;
      }
    } else if (ch == '}') {
      ++nestedBraces;
    } else if (ch == '{') {
      if (nestedBraces > 0) {
        --nestedBraces;
      }
    } else if (ch == ',' && nestedParens == 0 && nestedBrackets == 0 &&
               nestedBraces == 0) {
      ++activeParameter;
    }
  }

  if (!foundOpenParen) {
    return std::nullopt;
  }

  size_t end = openParen;
  while (end > 0 && std::isspace(static_cast<unsigned char>(source[end - 1]))) {
    --end;
  }
  size_t start = end;
  while (start > 0) {
    char ch = source[start - 1];
    if (isIdentifierChar(ch) || ch == '.' ||
        std::isspace(static_cast<unsigned char>(ch))) {
      --start;
      continue;
    }
    break;
  }

  std::string callee = source.substr(start, end - start);
  callee.erase(
      std::remove_if(callee.begin(), callee.end(),
                     [](unsigned char ch) { return std::isspace(ch); }),
      callee.end());
  if (callee.empty()) {
    return std::nullopt;
  }

  return CallContext{std::move(callee), activeParameter};
}

std::optional<CallContext> callContextNearOffset(const std::string &source,
                                                 size_t offset) {
  if (auto call = callContextAtOffset(source, offset)) {
    return call;
  }

  if (source.empty()) {
    return std::nullopt;
  }

  size_t pos = std::min(offset, source.size() - 1);
  if (isIdentifierChar(source[pos])) {
    size_t end = pos + 1;
    while (end < source.size() && isIdentifierChar(source[end])) {
      ++end;
    }
    while (end < source.size() &&
           std::isspace(static_cast<unsigned char>(source[end]))) {
      ++end;
    }
    if (end < source.size() && source[end] == '(') {
      return callContextAtOffset(source, end + 1);
    }
  }

  if (pos < source.size() && source[pos] == '(') {
    return callContextAtOffset(source, pos + 1);
  }

  return std::nullopt;
}

std::vector<LspSignature> resolveSignatures(const std::string &source,
                                            const std::string &uri,
                                            const ProjectState &project,
                                            size_t offset,
                                            int64_t &activeParameter) {
  auto path = uriToPath(uri);
  if (!path) {
    return {};
  }

  auto call = callContextNearOffset(source, offset);
  if (!call) {
    return {};
  }
  activeParameter = call->activeParameter;

  std::string moduleId = std::filesystem::weakly_canonical(*path).string();
  auto moduleIt = project.moduleMap.find(moduleId);
  if (moduleIt == project.moduleMap.end()) {
    return {};
  }
  const sema::ModuleInfo &module = *moduleIt->second;

  auto dot = call->callee.find('.');
  if (dot != std::string::npos) {
    std::string base = call->callee.substr(0, dot);
    std::string member = call->callee.substr(dot + 1);
    for (const auto &import : module.imports) {
      for (const auto &targetId : import.targetModuleIds) {
        auto targetIt = project.moduleMap.find(targetId);
        if (targetIt == project.moduleMap.end()) {
          continue;
        }
        if (effectiveImportAlias(import, *targetIt->second) != base) {
          continue;
        }
        auto signatures =
            findTopLevelSignatures(*targetIt->second, member, true);
        if (!signatures.empty()) {
          return signatures;
        }
      }
    }
    return {};
  }

  auto localSignatures = findTopLevelSignatures(module, call->callee);
  if (!localSignatures.empty()) {
    return localSignatures;
  }

  for (const auto &import : module.imports) {
    for (const auto &binding : import.bindings) {
      if (binding.localName != call->callee) {
        continue;
      }
      for (const auto &targetId : import.targetModuleIds) {
        auto targetIt = project.moduleMap.find(targetId);
        if (targetIt == project.moduleMap.end()) {
          continue;
        }
        auto signatures =
            findTopLevelSignatures(*targetIt->second, binding.sourceName, true);
        if (!signatures.empty()) {
          return signatures;
        }
      }
    }
  }

  return {};
}

std::optional<HoverInfo> hoverForNode(const Node *node) {
  if (dynamic_cast<const FunDecl *>(node)) {
    if (auto signature = signatureForNode(node)) {
      return HoverInfo{"zap", signature->label};
    }
  }
  if (dynamic_cast<const ExtDecl *>(node)) {
    if (auto signature = signatureForNode(node)) {
      return HoverInfo{"zap", "ext fun " + signature->label};
    }
  }
  if (auto var = dynamic_cast<const VarDecl *>(node)) {
    return HoverInfo{"zap",
                     "var " + var->name_ + ": " + renderType(var->type_.get())};
  }
  if (auto cnst = dynamic_cast<const ConstDecl *>(node)) {
    return HoverInfo{"zap", "const " + cnst->name_ + ": " +
                                renderType(cnst->type_.get())};
  }
  if (auto record = dynamic_cast<const RecordDecl *>(node)) {
    std::string hover = "record " + record->name_;
    if (!record->genericParams_.empty()) {
      hover += "<";
      for (size_t i = 0; i < record->genericParams_.size(); ++i) {
        if (i != 0) {
          hover += ", ";
        }
        hover += record->genericParams_[i]
                     ? record->genericParams_[i]->qualifiedName()
                     : std::string("?");
      }
      hover += ">";
    }
    if (!record->genericConstraints_.empty()) {
      hover += " where ";
      for (size_t i = 0; i < record->genericConstraints_.size(); ++i) {
        if (i != 0) {
          hover += ", ";
        }
        hover += record->genericConstraints_[i].parameterName + ": ";
        hover += record->genericConstraints_[i].boundType
                     ? renderType(record->genericConstraints_[i].boundType.get())
                     : std::string("?");
      }
    }
    return HoverInfo{"zap", hover};
  }
  if (auto strukt = dynamic_cast<const StructDeclarationNode *>(node)) {
    std::string hover = "struct " + strukt->name_;
    if (!strukt->genericParams_.empty()) {
      hover += "<";
      for (size_t i = 0; i < strukt->genericParams_.size(); ++i) {
        if (i != 0) {
          hover += ", ";
        }
        hover += strukt->genericParams_[i]
                     ? strukt->genericParams_[i]->qualifiedName()
                     : std::string("?");
      }
      hover += ">";
    }
    if (!strukt->genericConstraints_.empty()) {
      hover += " where ";
      for (size_t i = 0; i < strukt->genericConstraints_.size(); ++i) {
        if (i != 0) {
          hover += ", ";
        }
        hover += strukt->genericConstraints_[i].parameterName + ": ";
        hover += strukt->genericConstraints_[i].boundType
                     ? renderType(strukt->genericConstraints_[i].boundType.get())
                     : std::string("?");
      }
    }
    return HoverInfo{"zap", hover};
  }
  if (auto cls = dynamic_cast<const ClassDecl *>(node)) {
    std::string hover = "class " + cls->name_;
    if (!cls->genericParams_.empty()) {
      hover += "<";
      for (size_t i = 0; i < cls->genericParams_.size(); ++i) {
        if (i != 0) {
          hover += ", ";
        }
        hover += cls->genericParams_[i]
                     ? cls->genericParams_[i]->qualifiedName()
                     : std::string("?");
      }
      hover += ">";
    }
    if (cls->baseType_) {
      hover += " : " + renderType(cls->baseType_.get());
    }
    if (!cls->genericConstraints_.empty()) {
      hover += " where ";
      for (size_t i = 0; i < cls->genericConstraints_.size(); ++i) {
        if (i != 0) {
          hover += ", ";
        }
        hover += cls->genericConstraints_[i].parameterName + ": ";
        hover += cls->genericConstraints_[i].boundType
                     ? renderType(cls->genericConstraints_[i].boundType.get())
                     : std::string("?");
      }
    }
    return HoverInfo{"zap", hover};
  }
  if (auto enm = dynamic_cast<const EnumDecl *>(node)) {
    return HoverInfo{"zap", "enum " + enm->name_};
  }
  if (auto alias = dynamic_cast<const TypeAliasDecl *>(node)) {
    return HoverInfo{"zap", "alias " + alias->name_ + " = " +
                                renderType(alias->type_.get())};
  }
  return std::nullopt;
}

std::optional<HoverInfo> findTopLevelHover(const sema::ModuleInfo &module,
                                           std::string_view name,
                                           bool publicOnly = false) {
  for (const auto &child : module.root->children) {
    const TopLevel *topLevel = dynamic_cast<const TopLevel *>(child.get());
    auto childName = topLevelNameForNode(child.get());
    if (!childName || *childName != name) {
      continue;
    }
    if (publicOnly &&
        (!topLevel || topLevel->visibility_ != Visibility::Public)) {
      continue;
    }
    if (auto hover = hoverForNode(child.get())) {
      return hover;
    }
  }
  return std::nullopt;
}

JsonObject makeHover(const HoverInfo &hover) {
  JsonObject::Object contents;
  contents.emplace("kind", JsonObject("markdown"));
  contents.emplace("value", JsonObject("```" + hover.language + "\n" +
                                       hover.value + "\n```"));

  JsonObject::Object result;
  result.emplace("contents", JsonObject(std::move(contents)));
  return JsonObject(std::move(result));
}

std::optional<HoverInfo> resolveHover(const std::string &source,
                                      const std::string &uri,
                                      const ProjectState &project,
                                      size_t offset) {
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

  if (auto qualified = qualifiedIdentifierAtOffset(source, offset)) {
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
          if (auto hover =
                  findTopLevelHover(*targetIt->second, memberName, true)) {
            return hover;
          }
        }
      }

      if (base.find('.') == std::string::npos) {
        if (auto classType =
                resolveVariableClassType(project, module, offset, base)) {
          if (auto cls = resolveClassByTypeName(project, module, *classType)) {
            if (auto hover =
                    hoverForClassMember(cls, project, module, memberName)) {
              return hover;
            }
          }
        }
      }
    }
  }

  auto name = identifierAt(source, offset);
  if (!name) {
    return std::nullopt;
  }

  if (auto hover = findTopLevelHover(module, *name)) {
    return hover;
  }

  for (const auto &import : module.imports) {
    for (const auto &binding : import.bindings) {
      if (binding.localName != *name) {
        continue;
      }
      for (const auto &targetId : import.targetModuleIds) {
        auto targetIt = project.moduleMap.find(targetId);
        if (targetIt == project.moduleMap.end()) {
          continue;
        }
        if (auto hover = findTopLevelHover(*targetIt->second,
                                           binding.sourceName, true)) {
          return hover;
        }
      }
    }
  }

  return std::nullopt;
}

JsonObject makeSignatureHelp(const std::vector<LspSignature> &signatures,
                             int64_t activeSignature, int64_t activeParameter) {
  JsonObject::List signatureItems;
  signatureItems.reserve(signatures.size());
  for (const auto &signature : signatures) {
    JsonObject::List parameters;
    parameters.reserve(signature.parameters.size());
    for (const auto &param : signature.parameters) {
      JsonObject::Object parameter;
      parameter.emplace("label", JsonObject(param));
      parameters.push_back(JsonObject(std::move(parameter)));
    }

    JsonObject::Object sig;
    sig.emplace("label", JsonObject(signature.label));
    sig.emplace("parameters", JsonObject(std::move(parameters)));
    signatureItems.push_back(JsonObject(std::move(sig)));
  }

  int64_t clampedSignature =
      signatures.empty()
          ? 0
          : std::max<int64_t>(0, std::min<int64_t>(activeSignature,
                                                   static_cast<int64_t>(
                                                       signatures.size() - 1)));
  int64_t clampedParameter = 0;
  if (!signatures.empty()) {
    clampedParameter = std::max<int64_t>(
        0, std::min<int64_t>(
               activeParameter,
               static_cast<int64_t>(
                   signatures[clampedSignature].parameters.empty()
                       ? 0
                       : signatures[clampedSignature].parameters.size() - 1)));
  }

  JsonObject::Object result;
  result.emplace("signatures", JsonObject(std::move(signatureItems)));
  result.emplace("activeSignature", JsonObject(clampedSignature));
  result.emplace("activeParameter", JsonObject(clampedParameter));
  return JsonObject(std::move(result));
}

std::string effectiveImportAlias(const sema::ResolvedImport &import,
                                 const sema::ModuleInfo &target) {
  return import.moduleAlias.empty() ? target.moduleName : import.moduleAlias;
}

JsonObject makeRange(const zap::DiagnosticRange &range);

std::optional<LspSymbol> topLevelSymbolForNode(const Node *node,
                                               const std::string &uri) {
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
    return makeSymbol(uri, record->name_, record->span, 22,
                      record->visibility_);
  }
  if (auto strukt = dynamic_cast<const StructDeclarationNode *>(node)) {
    return makeSymbol(uri, strukt->name_, strukt->span, 22,
                      strukt->visibility_);
  }
  if (auto cls = dynamic_cast<const ClassDecl *>(node)) {
    return makeSymbol(uri, cls->name_, cls->span, 5, cls->visibility_);
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

void collectLocalsInBody(const BodyNode *body, size_t offset,
                         const std::string &uri,
                         std::vector<LspSymbol> &symbols);

void collectLocalsFromNode(const Node *node, size_t offset,
                           const std::string &uri,
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

void collectLocalsInBody(const BodyNode *body, size_t offset,
                         const std::string &uri,
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
  zap::DiagnosticRange range{{span.line, span.column, span.offset},
                             {span.line,
                              span.column + std::max<size_t>(span.length, 1),
                              span.offset + span.length}};
  JsonObject::Object object;
  object.emplace("uri", JsonObject(uri));
  object.emplace("range", makeRange(range));
  return JsonObject(std::move(object));
}

JsonObject makeCompletionItem(const LspSymbol &symbol,
                              const std::string &detail = "") {
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

  std::optional<std::string>
  sourceForPath(const std::filesystem::path &path) const {
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

  void open(const std::string &uri, std::filesystem::path path,
            std::string text, int64_t version) {
    std::filesystem::path canonicalPath =
        std::filesystem::weakly_canonical(path);
    std::string canonicalKey = canonicalPath.string();

    auto existingUriIt = uriByCanonicalPath_.find(canonicalKey);
    if (existingUriIt != uriByCanonicalPath_.end() && existingUriIt->second != uri) {
      documentsByUri_.erase(existingUriIt->second);
      existingUriIt->second = uri;
    } else {
      uriByCanonicalPath_[canonicalKey] = uri;
    }

    documentsByUri_[uri] =
        DocumentState{uri, std::move(canonicalPath), std::move(text), version};
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

    std::string canonicalKey = it->second.path.string();
    auto pathIt = uriByCanonicalPath_.find(canonicalKey);
    if (pathIt != uriByCanonicalPath_.end() && pathIt->second == uri) {
      uriByCanonicalPath_.erase(pathIt);
    }

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
    if (!loadModuleGraph(docIt->second.path, state.moduleMap, visiting,
                         state.analysis, uri, allowEntryErrors)) {
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
    std::string entryId =
        std::filesystem::weakly_canonical(docIt->second.path).string();
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

    zap::DiagnosticEngine diagnostics(*entrySource,
                                      docIt->second.path.string());
    std::vector<sema::ModuleInfo> modules;
    modules.reserve(project->moduleMap.size());
    for (auto &[_, module] : project->moduleMap) {
      modules.push_back(std::move(*module));
    }

    auto flags = findAndReadFlags(docIt->second.path);

    sema::Binder binder(diagnostics, flags.allowUnsafe);
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
  start.emplace("line", JsonObject(static_cast<int64_t>(
                            range.start.line > 0 ? range.start.line - 1 : 0)));
  start.emplace("character",
                JsonObject(static_cast<int64_t>(
                    range.start.column > 0 ? range.start.column - 1 : 0)));

  JsonObject::Object end;
  end.emplace("line", JsonObject(static_cast<int64_t>(
                          range.end.line > 0 ? range.end.line - 1 : 0)));
  end.emplace("character",
              JsonObject(static_cast<int64_t>(
                  range.end.column > 0 ? range.end.column - 1 : 0)));

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
      std::string targetUri = targetUriIt == project.uriByModuleId.end()
                                  ? pathToUri(targetId)
                                  : targetUriIt->second;

      symbols.push_back(*makeSymbol(project.uriByModuleId.at(module.moduleId),
                                    alias, import.span, 9, Visibility::Public));

      auto exported = collectTopLevelSymbols(target, targetUri, true);
      if (import.bindings.empty()) {
        continue;
      }
      for (const auto &binding : import.bindings) {
        auto exportIt = std::find_if(exported.begin(), exported.end(),
                                     [&](const LspSymbol &symbol) {
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
  auto docIt =
      project.moduleMap.find(std::filesystem::weakly_canonical(*path).string());
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
      {"Int", 22},     {"Int8", 22},   {"Int16", 22}, {"Int32", 22},
      {"Int64", 22},   {"UInt", 22},   {"UInt8", 22}, {"UInt16", 22},
      {"UInt32", 22},  {"UInt64", 22}, {"Float", 22}, {"Float32", 22},
      {"Float64", 22}, {"Bool", 22},   {"Void", 22},  {"Char", 22},
      {"String", 22}};
  for (const auto &[name, kind] : builtinTypes) {
    symbols.push_back(
        *makeSymbol(uri, name, SourceSpan(), kind, Visibility::Public));
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
          auto exported =
              collectTopLevelSymbols(*targetIt->second, targetUri, true);
          auto exportIt = std::find_if(exported.begin(), exported.end(),
                                       [&](const LspSymbol &symbol) {
                                         return symbol.name == memberName;
                                       });
          if (exportIt != exported.end()) {
            return *exportIt;
          }
        }
      }

      if (base.find('.') == std::string::npos) {
        if (auto classType =
                resolveVariableClassType(project, module, offset, base)) {
          if (auto cls = resolveClassByTypeName(project, module, *classType)) {
            if (auto field = findClassField(cls, project, module, memberName)) {
              return *makeSymbol(uri, field->name, field->span, 8,
                                 field->visibility_);
            }
            if (auto method =
                    findClassMethod(cls, project, module, memberName)) {
              return *makeSymbol(uri, method->name_, method->span, 2,
                                 method->visibility_);
            }
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
  if (!diagnostic.code.empty()) {
    object.emplace("code", JsonObject(diagnostic.code));
  }
  return JsonObject(std::move(object));
}

JsonObject
makePublishDiagnostics(std::string uri,
                       const std::vector<zap::Diagnostic> &diagnostics) {
  JsonObject::List items;
  items.reserve(diagnostics.size());
  for (const auto &diagnostic : diagnostics) {
    items.push_back(makeDiagnostic(diagnostic));
  }

  JsonObject::Object params;
  params.emplace("uri", JsonObject(std::move(uri)));
  params.emplace("diagnostics", JsonObject(std::move(items)));
  return makeNotification("textDocument/publishDiagnostics",
                          JsonObject(std::move(params)));
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
      capabilities.emplace("textDocumentSync",
                           JsonObject(std::move(syncOptions)));
      capabilities.emplace("definitionProvider", JsonObject(true));
      capabilities.emplace("hoverProvider", JsonObject(true));

      JsonObject::Object completionOptions;
      completionOptions.emplace("resolveProvider", JsonObject(false));
      completionOptions.emplace(
          "triggerCharacters",
          JsonObject(JsonObject::List{
              JsonObject("."), JsonObject("_"), JsonObject("a"),
              JsonObject("b"), JsonObject("c"), JsonObject("d"),
              JsonObject("e"), JsonObject("f"), JsonObject("g"),
              JsonObject("h"), JsonObject("i"), JsonObject("j"),
              JsonObject("k"), JsonObject("l"), JsonObject("m"),
              JsonObject("n"), JsonObject("o"), JsonObject("p"),
              JsonObject("q"), JsonObject("r"), JsonObject("s"),
              JsonObject("t"), JsonObject("u"), JsonObject("v"),
              JsonObject("w"), JsonObject("x"), JsonObject("y"),
              JsonObject("z"), JsonObject("A"), JsonObject("B"),
              JsonObject("C"), JsonObject("D"), JsonObject("E"),
              JsonObject("F"), JsonObject("G"), JsonObject("H"),
              JsonObject("I"), JsonObject("J"), JsonObject("K"),
              JsonObject("L"), JsonObject("M"), JsonObject("N"),
              JsonObject("O"), JsonObject("P"), JsonObject("Q"),
              JsonObject("R"), JsonObject("S"), JsonObject("T"),
              JsonObject("U"), JsonObject("V"), JsonObject("W"),
              JsonObject("X"), JsonObject("Y"), JsonObject("Z")}));
      capabilities.emplace("completionProvider",
                           JsonObject(std::move(completionOptions)));

      JsonObject::Object signatureHelpOptions;
      signatureHelpOptions.emplace(
          "triggerCharacters",
          JsonObject(JsonObject::List{JsonObject("("), JsonObject(",")}));
      capabilities.emplace("signatureHelpProvider",
                           JsonObject(std::move(signatureHelpOptions)));

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
      auto version =
          getIntegerField(request, {"params", "textDocument", "version"})
              .value_or(0);

      if (uri && text) {
        auto path = uriToPath(*uri);
        if (path) {
          workspace.open(*uri, *path, *text, version);
          publishAnalysis(server, workspace.analyze(*uri));
        }
      }
    } else if (*method == "textDocument/didChange") {
      auto uri = getStringField(request, {"params", "textDocument", "uri"});
      auto version =
          getIntegerField(request, {"params", "textDocument", "version"})
              .value_or(0);
      const JsonObject *changes =
          getPath(request, {"params", "contentChanges"});

      if (uri && changes && changes->isList() &&
          !changes->getAsList().empty()) {
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
      auto character =
          getIntegerField(request, {"params", "position", "character"});
      if (id && uri && line && character) {
        JsonObject::List items;
        if (const auto *document = workspace.document(*uri)) {
          auto project = workspace.loadProject(*uri, true);
          if (project) {
            size_t offset =
                offsetFromPosition(document->text, *line, *character);
            if (auto member =
                    memberAccessBeforeCursor(document->text, offset)) {
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
                    if (effectiveImportAlias(import, *targetIt->second) !=
                        base) {
                      continue;
                    }
                    auto targetUriIt = project->uriByModuleId.find(targetId);
                    std::string targetUri =
                        targetUriIt == project->uriByModuleId.end()
                            ? pathToUri(targetId)
                            : targetUriIt->second;
                    for (const auto &symbol : collectTopLevelSymbols(
                             *targetIt->second, targetUri, true)) {
                      items.push_back(
                          makeCompletionItem(symbol, "imported member"));
                    }
                  }
                }

                if (base.find('.') == std::string::npos) {
                  if (auto classType = resolveVariableClassType(
                          *project, *moduleIt->second, offset, base)) {
                    if (auto cls = resolveClassByTypeName(
                            *project, *moduleIt->second, *classType)) {
                      std::set<std::string> seenMembers;
                      const ClassDecl *current = cls;
                      while (current) {
                        for (const auto &field : current->fields_) {
                          if (!field ||
                              !seenMembers.insert(field->name).second) {
                            continue;
                          }
                          items.push_back(makeCompletionItem(
                              *makeSymbol(*uri, field->name, field->span, 8,
                                          field->visibility_),
                              "field"));
                        }
                        for (const auto &method : current->methods_) {
                          if (!method ||
                              !seenMembers.insert(method->name_).second) {
                            continue;
                          }
                          auto detail = method->name_;
                          if (auto signature = signatureForNode(method.get())) {
                            detail = signature->label;
                          }
                          items.push_back(makeCompletionItem(
                              *makeSymbol(*uri, method->name_, method->span, 2,
                                          method->visibility_),
                              detail));
                        }
                        if (!current->baseType_) {
                          break;
                        }
                        current = resolveClassByTypeName(
                            *project, *moduleIt->second,
                            current->baseType_->qualifiedName());
                      }
                    }
                  }
                }
              }
            } else {
              std::vector<LspSymbol> symbols =
                  collectCompletionSymbols(*uri, *project, offset);
              std::set<std::string> seen;
              static constexpr const char *keywords[] = {
                  "fun",    "return",   "if",      "else",   "iftype", "while",
                  "var",    "const",    "import",  "pub",    "priv",   "prot",
                  "struct", "record",   "class",   "enum",   "alias",  "ext",
                  "global", "break",    "continue","ref",    "as",     "new",
                  "self",   "where",    "unsafe",  "weak"};
              for (const char *keyword : keywords) {
                if (seen.insert(keyword).second) {
                  items.push_back(makeCompletionItem(
                      *makeSymbol(*uri, keyword, SourceSpan(), 14), "keyword"));
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
      auto character =
          getIntegerField(request, {"params", "position", "character"});
      if (id && uri && line && character) {
        JsonObject result(nullptr);
        if (const auto *document = workspace.document(*uri)) {
          auto project = workspace.loadProject(*uri, true);
          if (project) {
            size_t offset =
                offsetFromPosition(document->text, *line, *character);
            auto symbol = resolveDefinition(workspace, *uri, *project, offset);
            if (symbol) {
              result = makeLocation(symbol->uri, symbol->span);
            }
          }
        }
        server.sendMessage(makeResponse(id, std::move(result)));
      }
    } else if (*method == "textDocument/hover") {
      auto uri = getStringField(request, {"params", "textDocument", "uri"});
      auto line = getIntegerField(request, {"params", "position", "line"});
      auto character =
          getIntegerField(request, {"params", "position", "character"});
      if (id && uri && line && character) {
        JsonObject result(nullptr);
        if (const auto *document = workspace.document(*uri)) {
          auto project = workspace.loadProject(*uri, true);
          if (project) {
            size_t offset =
                offsetFromPosition(document->text, *line, *character);
            auto hover = resolveHover(document->text, *uri, *project, offset);
            if (hover) {
              result = makeHover(*hover);
            }
          }
        }
        server.sendMessage(makeResponse(id, std::move(result)));
      }
    } else if (*method == "textDocument/signatureHelp") {
      auto uri = getStringField(request, {"params", "textDocument", "uri"});
      auto line = getIntegerField(request, {"params", "position", "line"});
      auto character =
          getIntegerField(request, {"params", "position", "character"});
      if (id && uri && line && character) {
        JsonObject result(nullptr);
        if (const auto *document = workspace.document(*uri)) {
          auto project = workspace.loadProject(*uri, true);
          if (project) {
            size_t offset =
                offsetFromPosition(document->text, *line, *character);
            int64_t activeParameter = 0;
            auto signatures = resolveSignatures(document->text, *uri, *project,
                                                offset, activeParameter);
            if (!signatures.empty()) {
              int64_t activeSignature = 0;
              for (size_t i = 0; i < signatures.size(); ++i) {
                if (activeParameter <
                    static_cast<int64_t>(std::max<size_t>(
                        size_t(1), signatures[i].parameters.size()))) {
                  activeSignature = static_cast<int64_t>(i);
                  break;
                }
              }
              result = makeSignatureHelp(signatures, activeSignature,
                                         activeParameter);
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
