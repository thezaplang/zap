#include "binder.hpp"
#include "../ast/const/const_char.hpp"
#include "../ast/record_decl.hpp"
#include <algorithm>
#include <cctype>
#include <sstream>

namespace sema
{
  namespace
  {
    bool isStringType(const std::shared_ptr<zir::Type> &type)
    {
      return type && type->getKind() == zir::TypeKind::Record &&
             static_cast<zir::RecordType *>(type.get())->getName() == "String";
    }

    std::vector<std::string> splitQualified(const std::string &value)
    {
      std::vector<std::string> parts;
      std::stringstream ss(value);
      std::string item;
      while (std::getline(ss, item, '.'))
      {
        if (!item.empty())
        {
          parts.push_back(item);
        }
      }
      return parts;
    }

    bool extractQualifiedPath(const ExpressionNode *expr,
                              std::vector<std::string> &parts)
    {
      if (auto id = dynamic_cast<const ConstId *>(expr))
      {
        parts.push_back(id->value_);
        return true;
      }

      if (auto member = dynamic_cast<const MemberAccessNode *>(expr))
      {
        if (!extractQualifiedPath(member->left_.get(), parts))
        {
          return false;
        }
        parts.push_back(member->member_);
        return true;
      }

      return false;
    }
  } // namespace

  Binder::Binder(zap::DiagnosticEngine &diag, bool allowUnsafe)
      : _diag(diag), allowUnsafe_(allowUnsafe), hadError_(false) {}

  std::unique_ptr<BoundRootNode> Binder::bind(RootNode &root)
  {
    (void)root;
    _diag.report(SourceSpan(), zap::DiagnosticLevel::Error,
                 "Internal error: single-file binder entry point is unsupported.");
    return nullptr;
  }

  std::unique_ptr<BoundRootNode> Binder::bind(std::vector<ModuleInfo> &modules)
  {
    hadError_ = false;
    boundRoot_ = std::make_unique<BoundRootNode>();
    modules_.clear();
    currentScope_.reset();
    currentFunction_.reset();
    currentModuleId_.clear();
    unsafeDepth_ = 0;
    unsafeTypeContextDepth_ = 0;
    externTypeContextDepth_ = 0;

    initializeBuiltins();

    for (auto &module : modules)
    {
      ModuleState state;
      state.info = &module;
      state.scope = std::make_shared<SymbolTable>(builtinScope_);
      state.symbol = std::make_shared<ModuleSymbol>(module.moduleName, module.moduleId);
      modules_[module.moduleId] = state;
    }

    for (auto &[_, module] : modules_)
    {
      predeclareModuleTypes(module);
    }

    for (auto &[_, module] : modules_)
    {
      applyImports(module, true);
    }

    for (auto &[_, module] : modules_)
    {
      predeclareModuleAliases(module);
    }

    for (auto &[_, module] : modules_)
    {
      applyImports(module, true);
    }

    for (auto &[_, module] : modules_)
    {
      predeclareModuleValues(module);
    }

    for (auto &[_, module] : modules_)
    {
      applyImports(module, false);
    }

    for (auto &[_, module] : modules_)
    {
      currentModuleId_ = module.info->moduleId;
      currentScope_ = module.scope;
      for (const auto &child : module.info->root->children)
      {
        if (dynamic_cast<RecordDecl *>(child.get()) ||
            dynamic_cast<StructDeclarationNode *>(child.get()) ||
            dynamic_cast<EnumDecl *>(child.get()))
        {
          child->accept(*this);
        }
      }
    }

    for (auto &[_, module] : modules_)
    {
      currentModuleId_ = module.info->moduleId;
      currentScope_ = module.scope;
      for (const auto &child : module.info->root->children)
      {
        if (dynamic_cast<ImportNode *>(child.get()) ||
            dynamic_cast<RecordDecl *>(child.get()) ||
            dynamic_cast<StructDeclarationNode *>(child.get()) ||
            dynamic_cast<EnumDecl *>(child.get()) ||
            dynamic_cast<TypeAliasDecl *>(child.get()))
        {
          continue;
        }
        child->accept(*this);
      }
    }

    return (hadError_ || _diag.hadErrors()) ? nullptr : std::move(boundRoot_);
  }

  void Binder::initializeBuiltins()
  {
    builtinScope_ = std::make_shared<SymbolTable>();

    auto declareType = [&](const std::string &name, zir::TypeKind kind) {
      builtinScope_->declare(name, std::make_shared<TypeSymbol>(
                                       name, std::make_shared<zir::PrimitiveType>(kind),
                                       name, "", Visibility::Public));
    };

    declareType("Int", zir::TypeKind::Int);
    declareType("Int8", zir::TypeKind::Int8);
    declareType("Int16", zir::TypeKind::Int16);
    declareType("Int32", zir::TypeKind::Int32);
    declareType("Int64", zir::TypeKind::Int64);
    declareType("UInt", zir::TypeKind::UInt);
    declareType("UInt8", zir::TypeKind::UInt8);
    declareType("UInt16", zir::TypeKind::UInt16);
    declareType("UInt32", zir::TypeKind::UInt32);
    declareType("UInt64", zir::TypeKind::UInt64);
    declareType("Float", zir::TypeKind::Float);
    declareType("Float32", zir::TypeKind::Float32);
    declareType("Float64", zir::TypeKind::Float64);
    declareType("Bool", zir::TypeKind::Bool);
    declareType("Void", zir::TypeKind::Void);
    declareType("Char", zir::TypeKind::Char);

    builtinScope_->declare("String", std::make_shared<TypeSymbol>(
                                         "String",
                                         std::make_shared<zir::RecordType>("String", "String"),
                                         "String", "", Visibility::Public));
  }

  std::string Binder::mangleName(const std::string &modulePath,
                                 const std::string &name) const
  {
    std::string mangled = "zap$";
    for (char c : modulePath)
    {
      if (std::isalnum(static_cast<unsigned char>(c)))
      {
        mangled += c;
      }
      else if (c == '/' || c == '\\' || c == '.' || c == '-')
      {
        mangled += '$';
      }
      else
      {
        mangled += '_';
      }
    }
    if (!mangled.empty() && mangled.back() != '$')
    {
      mangled += '$';
    }
    mangled += name;
    return mangled;
  }

  std::string Binder::displayTypeName(const std::string &moduleName,
                                      const std::string &name) const
  {
    if (moduleName.empty() || moduleName == "__single_module__")
    {
      return name;
    }
    return moduleName + "." + name;
  }

  std::string Binder::currentModuleLinkPath() const
  {
    auto it = modules_.find(currentModuleId_);
    if (it == modules_.end() || !it->second.info)
    {
      return currentModuleId_;
    }
    return it->second.info->linkPath.empty() ? it->second.info->moduleId
                                             : it->second.info->linkPath;
  }

  void Binder::predeclareModuleTypes(ModuleState &module)
  {
    currentModuleId_ = module.info->moduleId;
    currentScope_ = module.scope;

    for (const auto &child : module.info->root->children)
    {
      if (auto recordDecl = dynamic_cast<RecordDecl *>(child.get()))
      {
        auto type = std::make_shared<zir::RecordType>(
            displayTypeName(module.info->moduleName, recordDecl->name_),
            mangleName(module.info->linkPath.empty() ? module.info->moduleId
                                                    : module.info->linkPath,
                       recordDecl->name_));
        auto symbol = std::make_shared<TypeSymbol>(
            recordDecl->name_, type,
            mangleName(module.info->linkPath.empty() ? module.info->moduleId
                                                     : module.info->linkPath,
                       recordDecl->name_),
            module.info->moduleName, recordDecl->visibility_, false);
        if (!module.scope->declare(recordDecl->name_, symbol))
        {
          error(recordDecl->span,
                "Type '" + recordDecl->name_ + "' already declared.");
        }
        module.symbol->members[recordDecl->name_] = symbol;
        if (recordDecl->visibility_ == Visibility::Public)
        {
          module.symbol->exports[recordDecl->name_] = symbol;
        }
      }
      else if (auto structDecl = dynamic_cast<StructDeclarationNode *>(child.get()))
      {
        auto type = std::make_shared<zir::RecordType>(
            displayTypeName(module.info->moduleName, structDecl->name_),
            mangleName(module.info->linkPath.empty() ? module.info->moduleId
                                                    : module.info->linkPath,
                       structDecl->name_));
        auto symbol = std::make_shared<TypeSymbol>(
            structDecl->name_, type,
            mangleName(module.info->linkPath.empty() ? module.info->moduleId
                                                     : module.info->linkPath,
                       structDecl->name_),
            module.info->moduleName, structDecl->visibility_,
            structDecl->isUnsafe_);
        if (!module.scope->declare(structDecl->name_, symbol))
        {
          error(structDecl->span,
                "Type '" + structDecl->name_ + "' already declared.");
        }
        module.symbol->members[structDecl->name_] = symbol;
        if (structDecl->visibility_ == Visibility::Public)
        {
          module.symbol->exports[structDecl->name_] = symbol;
        }
      }
      else if (auto enumDecl = dynamic_cast<EnumDecl *>(child.get()))
      {
        auto type = std::make_shared<zir::EnumType>(
            displayTypeName(module.info->moduleName, enumDecl->name_),
            enumDecl->entries_,
            mangleName(module.info->linkPath.empty() ? module.info->moduleId
                                                     : module.info->linkPath,
                       enumDecl->name_));
        auto symbol = std::make_shared<TypeSymbol>(
            enumDecl->name_, type,
            mangleName(module.info->linkPath.empty() ? module.info->moduleId
                                                     : module.info->linkPath,
                       enumDecl->name_),
            module.info->moduleName, enumDecl->visibility_, false);
        if (!module.scope->declare(enumDecl->name_, symbol))
        {
          error(enumDecl->span,
                "Type '" + enumDecl->name_ + "' already declared.");
        }
        module.symbol->members[enumDecl->name_] = symbol;
        if (enumDecl->visibility_ == Visibility::Public)
        {
          module.symbol->exports[enumDecl->name_] = symbol;
        }
      }
    }
  }

  void Binder::predeclareModuleAliases(ModuleState &module)
  {
    currentModuleId_ = module.info->moduleId;
    currentScope_ = module.scope;

    for (const auto &child : module.info->root->children)
    {
      if (auto aliasDecl = dynamic_cast<TypeAliasDecl *>(child.get()))
      {
        auto type = mapType(*aliasDecl->type_);
        if (!type)
        {
          error(aliasDecl->span,
                "Unknown type: " + aliasDecl->type_->qualifiedName());
          continue;
        }
        auto symbol = std::make_shared<TypeSymbol>(
            aliasDecl->name_, type,
            mangleName(module.info->linkPath.empty() ? module.info->moduleId
                                                     : module.info->linkPath,
                       aliasDecl->name_),
            module.info->moduleName, aliasDecl->visibility_, false);
        if (!module.scope->declare(aliasDecl->name_, symbol))
        {
          error(aliasDecl->span,
                "Type '" + aliasDecl->name_ + "' already declared.");
        }
        module.symbol->members[aliasDecl->name_] = symbol;
        if (aliasDecl->visibility_ == Visibility::Public)
        {
          module.symbol->exports[aliasDecl->name_] = symbol;
        }
      }
    }
  }

  void Binder::applyImports(ModuleState &module, bool allowIncomplete)
  {
    std::map<std::string, std::string> namespaceOwners;

    for (const auto &import : module.info->imports)
    {
      if (!import.moduleAlias.empty() && import.targetModuleIds.size() != 1)
      {
        if (!allowIncomplete)
        {
          error(import.span,
                "Module alias imports are only allowed when the path resolves to a single module.");
        }
        continue;
      }

      for (const auto &targetId : import.targetModuleIds)
      {
        auto targetIt = modules_.find(targetId);
        if (targetIt == modules_.end())
        {
          continue;
        }

        auto &target = targetIt->second;
        auto alias = import.moduleAlias.empty() ? target.info->moduleName
                                                : import.moduleAlias;

        auto existingOwner = namespaceOwners.find(alias);
        if (existingOwner != namespaceOwners.end() &&
            existingOwner->second != targetId)
        {
          if (!allowIncomplete)
          {
            error(import.span,
                  "Import namespace '" + alias +
                      "' is ambiguous because multiple files share that module name.");
          }
          continue;
        }

        if (!module.scope->lookupLocal(alias))
        {
          module.scope->declare(alias, target.symbol);
          namespaceOwners[alias] = targetId;
        }
        else if (module.scope->lookupLocal(alias) != target.symbol)
        {
          if (!allowIncomplete)
          {
            error(import.span,
                  "Cannot import module '" + alias +
                      "' because that name is already declared in the current file.");
          }
          continue;
        }

        if (import.bindings.empty())
        {
          if (import.visibility == Visibility::Public)
          {
            module.symbol->exports[alias] = target.symbol;
          }
          continue;
        }

        if (import.targetModuleIds.size() != 1)
        {
          if (!allowIncomplete)
          {
            error(import.span,
                  "Selective imports are only allowed when the path resolves to a single module.");
          }
          continue;
        }

        for (const auto &binding : import.bindings)
        {
          auto exportedIt = target.symbol->exports.find(binding.sourceName);
          if (exportedIt == target.symbol->exports.end())
          {
            if (allowIncomplete)
            {
              continue;
            }
            auto memberIt = target.symbol->members.find(binding.sourceName);
            if (memberIt != target.symbol->members.end() &&
                memberIt->second->visibility != Visibility::Public)
            {
              error(import.span,
                    "Member '" + binding.sourceName + "' of module '" + alias +
                        "' is private.");
            }
            else
            {
              error(import.span,
                    "Module '" + alias + "' has no public member '" +
                        binding.sourceName + "'.");
            }
            continue;
          }

          auto existing = module.scope->lookupLocal(binding.localName);
          if (existing && existing != exportedIt->second)
          {
            if (!allowIncomplete)
            {
              error(import.span,
                    "Imported name '" + binding.localName +
                        "' conflicts with an existing declaration in the current file.");
            }
            continue;
          }
          if (!existing)
          {
            module.scope->declare(binding.localName, exportedIt->second);
          }
          if (import.visibility == Visibility::Public)
          {
            module.symbol->exports[binding.localName] = exportedIt->second;
          }
        }
      }
    }
  }

  void Binder::predeclareModuleValues(ModuleState &module)
  {
    currentModuleId_ = module.info->moduleId;
    currentScope_ = module.scope;

    for (const auto &child : module.info->root->children)
    {
      if (auto funDecl = dynamic_cast<FunDecl *>(child.get()))
      {
        if (funDecl->isUnsafe_)
        {
          requireUnsafeEnabled(funDecl->span, "'unsafe fun'");
          ++unsafeTypeContextDepth_;
        }

        std::vector<std::shared_ptr<VariableSymbol>> params;
        for (size_t i = 0; i < funDecl->params_.size(); ++i)
        {
          const auto &p = funDecl->params_[i];
          if (p->isVariadic && i + 1 != funDecl->params_.size())
          {
            error(p->span, "Variadic parameter must be the last parameter.");
          }
          if (p->isVariadic && p->isRef)
          {
            error(p->span, "Variadic parameter cannot be passed by 'ref'.");
          }
          auto mappedType = mapType(*p->type);
          if (!mappedType)
          {
            error(p->span, "Unknown type: " + p->type->qualifiedName());
            mappedType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
          }
          auto symbol = std::make_shared<VariableSymbol>(
              p->name, mappedType, false, p->isRef, p->name,
              module.info->moduleName, Visibility::Private);
          if (p->isVariadic)
          {
            symbol->is_variadic_pack = true;
            symbol->variadic_element_type = mappedType;
            symbol->type = std::make_shared<zir::PointerType>(mappedType);
          }
          params.push_back(std::move(symbol));
        }

        std::shared_ptr<zir::Type> retType = nullptr;
        if (funDecl->returnType_)
        {
          retType = mapType(*funDecl->returnType_);
        }
        else if (funDecl->name_ == "main" && module.info->isEntry)
        {
          retType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int);
        }
        else
        {
          retType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
        }

        if (!retType)
        {
          error(funDecl->span, "Unknown return type in function '" + funDecl->name_ + "'.");
          retType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
        }

        if (funDecl->isUnsafe_)
        {
          --unsafeTypeContextDepth_;
        }

        auto linkName =
            (funDecl->name_ == "main" && module.info->isEntry)
                ? std::string("main")
                : mangleName(module.info->linkPath.empty() ? module.info->moduleId
                                                           : module.info->linkPath,
                             funDecl->name_);
        auto symbol = std::make_shared<FunctionSymbol>(
            funDecl->name_, std::move(params), std::move(retType), linkName,
            module.info->moduleName, funDecl->visibility_, funDecl->isUnsafe_);

        if (!module.scope->declare(funDecl->name_, symbol))
        {
          error(funDecl->span,
                "Function '" + funDecl->name_ + "' already declared.");
        }
        module.symbol->members[funDecl->name_] = symbol;
        if (funDecl->visibility_ == Visibility::Public)
        {
          module.symbol->exports[funDecl->name_] = symbol;
        }
      }
      else if (auto extDecl = dynamic_cast<ExtDecl *>(child.get()))
      {
        ++externTypeContextDepth_;
        std::vector<std::shared_ptr<VariableSymbol>> params;
        for (const auto &p : extDecl->params_)
        {
          if (p->isVariadic)
          {
            error(p->span,
                  "Variadic parameters are only supported in Zap function declarations.");
          }
          auto mappedType = mapType(*p->type);
          if (!mappedType)
          {
            error(p->span, "Unknown type: " + p->type->qualifiedName());
            mappedType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
          }
          params.push_back(std::make_shared<VariableSymbol>(
              p->name, mappedType, false, p->isRef, p->name,
              module.info->moduleName, Visibility::Private));
        }

        auto retType =
            extDecl->returnType_
                ? mapType(*extDecl->returnType_)
                : std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
        if (!retType)
        {
          error(extDecl->span, "Unknown return type in external function '" +
                                   extDecl->name_ + "'.");
          retType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
        }
        --externTypeContextDepth_;

        auto linkName = extDecl->name_;
        bool isStdFsModule =
            module.info->moduleName == "fs" &&
            module.info->sourceName.find("/std/fs.zp") != std::string::npos;
        bool isStdIoModule =
            module.info->moduleName == "io" &&
            module.info->sourceName.find("/std/io.zp") != std::string::npos;
        bool isStdPathModule =
            module.info->moduleName == "path" &&
            module.info->sourceName.find("/std/path.zp") != std::string::npos;
        if (isStdFsModule && extDecl->name_ == "mkdir")
        {
          linkName = "zap_fs_mkdir";
        }
        else if (isStdIoModule && extDecl->name_ == "printf")
        {
          linkName = "zap_printf";
        }
        else if (isStdIoModule && extDecl->name_ == "printfln")
        {
          linkName = "zap_printfln";
        }
        else if (isStdPathModule && extDecl->name_ == "basename")
        {
          linkName = "zap_path_basename";
        }

        auto symbol = std::make_shared<FunctionSymbol>(
            extDecl->name_, std::move(params), std::move(retType), linkName,
            module.info->moduleName, extDecl->visibility_, false,
            extDecl->isCVariadic_);

        if (!module.scope->declare(extDecl->name_, symbol))
        {
          error(extDecl->span,
                "External function '" + extDecl->name_ + "' already declared.");
        }
        module.symbol->members[extDecl->name_] = symbol;
        if (extDecl->visibility_ == Visibility::Public)
        {
          module.symbol->exports[extDecl->name_] = symbol;
        }
      }
      else if (auto varDecl = dynamic_cast<VarDecl *>(child.get()))
      {
        if (!varDecl->isGlobal_)
        {
          continue;
        }
        auto type = mapType(*varDecl->type_);
        if (!type)
        {
          error(varDecl->span, "Unknown type: " + varDecl->type_->qualifiedName());
          type = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
        }
        auto symbol = std::make_shared<VariableSymbol>(
            varDecl->name_, type, false, false,
            mangleName(module.info->linkPath.empty() ? module.info->moduleId
                                                     : module.info->linkPath,
                       varDecl->name_),
            module.info->moduleName, varDecl->visibility_);
        if (!module.scope->declare(varDecl->name_, symbol))
        {
          error(varDecl->span, "Variable '" + varDecl->name_ + "' already declared.");
        }
        module.symbol->members[varDecl->name_] = symbol;
        if (varDecl->visibility_ == Visibility::Public)
        {
          module.symbol->exports[varDecl->name_] = symbol;
        }
      }
      else if (auto constDecl = dynamic_cast<ConstDecl *>(child.get()))
      {
        auto type = mapType(*constDecl->type_);
        if (!type)
        {
          error(constDecl->span, "Unknown type: " + constDecl->type_->qualifiedName());
          type = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
        }
        auto symbol = std::make_shared<VariableSymbol>(
            constDecl->name_, type, true, false,
            mangleName(module.info->linkPath.empty() ? module.info->moduleId
                                                     : module.info->linkPath,
                       constDecl->name_),
            module.info->moduleName, constDecl->visibility_);
        if (!module.scope->declare(constDecl->name_, symbol))
        {
          error(constDecl->span, "Identifier '" + constDecl->name_ + "' already declared.");
        }
        module.symbol->members[constDecl->name_] = symbol;
        if (constDecl->visibility_ == Visibility::Public)
        {
          module.symbol->exports[constDecl->name_] = symbol;
        }
      }
    }
  }

  std::shared_ptr<Symbol> Binder::lookupVisibleSymbol(const std::string &name) const
  {
    return currentScope_ ? currentScope_->lookup(name) : nullptr;
  }

  std::shared_ptr<Symbol> Binder::resolveModuleMember(const std::string &moduleName,
                                                      const std::string &memberName,
                                                      SourceSpan span)
  {
    auto moduleSym = std::dynamic_pointer_cast<ModuleSymbol>(currentScope_->lookup(moduleName));
    if (!moduleSym)
    {
      error(span, "Undefined module: " + moduleName);
      return nullptr;
    }

    auto memberIt = moduleSym->members.find(memberName);
    if (memberIt == moduleSym->members.end())
    {
      error(span, "Module '" + moduleName + "' has no member '" + memberName + "'.");
      return nullptr;
    }

    if (memberIt->second->visibility != Visibility::Public)
    {
      error(span, "Member '" + memberName + "' of module '" + moduleName +
                      "' is private.");
      return nullptr;
    }

    return memberIt->second;
  }

  std::shared_ptr<Symbol>
  Binder::resolveQualifiedSymbol(const std::vector<std::string> &parts,
                                 SourceSpan span, SymbolKind expectedKind,
                                 bool allowAnyKind)
  {
    if (parts.empty())
    {
      return nullptr;
    }

    if (parts.size() == 1)
    {
      auto symbol = lookupVisibleSymbol(parts.front());
      if (!symbol)
      {
        error(span, "Undefined identifier: " + parts.front());
        return nullptr;
      }
      if (!allowAnyKind && symbol->getKind() != expectedKind)
      {
        return nullptr;
      }
      return symbol;
    }

    if (parts.size() != 2)
    {
      error(span, "Only single-level module qualification is supported.");
      return nullptr;
    }

    auto symbol = resolveModuleMember(parts[0], parts[1], span);
    if (!symbol)
    {
      return nullptr;
    }
    if (!allowAnyKind && symbol->getKind() != expectedKind)
    {
      return nullptr;
    }
    return symbol;
  }

  void Binder::visit(RootNode &node)
  {
    for (const auto &child : node.children)
    {
      if (dynamic_cast<ImportNode *>(child.get()))
      {
        continue;
      }
      child->accept(*this);
    }
  }

  void Binder::visit(ImportNode &node) { (void)node; }

  void Binder::visit(FunDecl &node)
  {
    auto symbol = std::dynamic_pointer_cast<FunctionSymbol>(
        currentScope_->lookup(node.name_));
    if (!symbol)
    {
      error(node.span,
            "Internal error: Function symbol not found for " + node.name_);
      return;
    }

    pushScope();
    auto oldFunction = currentFunction_;
    currentFunction_ = symbol;
    int oldUnsafeDepth = unsafeDepth_;
    if (node.isUnsafe_)
    {
      requireUnsafeEnabled(node.span, "'unsafe fun'");
      ++unsafeDepth_;
    }

    for (const auto &param : symbol->parameters)
    {
      if (!currentScope_->declare(param->name, param))
      {
        error(node.span, "Parameter '" + param->name + "' already declared.");
      }
    }

    auto boundBody = bindBody(node.body_.get(), false);

    popScope();
    currentFunction_ = oldFunction;
    unsafeDepth_ = oldUnsafeDepth;

    bool hasReturn = false;
    if (boundBody)
    {
      if (boundBody->result)
        hasReturn = true;
      for (const auto &stmt : boundBody->statements)
      {
        if (dynamic_cast<BoundReturnStatement *>(stmt.get()))
        {
          hasReturn = true;
          break;
        }
      }
    }

    if (!hasReturn && symbol->linkName == "main" &&
        symbol->returnType->isInteger())
    {
      auto intType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int);
      auto lit = std::make_unique<BoundLiteral>("0", intType);
      boundBody->statements.push_back(
          std::make_unique<BoundReturnStatement>(std::move(lit)));
      hasReturn = true;
    }

    if (!hasReturn && symbol->returnType->getKind() != zir::TypeKind::Void)
    {
      auto kind = symbol->returnType->getKind();
      if (symbol->returnType->isInteger() || kind == zir::TypeKind::Float ||
          kind == zir::TypeKind::Bool)
      {
        std::string litVal = "0";
        if (kind == zir::TypeKind::Float)
          litVal = "0.0";
        else if (kind == zir::TypeKind::Bool)
          litVal = "false";
        auto lit = std::make_unique<BoundLiteral>(litVal, symbol->returnType);
        boundBody->statements.push_back(
            std::make_unique<BoundReturnStatement>(std::move(lit)));
      }

      _diag.report(node.span, zap::DiagnosticLevel::Warning,
                   "Function '" + node.name_ +
                       "' has non-void return type but no return on some paths.");
    }

    boundRoot_->functions.push_back(
        std::make_unique<BoundFunctionDeclaration>(symbol, std::move(boundBody)));
  }

  void Binder::visit(ExtDecl &node)
  {
    auto symbol = std::dynamic_pointer_cast<FunctionSymbol>(
        currentScope_->lookup(node.name_));
    if (!symbol)
    {
      error(node.span,
            "Internal error: External function symbol not found for " +
                node.name_);
      return;
    }

    for (const auto &existing : boundRoot_->externalFunctions)
    {
      if (existing->symbol->linkName == symbol->linkName)
      {
        return;
      }
    }

    boundRoot_->externalFunctions.push_back(
        std::make_unique<BoundExternalFunctionDeclaration>(symbol));
  }

  std::unique_ptr<BoundBlock> Binder::bindBody(BodyNode *body, bool createScope)
  {
    auto savedBlock = std::move(currentBlock_);
    if (createScope)
    {
      pushScope();
    }

    if (body)
    {
      body->accept(*this);
    }

    auto boundBody = std::make_unique<BoundBlock>();
    if (currentBlock_)
    {
      boundBody = std::move(currentBlock_);
    }

    if (createScope)
    {
      popScope();
    }

    currentBlock_ = std::move(savedBlock);
    return boundBody;
  }

  void Binder::visit(BodyNode &node)
  {
    currentBlock_ = std::make_unique<BoundBlock>();

    for (const auto &stmt : node.statements)
    {
      stmt->accept(*this);
      if (!statementStack_.empty())
      {
        currentBlock_->statements.push_back(std::move(statementStack_.top()));
        statementStack_.pop();
      }
      else if (!expressionStack_.empty())
      {
        auto expr = std::move(expressionStack_.top());
        expressionStack_.pop();
        currentBlock_->statements.push_back(
            std::make_unique<BoundExpressionStatement>(std::move(expr)));
      }
    }

    if (node.result)
    {
      node.result->accept(*this);
      if (!expressionStack_.empty())
      {
        currentBlock_->result = std::move(expressionStack_.top());
        expressionStack_.pop();
      }
    }
  }

  void Binder::visit(UnsafeBlockNode &node)
  {
    requireUnsafeEnabled(node.span, "'unsafe' block");
    int oldUnsafeDepth = unsafeDepth_;
    ++unsafeDepth_;

    auto savedBlock = std::move(currentBlock_);
    pushScope();
    visit(static_cast<BodyNode &>(node));

    auto boundBody = std::make_unique<BoundBlock>();
    if (currentBlock_)
    {
      boundBody = std::move(currentBlock_);
    }

    popScope();
    currentBlock_ = std::move(savedBlock);
    unsafeDepth_ = oldUnsafeDepth;
    statementStack_.push(std::move(boundBody));
  }

  void Binder::visit(VarDecl &node)
  {
    auto existing = currentScope_->lookupLocal(node.name_);
    std::shared_ptr<VariableSymbol> symbol;
    if (existing)
    {
      symbol = std::dynamic_pointer_cast<VariableSymbol>(existing);
    }

    auto type = mapType(*node.type_);
    if (!type)
    {
      error(node.span, "Unknown type: " + node.type_->qualifiedName());
      type = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
    }

    std::unique_ptr<BoundExpression> initializer = nullptr;
    if (node.initializer_)
    {
      node.initializer_->accept(*this);
      if (!expressionStack_.empty())
      {
        initializer = std::move(expressionStack_.top());
        expressionStack_.pop();

        if (!canConvert(initializer->type, type))
        {
          error(node.initializer_->span,
                "Cannot assign expression of type '" +
                    initializer->type->toString() +
                    "' to variable of type '" + type->toString() + "'");
        }
        else
        {
          initializer = wrapInCast(std::move(initializer), type);
        }
      }
    }

    if (!symbol)
    {
      symbol = std::make_shared<VariableSymbol>(
          node.name_, type, false, false,
          node.isGlobal_ ? mangleName(currentModuleLinkPath(), node.name_)
                         : node.name_,
          modules_[currentModuleId_].info->moduleName, node.visibility_);
      if (!currentScope_->declare(node.name_, symbol))
      {
        error(node.span, "Variable '" + node.name_ + "' already declared.");
      }
    }

    auto boundDecl = std::make_unique<BoundVariableDeclaration>(
        symbol, std::move(initializer));

    if (currentBlock_ && !node.isGlobal_)
    {
      statementStack_.push(std::move(boundDecl));
    }
    else
    {
      boundRoot_->globals.push_back(std::move(boundDecl));
    }
  }

  void Binder::visit(ConstDecl &node)
  {
    auto existing = currentScope_->lookupLocal(node.name_);
    std::shared_ptr<VariableSymbol> symbol;
    if (existing)
    {
      symbol = std::dynamic_pointer_cast<VariableSymbol>(existing);
    }

    auto type = mapType(*node.type_);
    if (!type)
    {
      error(node.span, "Unknown type: " + node.type_->qualifiedName());
      type = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
    }

    std::unique_ptr<BoundExpression> initializer = nullptr;
    if (node.initializer_)
    {
      node.initializer_->accept(*this);
      if (!expressionStack_.empty())
      {
        initializer = std::move(expressionStack_.top());
        expressionStack_.pop();

        if (!canConvert(initializer->type, type))
        {
          error(node.initializer_->span,
                "Cannot assign expression of type '" +
                    initializer->type->toString() +
                    "' to constant of type '" + type->toString() + "'");
        }
        else
        {
          initializer = wrapInCast(std::move(initializer), type);
        }
      }
    }
    else
    {
      error(node.span, "Constant '" + node.name_ + "' must be initialized.");
    }

    if (!symbol)
    {
      symbol = std::make_shared<VariableSymbol>(
          node.name_, type, true, false,
          mangleName(currentModuleLinkPath(), node.name_),
          modules_[currentModuleId_].info->moduleName, node.visibility_);
      if (!currentScope_->declare(node.name_, symbol))
      {
        error(node.span, "Identifier '" + node.name_ + "' already declared.");
      }
    }

    if (initializer)
    {
      symbol->constant_value =
          std::shared_ptr<BoundExpression>(initializer->clone());
    }

    auto boundDecl = std::make_unique<BoundVariableDeclaration>(
        symbol, std::move(initializer));

    if (currentBlock_)
    {
      statementStack_.push(std::move(boundDecl));
    }
    else
    {
      boundRoot_->globals.push_back(std::move(boundDecl));
    }
  }

  void Binder::visit(ReturnNode &node)
  {
    std::unique_ptr<BoundExpression> expr = nullptr;
    if (node.returnValue)
    {
      node.returnValue->accept(*this);
      if (!expressionStack_.empty())
      {
        expr = std::move(expressionStack_.top());
        expressionStack_.pop();
      }
    }

    if (currentFunction_)
    {
      auto expectedType = currentFunction_->returnType;
      auto actualType =
          expr ? expr->type
               : std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
      if (!canConvert(actualType, expectedType))
      {
        error(node.span, "Function '" + currentFunction_->name +
                             "' expects return type '" +
                             expectedType->toString() + "', but received '" +
                             actualType->toString() + "'");
      }
      else if (expr)
      {
        expr = wrapInCast(std::move(expr), expectedType);
      }
    }

    statementStack_.push(
        std::make_unique<BoundReturnStatement>(std::move(expr)));
  }

  void Binder::visit(BinExpr &node)
  {
    node.left_->accept(*this);
    if (expressionStack_.empty())
      return;
    auto left = std::move(expressionStack_.top());
    expressionStack_.pop();

    node.right_->accept(*this);
    if (expressionStack_.empty())
      return;
    auto right = std::move(expressionStack_.top());
    expressionStack_.pop();

    auto leftType = left->type;
    auto rightType = right->type;
    std::shared_ptr<zir::Type> resultType = leftType;

    if (node.op_ == "~")
    {
      bool leftOk = isStringType(leftType) ||
                    leftType->getKind() == zir::TypeKind::Char;
      bool rightOk = isStringType(rightType) ||
                     rightType->getKind() == zir::TypeKind::Char;
      bool hasString = isStringType(leftType) || isStringType(rightType);

      if (!leftOk || !rightOk || !hasString)
      {
        error(SourceSpan::merge(node.left_->span, node.right_->span),
              "Concatenation requires String and/or Char operands with at least one String, got '" +
                  leftType->toString() + "' and '" +
                  rightType->toString() + "'");
      }
      resultType = std::make_shared<zir::RecordType>("String", "String");
    }
    else if ((node.op_ == "+" || node.op_ == "-") &&
             (isPointerType(leftType) || isPointerType(rightType)))
    {
      requireUnsafeEnabled(node.span, "pointer arithmetic");
      requireUnsafeContext(node.span, "pointer arithmetic");

      if (node.op_ == "+" &&
          isPointerType(leftType) && rightType->isInteger())
      {
        resultType = leftType;
      }
      else if (node.op_ == "+" &&
               leftType->isInteger() && isPointerType(rightType))
      {
        std::swap(left, right);
        std::swap(leftType, rightType);
        resultType = leftType;
      }
      else if (node.op_ == "-" &&
               isPointerType(leftType) && rightType->isInteger())
      {
        resultType = leftType;
      }
      else if (node.op_ == "-" &&
               isPointerType(leftType) && isPointerType(rightType))
      {
        if (leftType->toString() != rightType->toString())
        {
          error(SourceSpan::merge(node.left_->span, node.right_->span),
                "Pointer subtraction requires operands of the same type.");
        }
        resultType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int);
      }
      else
      {
        error(SourceSpan::merge(node.left_->span, node.right_->span),
              "Invalid pointer arithmetic between '" + leftType->toString() +
                  "' and '" + rightType->toString() + "'");
      }
    }
    else if (node.op_ == "+" || node.op_ == "-" || node.op_ == "*" ||
             node.op_ == "/" || node.op_ == "%" || node.op_ == "^")
    {
      if (!isNumeric(leftType) || !isNumeric(rightType))
      {
        error(SourceSpan::merge(node.left_->span, node.right_->span),
              "Operator '" + node.op_ + "' cannot be applied to '" +
                  leftType->toString() + "' and '" +
                  rightType->toString() + "'");
      }
      else
      {
        resultType = getPromotedType(leftType, rightType);
        left = wrapInCast(std::move(left), resultType);
        right = wrapInCast(std::move(right), resultType);
      }
    }
    else if (node.op_ == "==" || node.op_ == "!=" || node.op_ == "<" ||
             node.op_ == "<=" || node.op_ == ">" || node.op_ == ">=")
    {
      if ((isPointerType(leftType) || isPointerType(rightType) ||
           isNullType(leftType) || isNullType(rightType)))
      {
        requireUnsafeEnabled(node.span, "pointer comparisons");
        requireUnsafeContext(node.span, "pointer comparisons");
      }
      
      // Reject comparisons of struct types
      if (leftType->getKind() == zir::TypeKind::Record ||
          rightType->getKind() == zir::TypeKind::Record)
      {
        error(SourceSpan::merge(node.left_->span, node.right_->span),
              "Cannot compare struct types '" + leftType->toString() + "' and '" +
                  rightType->toString() + "'");
      }
      
      if (!canConvert(leftType, rightType) && !canConvert(rightType, leftType))
      {
        error(SourceSpan::merge(node.left_->span, node.right_->span),
              "Cannot compare '" + leftType->toString() + "' and '" +
                  rightType->toString() + "'");
      }
      else if (isNullType(leftType) && isPointerType(rightType))
      {
        left = wrapInCast(std::move(left), rightType);
      }
      else if (isNullType(rightType) && isPointerType(leftType))
      {
        right = wrapInCast(std::move(right), leftType);
      }
      resultType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Bool);
    }
    else if (node.op_ == "&&" || node.op_ == "||")
    {
      if (leftType->getKind() != zir::TypeKind::Bool ||
          rightType->getKind() != zir::TypeKind::Bool)
      {
        error(SourceSpan::merge(node.left_->span, node.right_->span),
              "Logical operator '" + node.op_ +
                  "' requires Bool operands.");
      }
      resultType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Bool);
    }

    expressionStack_.push(std::make_unique<BoundBinaryExpression>(
        std::move(left), node.op_, std::move(right), resultType));
  }

  void Binder::visit(TernaryExpr &node)
  {
    node.condition_->accept(*this);
    if (expressionStack_.empty())
      return;
    auto condition = std::move(expressionStack_.top());
    expressionStack_.pop();

    if (condition->type->getKind() != zir::TypeKind::Bool)
    {
      error(node.condition_->span, "Ternary condition must be Bool, got '" +
                                       condition->type->toString() + "'");
    }

    node.thenExpr_->accept(*this);
    if (expressionStack_.empty())
      return;
    auto thenExpr = std::move(expressionStack_.top());
    expressionStack_.pop();

    node.elseExpr_->accept(*this);
    if (expressionStack_.empty())
      return;
    auto elseExpr = std::move(expressionStack_.top());
    expressionStack_.pop();

    if (!canConvert(thenExpr->type, elseExpr->type) &&
        !canConvert(elseExpr->type, thenExpr->type))
    {
      error(SourceSpan::merge(node.thenExpr_->span, node.elseExpr_->span),
            "Ternary branches must be compatible, got '" +
                thenExpr->type->toString() + "' and '" +
                elseExpr->type->toString() + "'");
    }

    auto resultType = canConvert(thenExpr->type, elseExpr->type)
                          ? elseExpr->type
                          : thenExpr->type;
    thenExpr = wrapInCast(std::move(thenExpr), resultType);
    elseExpr = wrapInCast(std::move(elseExpr), resultType);

    expressionStack_.push(std::make_unique<BoundTernaryExpression>(
        std::move(condition), std::move(thenExpr), std::move(elseExpr),
        resultType));
  }

  void Binder::visit(ConstInt &node)
  {
    expressionStack_.push(std::make_unique<BoundLiteral>(
        std::to_string(node.value_),
        std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int)));
  }

  void Binder::visit(ConstFloat &node)
  {
    expressionStack_.push(std::make_unique<BoundLiteral>(
        std::to_string(node.value_),
        std::make_shared<zir::PrimitiveType>(zir::TypeKind::Float)));
  }

  void Binder::visit(ConstString &node)
  {
    expressionStack_.push(std::make_unique<BoundLiteral>(
        node.value_, std::make_shared<zir::RecordType>("String", "String")));
  }

  void Binder::visit(ConstChar &node)
  {
    expressionStack_.push(std::make_unique<BoundLiteral>(
        node.value_, std::make_shared<zir::PrimitiveType>(zir::TypeKind::Char)));
  }

  void Binder::visit(ConstNull &node)
  {
    requireUnsafeEnabled(node.span, "'null'");
    requireUnsafeContext(node.span, "'null'");
    expressionStack_.push(std::make_unique<BoundLiteral>(
        "0", std::make_shared<zir::PrimitiveType>(zir::TypeKind::NullPtr)));
  }

  void Binder::visit(CastExpr &node)
  {
    node.expr_->accept(*this);
    if (expressionStack_.empty())
      return;

    auto expr = std::move(expressionStack_.top());
    expressionStack_.pop();

    auto targetType = mapType(*node.type_);
    if (!targetType)
    {
      error(node.type_->span, "Unknown type: " + node.type_->qualifiedName());
      return;
    }

    requireUnsafeEnabled(node.span, "explicit casts");
    requireUnsafeContext(node.span, "explicit casts");

    bool castAllowed = false;
    if (isNumeric(expr->type) && isNumeric(targetType))
      castAllowed = true;
    else if ((isPointerType(expr->type) || isNullType(expr->type)) &&
             isPointerType(targetType))
      castAllowed = true;
    else if (isStringType(expr->type) && isPointerType(targetType))
      castAllowed = true;
    else if (isPointerType(expr->type) && targetType->isInteger())
      castAllowed = true;
    else if (expr->type->isInteger() && isPointerType(targetType))
      castAllowed = true;

    if (!castAllowed)
    {
      error(node.span, "Cannot cast from '" + expr->type->toString() +
                           "' to '" + targetType->toString() + "'");
      return;
    }

    expressionStack_.push(std::make_unique<BoundCast>(std::move(expr), targetType));
  }

  void Binder::visit(ConstId &node)
  {
    auto symbol = currentScope_->lookup(node.value_);
    if (!symbol)
    {
      error(node.span, "Undefined identifier: " + node.value_);
      return;
    }

    if (auto varSymbol = std::dynamic_pointer_cast<VariableSymbol>(symbol))
    {
      expressionStack_.push(std::make_unique<BoundVariableExpression>(varSymbol));
    }
    else if (auto typeSymbol = std::dynamic_pointer_cast<TypeSymbol>(symbol))
    {
      expressionStack_.push(std::make_unique<BoundLiteral>("", typeSymbol->type));
    }
    else if (auto moduleSymbol = std::dynamic_pointer_cast<ModuleSymbol>(symbol))
    {
      expressionStack_.push(std::make_unique<BoundModuleReference>(moduleSymbol));
    }
    else
    {
      error(node.span, "'" + node.value_ + "' is not a variable or type.");
    }
  }

  void Binder::visit(AssignNode &node)
  {
    node.target_->accept(*this);
    if (expressionStack_.empty())
      return;
    auto target = std::move(expressionStack_.top());
    expressionStack_.pop();

    bool isLValue = dynamic_cast<BoundVariableExpression *>(target.get()) ||
                    dynamic_cast<BoundIndexAccess *>(target.get()) ||
                    dynamic_cast<BoundMemberAccess *>(target.get()) ||
                    (dynamic_cast<BoundUnaryExpression *>(target.get()) &&
                     static_cast<BoundUnaryExpression *>(target.get())->op == "*");

    if (!isLValue)
    {
      error(node.span, "Target of assignment must be an l-value.");
      return;
    }

    if (auto varExpr = dynamic_cast<BoundVariableExpression *>(target.get()))
    {
      if (varExpr->symbol->is_const)
      {
        error(node.span,
              "Cannot assign to constant '" + varExpr->symbol->name + "'.");
        return;
      }
    }
    else if (auto indexExpr = dynamic_cast<BoundIndexAccess *>(target.get()))
    {
      if (isStringType(indexExpr->left->type))
      {
        error(node.span, "Cannot assign through String index access.");
        return;
      }
    }

    node.expr_->accept(*this);
    if (expressionStack_.empty())
      return;
    auto expr = std::move(expressionStack_.top());
    expressionStack_.pop();

    if (!canConvert(expr->type, target->type))
    {
      error(node.span, "Cannot assign expression of type '" +
                           expr->type->toString() + "' to type '" +
                           target->type->toString() + "'");
    }
    else
    {
      expr = wrapInCast(std::move(expr), target->type);
    }

    statementStack_.push(
        std::make_unique<BoundAssignment>(std::move(target), std::move(expr)));
  }

  void Binder::visit(IndexAccessNode &node)
  {
    node.left_->accept(*this);
    if (expressionStack_.empty())
      return;
    auto left = std::move(expressionStack_.top());
    expressionStack_.pop();

    if (left->type->getKind() != zir::TypeKind::Array &&
        !isStringType(left->type))
    {
      error(node.span,
            "Type '" + left->type->toString() + "' does not support indexing.");
      return;
    }

    node.index_->accept(*this);
    if (expressionStack_.empty())
      return;
    auto index = std::move(expressionStack_.top());
    expressionStack_.pop();

    if (!index->type->isInteger())
    {
      error(node.span, "Array index must be an integer, but got '" +
                           index->type->toString() + "'");
    }

    std::shared_ptr<zir::Type> elementType;
    if (left->type->getKind() == zir::TypeKind::Array)
    {
      auto arrayType = std::static_pointer_cast<zir::ArrayType>(left->type);
      elementType = arrayType->getBaseType();
    }
    else
    {
      elementType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Char);
    }

    expressionStack_.push(std::make_unique<BoundIndexAccess>(
        std::move(left), std::move(index), elementType));
  }

  void Binder::visit(MemberAccessNode &node)
  {
    node.left_->accept(*this);
    if (expressionStack_.empty())
      return;

    auto left = std::move(expressionStack_.top());
    expressionStack_.pop();

    if (auto moduleRef = dynamic_cast<BoundModuleReference *>(left.get()))
    {
      auto memberIt = moduleRef->symbol->members.find(node.member_);
      if (memberIt == moduleRef->symbol->members.end())
      {
        error(node.span, "Module '" + moduleRef->symbol->name +
                             "' has no member '" + node.member_ + "'");
        return;
      }
      if (memberIt->second->visibility != Visibility::Public)
      {
        error(node.span, "Member '" + node.member_ + "' of module '" +
                             moduleRef->symbol->name + "' is private.");
        return;
      }

      if (auto varSymbol =
              std::dynamic_pointer_cast<VariableSymbol>(memberIt->second))
      {
        expressionStack_.push(
            std::make_unique<BoundVariableExpression>(varSymbol));
        return;
      }
      if (auto typeSymbol = std::dynamic_pointer_cast<TypeSymbol>(memberIt->second))
      {
        expressionStack_.push(
            std::make_unique<BoundLiteral>("", typeSymbol->type));
        return;
      }
      if (auto nestedModule =
              std::dynamic_pointer_cast<ModuleSymbol>(memberIt->second))
      {
        expressionStack_.push(
            std::make_unique<BoundModuleReference>(nestedModule));
        return;
      }

      error(node.span, "'" + node.member_ + "' is not a value or type.");
      return;
    }

    if (left->type->getKind() == zir::TypeKind::Enum)
    {
      auto enumType = std::static_pointer_cast<zir::EnumType>(left->type);
      int value = enumType->getVariantIndex(node.member_);
      if (value != -1)
      {
        expressionStack_.push(std::make_unique<BoundLiteral>(
            std::to_string(value), enumType));
        return;
      }
    }
    else if (left->type->getKind() == zir::TypeKind::Record)
    {
      auto recordType = std::static_pointer_cast<zir::RecordType>(left->type);
      for (const auto &field : recordType->getFields())
      {
        if (field.name == node.member_)
        {
          expressionStack_.push(std::make_unique<BoundMemberAccess>(
              std::move(left), node.member_, field.type));
          return;
        }
      }
    }

    error(node.span, "Member '" + node.member_ + "' not found in type '" +
                         left->type->toString() + "'");
  }

  void Binder::visit(FunCall &node)
  {
    std::vector<std::string> calleeParts;
    if (!node.callee_ || !extractQualifiedPath(node.callee_.get(), calleeParts))
    {
      error(node.span, "Only direct function calls are supported.");
      return;
    }

    auto symbol =
        resolveQualifiedSymbol(calleeParts, node.span, SymbolKind::Function);
    if (!symbol)
    {
      return;
    }

    auto funcSymbol = std::dynamic_pointer_cast<FunctionSymbol>(symbol);
    if (!funcSymbol)
    {
      error(node.span, "'" + calleeParts.back() + "' is not a function.");
      return;
    }

    if (funcSymbol->isUnsafe)
    {
      requireUnsafeEnabled(node.span, "unsafe function calls");
      requireUnsafeContext(node.span, "unsafe function calls");
    }

    size_t fixedParamCount = funcSymbol->fixedParameterCount();
    if (!funcSymbol->acceptsExtraArguments() &&
        node.params_.size() != funcSymbol->parameters.size())
    {
      error(node.callee_->span, "Function '" + funcSymbol->name + "' expects " +
                                    std::to_string(funcSymbol->parameters.size()) +
                                    " arguments, but received " +
                                    std::to_string(node.params_.size()));
    }
    if (funcSymbol->acceptsExtraArguments() && node.params_.size() < fixedParamCount)
    {
      error(node.callee_->span, "Function '" + funcSymbol->name + "' expects at least " +
                                    std::to_string(fixedParamCount) +
                                    " arguments, but received " +
                                    std::to_string(node.params_.size()));
    }

    std::vector<std::unique_ptr<BoundExpression>> boundArgs;
    std::vector<bool> argIsRefList;
    std::unique_ptr<BoundExpression> variadicPack = nullptr;
    bool seenSpreadArg = false;
    auto variadicParam = funcSymbol->variadicParameter();

    for (size_t i = 0; i < node.params_.size(); ++i)
    {
      if (seenSpreadArg)
      {
        error(node.params_[i]->value->span,
              "Spread argument must be the last argument in a function call.");
        return;
      }

      node.params_[i]->value->accept(*this);
      if (expressionStack_.empty())
        return;

      auto arg = std::move(expressionStack_.top());
      expressionStack_.pop();
      bool argIsRef = node.params_[i]->isRef;
      bool argIsSpread = node.params_[i]->isSpread;

      if (argIsSpread)
      {
        if (argIsRef)
        {
          error(node.params_[i]->value->span,
                "Spread arguments cannot be passed by 'ref'.");
        }
        if (!funcSymbol->hasVariadicParameter())
        {
          error(node.params_[i]->value->span,
                "Spread arguments can only be used when calling a variadic function.");
        }

        auto *varExpr = dynamic_cast<BoundVariableExpression *>(arg.get());
        if (!varExpr || !varExpr->symbol->is_variadic_pack)
        {
          error(node.params_[i]->value->span,
                "Spread arguments must reference a variadic parameter.");
        }
        else if (!variadicParam ||
                 varExpr->symbol->variadic_element_type->toString() !=
                     variadicParam->variadic_element_type->toString())
        {
          error(node.params_[i]->value->span,
                "Spread argument element type does not match the variadic parameter type.");
        }

        variadicPack = std::move(arg);
        seenSpreadArg = true;
        continue;
      }

      argIsRefList.push_back(argIsRef);
      if (i < fixedParamCount)
      {
        auto expectedType = funcSymbol->parameters[i]->type;
        if (argIsRef != funcSymbol->parameters[i]->is_ref)
        {
          error(node.params_[i]->value->span,
                "Argument " + std::to_string(i + 1) +
                    " ref-ness does not match parameter.");
        }

        if (argIsRef)
        {
          auto varExpr = dynamic_cast<BoundVariableExpression *>(arg.get());
          if (!varExpr)
          {
            error(node.params_[i]->value->span,
                  "Arguments passed by 'ref' must be variables.");
          }
          else if (arg->type->toString() != expectedType->toString())
          {
            error(node.params_[i]->value->span,
                  "Argument " + std::to_string(i + 1) +
                      " passed by 'ref' must match the parameter type exactly. Expected '" +
                      expectedType->toString() + "', but got '" +
                      arg->type->toString() + "'.");
          }
        }
        else if (!canConvert(arg->type, expectedType))
        {
          error(node.params_[i]->value->span,
                "Argument " + std::to_string(i + 1) + " expects type '" +
                    expectedType->toString() + "', but received type '" +
                    arg->type->toString() + "'");
        }
        else
        {
          arg = wrapInCast(std::move(arg), expectedType);
        }
      }
      else if (funcSymbol->hasVariadicParameter())
      {
        if (argIsRef)
        {
          error(node.params_[i]->value->span,
                "Variadic arguments cannot be passed by 'ref'.");
        }
        auto expectedType = variadicParam->variadic_element_type;
        if (!canConvert(arg->type, expectedType))
        {
          error(node.params_[i]->value->span,
                "Variadic argument " + std::to_string(i + 1) + " expects type '" +
                    expectedType->toString() + "', but received type '" +
                    arg->type->toString() + "'");
        }
        else
        {
          arg = wrapInCast(std::move(arg), expectedType);
        }
      }
      else if (funcSymbol->isCVariadic)
      {
        if (argIsRef)
        {
          error(node.params_[i]->value->span,
                "C variadic arguments cannot be passed by 'ref'.");
        }
        auto promotedType = getCVariadicArgumentType(arg->type);
        if (!promotedType)
        {
          error(node.params_[i]->value->span,
                "Type '" + arg->type->toString() +
                    "' is not supported in C variadic arguments.");
        }
        else
        {
          arg = wrapInCast(std::move(arg), promotedType);
        }
      }

      boundArgs.push_back(std::move(arg));
    }

    expressionStack_.push(std::make_unique<BoundFunctionCall>(
        funcSymbol, std::move(boundArgs), std::move(argIsRefList),
        std::move(variadicPack)));
  }

  void Binder::visit(IfNode &node)
  {
    node.condition_->accept(*this);
    if (expressionStack_.empty())
      return;

    auto cond = std::move(expressionStack_.top());
    expressionStack_.pop();

    if (cond->type->getKind() != zir::TypeKind::Bool)
    {
      error(node.condition_->span,
            "If condition must be Bool, got '" + cond->type->toString() + "'");
    }

    auto thenBody = bindBody(node.thenBody_.get(), true);

    std::unique_ptr<BoundBlock> elseBody = nullptr;
    if (node.elseBody_)
    {
      elseBody = bindBody(node.elseBody_.get(), true);
    }

    statementStack_.push(std::make_unique<BoundIfStatement>(
        std::move(cond), std::move(thenBody), std::move(elseBody)));
  }

  void Binder::visit(WhileNode &node)
  {
    node.condition_->accept(*this);
    if (expressionStack_.empty())
      return;

    auto cond = std::move(expressionStack_.top());
    expressionStack_.pop();

    if (cond->type->getKind() != zir::TypeKind::Bool)
    {
      error(node.condition_->span, "While condition must be Bool, got '" +
                                        cond->type->toString() + "'");
    }

    ++loopDepth_;
    auto body = bindBody(node.body_.get(), true);
    --loopDepth_;

    statementStack_.push(
        std::make_unique<BoundWhileStatement>(std::move(cond), std::move(body)));
  }

  void Binder::visit(BreakNode &node)
  {
    if (loopDepth_ <= 0)
    {
      error(node.span, "'break' can only be used inside loops.");
      return;
    }
    statementStack_.push(std::make_unique<BoundBreakStatement>());
  }

  void Binder::visit(ContinueNode &node)
  {
    if (loopDepth_ <= 0)
    {
      error(node.span, "'continue' can only be used inside loops.");
      return;
    }
    statementStack_.push(std::make_unique<BoundContinueStatement>());
  }

  void Binder::pushScope()
  {
    currentScope_ = std::make_shared<SymbolTable>(currentScope_);
  }

  void Binder::popScope()
  {
    if (currentScope_)
    {
      currentScope_ = currentScope_->getParent();
    }
  }

  std::optional<int64_t> Binder::evaluateConstantInt(const BoundExpression *expr)
  {
    if (auto lit = dynamic_cast<const BoundLiteral *>(expr))
    {
      try
      {
        return std::stoll(lit->value);
      }
      catch (...)
      {
        return std::nullopt;
      }
    }

    if (auto unary = dynamic_cast<const BoundUnaryExpression *>(expr))
    {
      auto inner = evaluateConstantInt(unary->expr.get());
      if (!inner)
        return std::nullopt;
      if (unary->op == "-")
        return -*inner;
      return inner;
    }

    if (auto binary = dynamic_cast<const BoundBinaryExpression *>(expr))
    {
      auto left = evaluateConstantInt(binary->left.get());
      auto right = evaluateConstantInt(binary->right.get());
      if (!left || !right)
        return std::nullopt;

      if (binary->op == "+")
        return *left + *right;
      if (binary->op == "-")
        return *left - *right;
      if (binary->op == "*")
        return *left * *right;
      if (binary->op == "/")
        return *right == 0 ? std::nullopt : std::optional<int64_t>(*left / *right);
      if (binary->op == "%")
        return *right == 0 ? std::nullopt : std::optional<int64_t>(*left % *right);
    }

    return std::nullopt;
  }

  std::shared_ptr<zir::Type> Binder::mapType(const TypeNode &typeNode)
  {
    if (typeNode.isVarArgs)
    {
      if (!typeNode.baseType)
        return nullptr;
      return mapType(*typeNode.baseType);
    }

    if (typeNode.isArray)
    {
      if (!typeNode.baseType)
        return nullptr;
      auto base = mapType(*typeNode.baseType);
      size_t size = 0;

      if (typeNode.arraySize)
      {
        typeNode.arraySize->accept(*this);
        if (!expressionStack_.empty())
        {
          auto boundSize = std::move(expressionStack_.top());
          expressionStack_.pop();
          auto evaluated = evaluateConstantInt(boundSize.get());
          if (evaluated)
          {
            size = static_cast<size_t>(*evaluated);
          }
          else
          {
            error(typeNode.span,
                  "Array size must be a constant integer expression.");
          }
        }
      }

      return std::make_shared<zir::ArrayType>(std::move(base), size);
    }

    if (typeNode.isPointer)
    {
      requireUnsafeEnabled(typeNode.span, "raw pointer types");
      if (!isUnsafeActive() && externTypeContextDepth_ <= 0)
      {
        error(typeNode.span, "Raw pointer types are only allowed inside unsafe code.");
      }
      if (!typeNode.baseType)
        return nullptr;
      auto base = mapType(*typeNode.baseType);
      return std::make_shared<zir::PointerType>(std::move(base));
    }

    std::vector<std::string> parts = typeNode.qualifiers;
    parts.push_back(typeNode.typeName);
    auto symbol =
        resolveQualifiedSymbol(parts, typeNode.span, SymbolKind::Type);
    if (symbol && symbol->getKind() == SymbolKind::Type)
    {
      auto typeSymbol = std::static_pointer_cast<TypeSymbol>(symbol);
      if (typeSymbol->isUnsafe)
      {
        requireUnsafeEnabled(typeNode.span, "unsafe struct types");
        requireUnsafeContext(typeNode.span, "unsafe struct types");
      }
      return symbol->type;
    }

    return nullptr;
  }

  void Binder::visit(ConstBool &node)
  {
    expressionStack_.push(std::make_unique<BoundLiteral>(
        node.value_ ? "true" : "false",
        std::make_shared<zir::PrimitiveType>(zir::TypeKind::Bool)));
  }

  void Binder::visit(UnaryExpr &node)
  {
    node.expr_->accept(*this);
    if (expressionStack_.empty())
      return;
    auto expr = std::move(expressionStack_.top());
    expressionStack_.pop();

    auto type = expr->type;
    if (node.op_ == "&")
    {
      requireUnsafeEnabled(node.span, "address-of");
      requireUnsafeContext(node.span, "address-of");

      bool isLValue = dynamic_cast<BoundVariableExpression *>(expr.get()) ||
                      dynamic_cast<BoundIndexAccess *>(expr.get()) ||
                      dynamic_cast<BoundMemberAccess *>(expr.get()) ||
                      (dynamic_cast<BoundUnaryExpression *>(expr.get()) &&
                       static_cast<BoundUnaryExpression *>(expr.get())->op == "*");
      if (!isLValue)
      {
        error(node.span, "Cannot take the address of a non-lvalue expression.");
      }

      type = std::make_shared<zir::PointerType>(expr->type);
    }
    else if (node.op_ == "*")
    {
      requireUnsafeEnabled(node.span, "pointer dereference");
      requireUnsafeContext(node.span, "pointer dereference");
      if (!isPointerType(type))
      {
        error(node.span, "Cannot dereference non-pointer type '" +
                             type->toString() + "'");
      }
      else
      {
        type = std::static_pointer_cast<zir::PointerType>(type)->getBaseType();
        if (type->getKind() == zir::TypeKind::Void)
        {
          error(node.span, "Cannot dereference '*Void' directly. Cast it to a concrete pointer type first.");
        }
      }
    }
    else if (node.op_ == "-" || node.op_ == "+")
    {
      if (!isNumeric(type))
      {
        error(node.span, "Operator '" + node.op_ +
                             "' cannot be applied to type '" +
                             type->toString() + "'");
      }
    }
    else if (node.op_ == "!")
    {
      if (type->getKind() != zir::TypeKind::Bool)
      {
        error(node.span, "Operator '!' cannot be applied to type '" +
                             type->toString() + "'");
      }
    }

    expressionStack_.push(
        std::make_unique<BoundUnaryExpression>(node.op_, std::move(expr), type));
  }

  void Binder::visit(ArrayLiteralNode &node)
  {
    std::vector<std::unique_ptr<BoundExpression>> elements;
    std::shared_ptr<zir::Type> elementType = nullptr;

    for (const auto &el : node.elements_)
    {
      el->accept(*this);
      if (!expressionStack_.empty())
      {
        auto boundEl = std::move(expressionStack_.top());
        expressionStack_.pop();

        if (!elementType)
        {
          elementType = boundEl->type;
        }
        else if (!canConvert(boundEl->type, elementType))
        {
          error(node.span, "Array elements must have the same type. Expected '" +
                               elementType->toString() + "', but got '" +
                               boundEl->type->toString() + "'");
        }
        elements.push_back(std::move(boundEl));
      }
    }

    auto arrayType = std::make_shared<zir::ArrayType>(
        elementType ? elementType
                    : std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void),
        elements.size());
    expressionStack_.push(
        std::make_unique<BoundArrayLiteral>(std::move(elements), arrayType));
  }

  void Binder::visit(TypeAliasDecl &node) { (void)node; }

  void Binder::visit(RecordDecl &node)
  {
    auto symbol = std::dynamic_pointer_cast<TypeSymbol>(
        currentScope_->lookup(node.name_));
    auto recordType = std::static_pointer_cast<zir::RecordType>(symbol->type);

    for (const auto &field : node.fields_)
    {
      auto fieldType = mapType(*field->type);
      if (!fieldType)
      {
        error(field->span, "Unknown type: " + field->type->qualifiedName());
        fieldType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
      }
      recordType->addField(field->name, fieldType);
    }

    auto boundRecord = std::make_unique<BoundRecordDeclaration>();
    boundRecord->type = recordType;
    boundRoot_->records.push_back(std::move(boundRecord));
  }

  void Binder::visit(StructDeclarationNode &node)
  {
    auto symbol = std::dynamic_pointer_cast<TypeSymbol>(
        currentScope_->lookup(node.name_));
    auto recordType = std::static_pointer_cast<zir::RecordType>(symbol->type);
    int oldUnsafeTypeContextDepth = unsafeTypeContextDepth_;
    int oldExternTypeContextDepth = externTypeContextDepth_;
    if (node.isUnsafe_)
    {
      requireUnsafeEnabled(node.span, "'unsafe struct'");
      ++unsafeTypeContextDepth_;
    }
    ++externTypeContextDepth_;

    for (const auto &field : node.fields_)
    {
      auto fieldType = mapType(*field->type);
      if (!fieldType)
      {
        error(field->span, "Unknown type: " + field->type->qualifiedName());
        fieldType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
      }
      recordType->addField(field->name, fieldType);
    }

    unsafeTypeContextDepth_ = oldUnsafeTypeContextDepth;
    externTypeContextDepth_ = oldExternTypeContextDepth;

    auto boundRecord = std::make_unique<BoundRecordDeclaration>();
    boundRecord->type = recordType;
    boundRoot_->records.push_back(std::move(boundRecord));
  }

  void Binder::visit(StructLiteralNode &node)
  {
    auto parts = splitQualified(node.type_name_);
    auto symbol =
        resolveQualifiedSymbol(parts, node.span, SymbolKind::Type);
    if (!symbol || symbol->getKind() != SymbolKind::Type)
    {
      error(node.span, "Unknown type: " + node.type_name_);
      return;
    }

    auto typeSymbol = std::static_pointer_cast<TypeSymbol>(symbol);
    if (typeSymbol->type->getKind() != zir::TypeKind::Record)
    {
      error(node.span, "'" + node.type_name_ + "' is not a struct.");
      return;
    }

    if (typeSymbol->isUnsafe)
    {
      requireUnsafeEnabled(node.span, "unsafe struct literals");
      requireUnsafeContext(node.span, "unsafe struct literals");
    }

    auto recordType = std::static_pointer_cast<zir::RecordType>(typeSymbol->type);
    std::vector<std::pair<std::string, std::unique_ptr<BoundExpression>>> boundFields;

    for (auto &fieldInit : node.fields_)
    {
      fieldInit.value->accept(*this);
      if (expressionStack_.empty())
        continue;
      auto boundVal = std::move(expressionStack_.top());
      expressionStack_.pop();

      bool found = false;
      for (const auto &f : recordType->getFields())
      {
        if (f.name == fieldInit.name)
        {
          if (!canConvert(boundVal->type, f.type))
          {
            error(node.span, "Cannot assign type '" + boundVal->type->toString() +
                                 "' to field '" + f.name + "' of type '" +
                                 f.type->toString() + "'");
          }
          found = true;
          break;
        }
      }

      if (!found)
      {
        error(node.span, "Field '" + fieldInit.name +
                             "' not found in struct '" + node.type_name_ + "'");
      }

      boundFields.push_back({fieldInit.name, std::move(boundVal)});
    }

    for (const auto &f : recordType->getFields())
    {
      bool initialized = false;
      for (const auto &bf : boundFields)
      {
        if (bf.first == f.name)
        {
          initialized = true;
          break;
        }
      }
      if (!initialized)
      {
        error(node.span, "Field '" + f.name + "' of struct '" +
                             node.type_name_ + "' is not initialized.");
      }
    }

    expressionStack_.push(
        std::make_unique<BoundStructLiteral>(std::move(boundFields), recordType));
  }

  void Binder::visit(EnumDecl &node)
  {
    auto symbol = std::dynamic_pointer_cast<TypeSymbol>(
        currentScope_->lookup(node.name_));
    auto enumType = std::static_pointer_cast<zir::EnumType>(symbol->type);

    auto boundEnum = std::make_unique<BoundEnumDeclaration>();
    boundEnum->type = enumType;
    boundRoot_->enums.push_back(std::move(boundEnum));
  }

  bool Binder::isNumeric(std::shared_ptr<zir::Type> type)
  {
    return type->isInteger() || type->isFloatingPoint();
  }

  bool Binder::isPointerType(std::shared_ptr<zir::Type> type) const
  {
    return type && type->getKind() == zir::TypeKind::Pointer;
  }

  bool Binder::isNullType(std::shared_ptr<zir::Type> type) const
  {
    return type && type->getKind() == zir::TypeKind::NullPtr;
  }

  bool Binder::isUnsafeActive() const
  {
    return unsafeDepth_ > 0 || unsafeTypeContextDepth_ > 0;
  }

  void Binder::requireUnsafeEnabled(SourceSpan span, const std::string &feature)
  {
    if (!allowUnsafe_)
    {
      error(span, "Using " + feature + " requires '--allow-unsafe'.");
    }
  }

  void Binder::requireUnsafeContext(SourceSpan span, const std::string &feature)
  {
    if (!isUnsafeActive())
    {
      error(span, "Using " + feature + " is only allowed inside unsafe code.");
    }
  }

  bool Binder::canConvert(std::shared_ptr<zir::Type> from,
                          std::shared_ptr<zir::Type> to)
  {
    if (!from || !to)
      return false;
    if (from->getKind() == to->getKind() &&
        from->toString() == to->toString())
      return true;
    if (isNullType(from) && isPointerType(to))
      return true;
    if (isNumeric(from) && isNumeric(to))
      return true;
    return false;
  }

  std::shared_ptr<zir::Type>
  Binder::getPromotedType(std::shared_ptr<zir::Type> t1,
                          std::shared_ptr<zir::Type> t2)
  {
    if (t1->toString() == t2->toString())
      return t1;
    if (t1->isFloatingPoint() || t2->isFloatingPoint())
    {
      if (t1->getKind() == zir::TypeKind::Float64 ||
          t2->getKind() == zir::TypeKind::Float64)
      {
        return std::make_shared<zir::PrimitiveType>(zir::TypeKind::Float64);
      }
      return std::make_shared<zir::PrimitiveType>(zir::TypeKind::Float);
    }
    return std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int64);
  }

  std::shared_ptr<zir::Type>
  Binder::getCVariadicArgumentType(std::shared_ptr<zir::Type> type)
  {
    if (!type)
      return nullptr;

    if (isPointerType(type))
      return type;

    switch (type->getKind())
    {
    case zir::TypeKind::Float:
    case zir::TypeKind::Float32:
      return std::make_shared<zir::PrimitiveType>(zir::TypeKind::Float64);
    case zir::TypeKind::Bool:
    case zir::TypeKind::Char:
    case zir::TypeKind::Int8:
    case zir::TypeKind::Int16:
    case zir::TypeKind::UInt8:
    case zir::TypeKind::UInt16:
      return std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int);
    case zir::TypeKind::Int32:
    case zir::TypeKind::Int64:
    case zir::TypeKind::UInt32:
    case zir::TypeKind::UInt64:
    case zir::TypeKind::Int:
    case zir::TypeKind::UInt:
    case zir::TypeKind::Float64:
      return type;
    default:
      return nullptr;
    }
  }

  std::unique_ptr<BoundExpression>
  Binder::wrapInCast(std::unique_ptr<BoundExpression> expr,
                     std::shared_ptr<zir::Type> targetType)
  {
    if (!expr || !targetType)
      return expr;
    if (expr->type->getKind() == targetType->getKind() &&
        expr->type->toString() == targetType->toString())
    {
      return expr;
    }
    if (isNullType(expr->type) && isPointerType(targetType))
    {
      return std::make_unique<BoundCast>(std::move(expr), targetType);
    }
    return std::make_unique<BoundCast>(std::move(expr), targetType);
  }

  void Binder::error(SourceSpan span, const std::string &message)
  {
    hadError_ = true;
    _diag.report(span, zap::DiagnosticLevel::Error, message);
  }

} // namespace sema
