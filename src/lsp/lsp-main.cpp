#include "ast/nodes.hpp"
#include "lsp.hpp"
#include "lsp/protocol_messages.hpp"
#include "lsp/protocol_utils.hpp"
#include "lsp/symbol_index.hpp"
#include "lsp/workspace.hpp"
#include "lsp/workspace_types.hpp"
#include "sema/module_info.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>

using namespace zap::lsp;

namespace {

using JsonObject = zap::lsp::JsonObject;

std::string effectiveImportAlias(const sema::ResolvedImport &import,
                                 const sema::ModuleInfo &target);

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
  auto renderGenericParams =
      [](const std::vector<std::unique_ptr<TypeNode>> &genericParams) {
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
    std::string label =
        fun->name_ + renderGenericParams(fun->genericParams_) + "(";
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

const ClassDecl *resolveClassInModule(const ProjectState &project,
                                      const sema::ModuleInfo &module,
                                      std::string_view name, bool publicOnly,
                                      std::set<std::string> &visited) {
  std::string moduleId = module.moduleId;
  if (visited.count(moduleId)) {
    return nullptr;
  }
  visited.insert(moduleId);

  if (auto cls = findClassDecl(module, name, publicOnly)) {
    return cls;
  }

  for (const auto &import : module.imports) {
    bool isImplicitPreludeImport =
        import.rawPath == "std/prelude" && import.moduleAlias.empty();
    if (isImplicitPreludeImport) {
      for (const auto &targetId : import.targetModuleIds) {
        auto targetIt = project.moduleMap.find(targetId);
        if (targetIt != project.moduleMap.end()) {
          if (auto cls = resolveClassInModule(project, *targetIt->second, name,
                                              true, visited)) {
            return cls;
          }
        }
      }
    }

    for (const auto &binding : import.bindings) {
      if (binding.localName == name) {
        for (const auto &targetId : import.targetModuleIds) {
          auto targetIt = project.moduleMap.find(targetId);
          if (targetIt != project.moduleMap.end()) {
            if (auto cls =
                    resolveClassInModule(project, *targetIt->second,
                                         binding.sourceName, true, visited)) {
              return cls;
            }
          }
        }
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
    std::set<std::string> visited;
    return resolveClassInModule(project, module, localName, publicOnly,
                                visited);
  }
  auto importedModule = findImportedModuleByAlias(project, module, qualifier);
  if (!importedModule) {
    return nullptr;
  }
  std::set<std::string> visited;
  return resolveClassInModule(project, *importedModule, localName, publicOnly,
                              visited);
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

std::optional<std::string>
resolveClassTypeNameFromSemanticType(const ProjectState &project,
                                     const sema::ModuleInfo &module,
                                     const std::shared_ptr<zir::Type> &type) {
  if (!type || type->getKind() != zir::TypeKind::Class) {
    return std::nullopt;
  }
  auto classType = std::static_pointer_cast<zir::ClassType>(type);
  std::string name = classType->getName();
  if (isClassTypeName(project, module, name)) {
    return name;
  }
  auto [_, localName] = splitQualifiedName(name);
  if (!localName.empty() && isClassTypeName(project, module, localName)) {
    return localName;
  }
  return std::nullopt;
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

std::optional<std::string>
resolveVariableClassType(const ProjectState &project,
                         const sema::ModuleInfo &module, size_t offset,
                         std::string_view name) {
  if (name == "self") {
    if (auto cls = enclosingClassAtOffset(*module.root, offset)) {
      return cls->name_;
    }
  }
  auto visible = findVisibleSymbolInfo(*module.root, offset, name);
  if (visible.node) {
    if (auto type = resolveClassTypeNameFromSemanticType(
            project, module, project.semanticInfo.typeFor(visible.node))) {
      return type;
    }
  }
  if (visible.typeNode) {
    return resolveClassTypeNameFromTypeNode(project, module, visible.typeNode);
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

std::vector<LspSignature> resolveSignaturesInModuleRecursive(
    const ProjectState &project, const sema::ModuleInfo &module,
    std::string_view name, bool publicOnly, std::set<std::string> &visited) {
  std::string moduleId = module.moduleId;
  if (visited.count(moduleId)) {
    return {};
  }
  visited.insert(moduleId);

  auto localSignatures = findTopLevelSignatures(module, name, publicOnly);
  if (!localSignatures.empty()) {
    return localSignatures;
  }

  for (const auto &import : module.imports) {
    bool isImplicitPreludeImport =
        import.rawPath == "std/prelude" && import.moduleAlias.empty();
    if (isImplicitPreludeImport) {
      for (const auto &targetId : import.targetModuleIds) {
        auto targetIt = project.moduleMap.find(targetId);
        if (targetIt != project.moduleMap.end()) {
          auto signatures = resolveSignaturesInModuleRecursive(
              project, *targetIt->second, name, true, visited);
          if (!signatures.empty()) {
            return signatures;
          }
        }
      }
    }

    for (const auto &binding : import.bindings) {
      if (binding.localName == name) {
        for (const auto &targetId : import.targetModuleIds) {
          auto targetIt = project.moduleMap.find(targetId);
          if (targetIt != project.moduleMap.end()) {
            auto signatures = resolveSignaturesInModuleRecursive(
                project, *targetIt->second, binding.sourceName, true, visited);
            if (!signatures.empty()) {
              return signatures;
            }
          }
        }
      }
    }
  }

  return {};
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

  std::set<std::string> visited;
  return resolveSignaturesInModuleRecursive(project, module, call->callee,
                                            false, visited);
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
        hover +=
            record->genericConstraints_[i].boundType
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
        hover +=
            strukt->genericConstraints_[i].boundType
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

std::optional<HoverInfo> resolveHoverInModuleRecursive(
    const ProjectState &project, const sema::ModuleInfo &module,
    std::string_view name, bool publicOnly, std::set<std::string> &visited) {
  std::string moduleId = module.moduleId;
  if (visited.count(moduleId)) {
    return std::nullopt;
  }
  visited.insert(moduleId);

  if (auto hover = findTopLevelHover(module, name, publicOnly)) {
    return hover;
  }

  for (const auto &import : module.imports) {
    bool isImplicitPreludeImport =
        import.rawPath == "std/prelude" && import.moduleAlias.empty();
    if (isImplicitPreludeImport) {
      for (const auto &targetId : import.targetModuleIds) {
        auto targetIt = project.moduleMap.find(targetId);
        if (targetIt != project.moduleMap.end()) {
          if (auto hover = resolveHoverInModuleRecursive(
                  project, *targetIt->second, name, true, visited)) {
            return hover;
          }
        }
      }
    }

    for (const auto &binding : import.bindings) {
      if (binding.localName == name) {
        for (const auto &targetId : import.targetModuleIds) {
          auto targetIt = project.moduleMap.find(targetId);
          if (targetIt != project.moduleMap.end()) {
            if (auto hover = resolveHoverInModuleRecursive(
                    project, *targetIt->second, binding.sourceName, true,
                    visited)) {
              return hover;
            }
          }
        }
      }
    }
  }

  return std::nullopt;
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

  std::set<std::string> visited;
  return resolveHoverInModuleRecursive(project, module, *name, false, visited);
}

std::string effectiveImportAlias(const sema::ResolvedImport &import,
                                 const sema::ModuleInfo &target) {
  return import.moduleAlias.empty() ? target.moduleName : import.moduleAlias;
}

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

std::vector<LspSymbol>
collectExportedSymbolsRecursive(const ProjectState &project,
                                const sema::ModuleInfo &module,
                                std::set<std::string> &visited) {
  std::string moduleId = module.moduleId;
  if (visited.count(moduleId)) {
    return {};
  }
  visited.insert(moduleId);

  auto targetUriIt = project.uriByModuleId.find(moduleId);
  std::string targetUri = targetUriIt == project.uriByModuleId.end()
                              ? pathToUri(moduleId)
                              : targetUriIt->second;

  std::vector<LspSymbol> symbols =
      collectTopLevelSymbols(module, targetUri, true);

  for (const auto &import : module.imports) {
    if (import.visibility != Visibility::Public) {
      continue;
    }
    for (const auto &targetId : import.targetModuleIds) {
      auto targetIt = project.moduleMap.find(targetId);
      if (targetIt == project.moduleMap.end()) {
        continue;
      }
      auto subExports =
          collectExportedSymbolsRecursive(project, *targetIt->second, visited);
      if (import.bindings.empty()) {
        for (const auto &sym : subExports) {
          symbols.push_back(sym);
        }
      } else {
        for (const auto &binding : import.bindings) {
          auto exportIt =
              std::find_if(subExports.begin(), subExports.end(),
                           [&](const LspSymbol &symbol) {
                             return symbol.name == binding.sourceName;
                           });
          if (exportIt != subExports.end()) {
            LspSymbol local = *exportIt;
            local.name = binding.localName;
            symbols.push_back(std::move(local));
          }
        }
      }
    }
  }

  return symbols;
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

      std::set<std::string> visited;
      auto exported = collectExportedSymbolsRecursive(project, target, visited);
      bool isImplicitPreludeImport =
          import.rawPath == "std/prelude" && import.moduleAlias.empty();
      if (isImplicitPreludeImport) {
        for (const auto &sym : exported) {
          symbols.push_back(sym);
        }
      }

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
          std::set<std::string> visited;
          auto exported = collectExportedSymbolsRecursive(
              project, *targetIt->second, visited);
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
                    std::set<std::string> visited;
                    for (const auto &symbol : collectExportedSymbolsRecursive(
                             *project, *targetIt->second, visited)) {
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
                  "fun",    "return", "if",       "else", "iftype", "while",
                  "var",    "const",  "import",   "pub",  "priv",   "prot",
                  "struct", "record", "class",    "enum", "alias",  "ext",
                  "global", "break",  "continue", "ref",  "as",     "new",
                  "self",   "where",  "unsafe",   "weak", "fail",   "or",
                  "for",    "match",  "module",   "impl", "static"};
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

  return shutdownRequested ? 0 : 1;
}
