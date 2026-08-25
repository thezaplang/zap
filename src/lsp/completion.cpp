#include "lsp/language_features.hpp"

#include "lsp/language_feature_helpers.hpp"
#include "lsp/protocol_messages.hpp"
#include "lsp/protocol_utils.hpp"
#include "lsp/symbol_index.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace zap::lsp {

bool matchesPrefix(std::string_view name, std::string_view prefix) {
  return prefix.empty() || name.rfind(prefix, 0) == 0;
}

struct StructLiteralCompletionContext {
  std::string typeName;
  std::string fieldPrefix;
  std::set<std::string> initializedFields;
};

std::optional<size_t> enclosingBraceBeforeOffset(const std::string &source,
                                                 size_t offset) {
  size_t pos = std::min(offset, source.size());
  int nestedBraces = 0;
  while (pos > 0) {
    --pos;
    char ch = source[pos];
    if (ch == '}') {
      ++nestedBraces;
      continue;
    }
    if (ch == '{') {
      if (nestedBraces == 0) {
        return pos;
      }
      --nestedBraces;
    }
  }
  return std::nullopt;
}

std::optional<std::string> typeNameBeforeBrace(const std::string &source,
                                               size_t brace) {
  size_t end = brace;
  while (end > 0 && std::isspace(static_cast<unsigned char>(source[end - 1]))) {
    --end;
  }

  size_t start = end;
  int angleDepth = 0;
  while (start > 0) {
    char ch = source[start - 1];
    if (ch == '>') {
      ++angleDepth;
      --start;
      continue;
    }
    if (ch == '<') {
      if (angleDepth == 0) {
        break;
      }
      --angleDepth;
      --start;
      continue;
    }
    if (angleDepth > 0) {
      --start;
      continue;
    }
    if (isIdentifierChar(ch) || ch == '.') {
      --start;
      continue;
    }
    break;
  }

  if (start == end) {
    return std::nullopt;
  }
  return source.substr(start, end - start);
}

std::optional<StructLiteralCompletionContext>
structLiteralCompletionAtCursor(const std::string &source, size_t offset) {
  auto brace = enclosingBraceBeforeOffset(source, offset);
  if (!brace) {
    return std::nullopt;
  }
  auto typeName = typeNameBeforeBrace(source, *brace);
  if (!typeName) {
    return std::nullopt;
  }

  size_t cursor = std::min(offset, source.size());
  size_t fieldStart = *brace + 1;
  int nestedParens = 0;
  int nestedBrackets = 0;
  int nestedBraces = 0;
  StructLiteralCompletionContext context{*typeName, "", {}};

  for (size_t i = *brace + 1; i < cursor; ++i) {
    char ch = source[i];
    if (ch == '(') {
      ++nestedParens;
      continue;
    }
    if (ch == ')' && nestedParens > 0) {
      --nestedParens;
      continue;
    }
    if (ch == '[') {
      ++nestedBrackets;
      continue;
    }
    if (ch == ']' && nestedBrackets > 0) {
      --nestedBrackets;
      continue;
    }
    if (ch == '{') {
      ++nestedBraces;
      continue;
    }
    if (ch == '}' && nestedBraces > 0) {
      --nestedBraces;
      continue;
    }

    bool atTopLevel =
        nestedParens == 0 && nestedBrackets == 0 && nestedBraces == 0;
    if (!atTopLevel) {
      continue;
    }

    if (ch == ',') {
      fieldStart = i + 1;
      continue;
    }

    if (std::isalpha(static_cast<unsigned char>(ch)) || ch == '_') {
      size_t nameStart = i;
      size_t nameEnd = i + 1;
      while (nameEnd < cursor && isIdentifierChar(source[nameEnd])) {
        ++nameEnd;
      }
      size_t afterName = nameEnd;
      while (afterName < cursor &&
             std::isspace(static_cast<unsigned char>(source[afterName]))) {
        ++afterName;
      }
      if (afterName < cursor && source[afterName] == ':') {
        context.initializedFields.insert(
            source.substr(nameStart, nameEnd - nameStart));
      }
      i = nameEnd - 1;
    }
  }

  nestedParens = 0;
  nestedBrackets = 0;
  nestedBraces = 0;
  for (size_t i = fieldStart; i < cursor; ++i) {
    char ch = source[i];
    if (ch == '(') {
      ++nestedParens;
    } else if (ch == ')' && nestedParens > 0) {
      --nestedParens;
    } else if (ch == '[') {
      ++nestedBrackets;
    } else if (ch == ']' && nestedBrackets > 0) {
      --nestedBrackets;
    } else if (ch == '{') {
      ++nestedBraces;
    } else if (ch == '}' && nestedBraces > 0) {
      --nestedBraces;
    } else if (ch == ':' && nestedParens == 0 && nestedBrackets == 0 &&
               nestedBraces == 0) {
      return std::nullopt;
    }
  }

  size_t end = cursor;
  while (end > fieldStart &&
         std::isspace(static_cast<unsigned char>(source[end - 1]))) {
    --end;
  }
  size_t start = end;
  while (start > fieldStart && isIdentifierChar(source[start - 1])) {
    --start;
  }
  if (start < end) {
    context.fieldPrefix = source.substr(start, end - start);
  }
  return context;
}

std::vector<const ParameterNode *>
findRecordLikeFields(const sema::ModuleInfo &module, std::string_view name,
                     bool publicOnly = false) {
  for (const auto &child : module.root->children) {
    auto topLevel = dynamic_cast<const TopLevel *>(child.get());
    if (auto record = dynamic_cast<const RecordDecl *>(child.get())) {
      if (record->name_ != name ||
          (publicOnly &&
           (!topLevel || topLevel->visibility_ != Visibility::Public))) {
        continue;
      }
      std::vector<const ParameterNode *> fields;
      for (const auto &field : record->fields_) {
        fields.push_back(field.get());
      }
      return fields;
    }
    if (auto strukt =
            dynamic_cast<const StructDeclarationNode *>(child.get())) {
      if (strukt->name_ != name ||
          (publicOnly &&
           (!topLevel || topLevel->visibility_ != Visibility::Public))) {
        continue;
      }
      std::vector<const ParameterNode *> fields;
      for (const auto &field : strukt->fields_) {
        fields.push_back(field.get());
      }
      return fields;
    }
  }
  return {};
}

std::vector<const ParameterNode *> resolveRecordLikeFieldsInModule(
    const ProjectState &project, const sema::ModuleInfo &module,
    std::string_view name, bool publicOnly, std::set<std::string> &visited) {
  std::string moduleId = module.moduleId;
  if (visited.count(moduleId)) {
    return {};
  }
  visited.insert(moduleId);

  auto fields = findRecordLikeFields(module, name, publicOnly);
  if (!fields.empty()) {
    return fields;
  }

  for (const auto &import : module.imports) {
    bool isImplicitPreludeImport =
        import.rawPath == "std/prelude" && import.moduleAlias.empty();
    if (isImplicitPreludeImport) {
      for (const auto &targetId : import.targetModuleIds) {
        auto targetIt = project.moduleMap.find(targetId);
        if (targetIt != project.moduleMap.end()) {
          auto importedFields = resolveRecordLikeFieldsInModule(
              project, *targetIt->second, name, true, visited);
          if (!importedFields.empty()) {
            return importedFields;
          }
        }
      }
    }

    for (const auto &binding : import.bindings) {
      if (binding.localName == name) {
        for (const auto &targetId : import.targetModuleIds) {
          auto targetIt = project.moduleMap.find(targetId);
          if (targetIt != project.moduleMap.end()) {
            auto importedFields = resolveRecordLikeFieldsInModule(
                project, *targetIt->second, binding.sourceName, true, visited);
            if (!importedFields.empty()) {
              return importedFields;
            }
          }
        }
      }
    }
  }

  return {};
}

std::vector<const ParameterNode *> resolveRecordLikeFieldsByTypeName(
    const ProjectState &project, const sema::ModuleInfo &module,
    std::string_view typeName, bool publicOnly = false) {
  std::string normalizedTypeName = stripGenericArguments(typeName);
  auto [qualifier, localName] = splitQualifiedName(normalizedTypeName);
  if (localName.empty()) {
    return {};
  }
  if (qualifier.empty()) {
    std::set<std::string> visited;
    return resolveRecordLikeFieldsInModule(project, module, localName,
                                           publicOnly, visited);
  }
  auto importedModule = findImportedModuleByAlias(project, module, qualifier);
  if (!importedModule) {
    return {};
  }
  std::set<std::string> visited;
  return resolveRecordLikeFieldsInModule(project, *importedModule, localName,
                                         publicOnly, visited);
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
  auto topLevel = collectTopLevelSymbols(module, uri, false);
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

JsonObject::List makeCompletionItems(const std::string &uri,
                                     const std::string &source,
                                     const ProjectState &project,
                                     size_t offset) {
  JsonObject::List items;
  std::string moduleId;
  auto path = uriToPath(uri);
  if (path) {
    moduleId = std::filesystem::weakly_canonical(*path).string();
  }
  auto moduleIt = project.moduleMap.find(moduleId);

  if (auto literal = structLiteralCompletionAtCursor(source, offset);
      literal && moduleIt != project.moduleMap.end()) {
    auto fields = resolveRecordLikeFieldsByTypeName(project, *moduleIt->second,
                                                    literal->typeName);
    if (!fields.empty()) {
      std::set<std::string> seenFields;
      for (const auto *field : fields) {
        if (!field || !matchesPrefix(field->name, literal->fieldPrefix) ||
            literal->initializedFields.count(field->name) ||
            !seenFields.insert(field->name).second) {
          continue;
        }
        items.push_back(makeCompletionItem(
            *makeSymbol(uri, field->name, field->span, 8, field->visibility_),
            "field"));
      }
      return items;
    }
  }

  if (auto member = memberAccessAtCursor(source, offset)) {
    const auto &[base, memberPrefix] = *member;
    if (moduleIt == project.moduleMap.end()) {
      return items;
    }

    std::set<std::string> seenImportedMembers;
    if (const auto *targetId = project.semanticInfo.importedModuleFor(
            moduleIt->second->moduleId, base)) {
      auto targetIt = project.moduleMap.find(*targetId);
      if (targetIt != project.moduleMap.end()) {
        std::set<std::string> visited;
        for (const auto &symbol : collectExportedSymbolsRecursive(
                 project, *targetIt->second, visited)) {
          if (matchesPrefix(symbol.name, memberPrefix) &&
              seenImportedMembers.insert(symbol.name).second) {
            items.push_back(makeCompletionItem(symbol, "imported member"));
          }
        }
      }
    }
    for (const auto &import : moduleIt->second->imports) {
      for (const auto &targetId : import.targetModuleIds) {
        auto targetIt = project.moduleMap.find(targetId);
        if (targetIt == project.moduleMap.end()) {
          continue;
        }
        if (effectiveImportAlias(import, *targetIt->second) != base) {
          continue;
        }
        std::set<std::string> visited;
        for (const auto &symbol : collectExportedSymbolsRecursive(
                 project, *targetIt->second, visited)) {
          if (!matchesPrefix(symbol.name, memberPrefix)) {
            continue;
          }
          if (!seenImportedMembers.insert(symbol.name).second) {
            continue;
          }
          items.push_back(makeCompletionItem(symbol, "imported member"));
        }
      }
    }

    if (base.find('.') == std::string::npos) {
      if (auto classType = resolveVariableClassType(project, *moduleIt->second,
                                                    offset, base)) {
        if (auto cls = resolveClassByTypeName(project, *moduleIt->second,
                                              *classType, false)) {
          std::set<std::string> seenMembers;
          const ClassDecl *current = cls;
          while (current) {
            for (const auto &field : current->fields_) {
              if (!field || !matchesPrefix(field->name, memberPrefix) ||
                  !seenMembers.insert(field->name).second) {
                continue;
              }
              items.push_back(
                  makeCompletionItem(*makeSymbol(uri, field->name, field->span,
                                                 8, field->visibility_),
                                     "field"));
            }
            for (const auto &method : current->methods_) {
              if (!method || !matchesPrefix(method->name_, memberPrefix) ||
                  !seenMembers.insert(method->name_).second) {
                continue;
              }
              auto detail = method->name_;
              if (auto signature = signatureForNode(method.get())) {
                detail = signature->label;
              }
              items.push_back(makeCompletionItem(
                  *makeSymbol(uri, method->name_, method->span, 2,
                              method->visibility_),
                  detail));
            }
            const ClassDecl *base = nullptr;
            for (const auto &implType : current->implementsList_) {
              base = resolveClassByTypeName(
                  project, *moduleIt->second, implType->qualifiedName(), false);
              if (base) {
                break;
              }
            }
            if (!base) {
              break;
            }
            current = base;
          }
        }
      }
    }

    if (auto recordType = resolveVariableRecordType(project, *moduleIt->second,
                                                    offset, base)) {
      std::set<std::string> seenFields;
      for (const auto &field : recordType->getFields()) {
        if (!matchesPrefix(field.name, memberPrefix) ||
            !seenFields.insert(field.name).second) {
          continue;
        }
        items.push_back(makeCompletionItem(
            *makeSymbol(uri, field.name, SourceSpan(), 8, Visibility::Private),
            "field"));
      }
    }
    return items;
  }

  std::vector<LspSymbol> symbols =
      collectCompletionSymbols(uri, project, offset);
  std::set<std::string> seen;
  static constexpr const char *keywords[] = {
      "fun",      "return", "if",     "else",     "iftype",  "while",  "var",
      "let",      "const",  "import", "pub",      "priv",    "prot",   "struct",
      "record",   "class",  "enum",   "alias",    "ext",     "global", "break",
      "continue", "ref",    "sink",   "noescape", "borrows", "as",     "new",
      "self",     "where",  "unsafe", "weak",     "fail",    "or",     "for",
      "case",     "module", "impl",   "static"};
  for (const char *keyword : keywords) {
    if (seen.insert(keyword).second) {
      items.push_back(makeCompletionItem(
          *makeSymbol(uri, keyword, SourceSpan(), 14, Visibility::Private),
          "keyword"));
    }
  }
  for (const auto &symbol : symbols) {
    if (seen.insert(symbol.name).second) {
      items.push_back(makeCompletionItem(symbol));
    }
  }
  return items;
}

} // namespace zap::lsp
