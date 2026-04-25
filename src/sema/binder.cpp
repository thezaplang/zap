#include "binder.hpp"
#include "../ast/class_decl.hpp"
#include "../ast/const/const_char.hpp"
#include "../ast/record_decl.hpp"
#include <algorithm>
#include <cctype>
#include <functional>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace sema {
namespace {
bool isStringType(const std::shared_ptr<zir::Type> &type) {
  return type &&
         (type->getKind() == zir::TypeKind::Record ||
          type->getKind() == zir::TypeKind::Class) &&
         static_cast<zir::RecordType *>(type.get())->getName() == "String";
}

std::string sanitizeTypeName(const std::string &value) {
  std::string out;
  out.reserve(value.size());
  for (char ch : value) {
    if (std::isalnum(static_cast<unsigned char>(ch))) {
      out.push_back(ch);
    } else {
      out.push_back('_');
    }
  }
  return out;
}

std::string renderGenericTypeName(
    const std::string &baseName,
    const std::vector<std::shared_ptr<zir::Type>> &arguments) {
  std::string name = baseName + "<";
  for (size_t i = 0; i < arguments.size(); ++i) {
    if (i != 0) {
      name += ", ";
    }
    name += arguments[i] ? arguments[i]->toString() : "<?>";
  }
  name += ">";
  return name;
}

std::string renderGenericCodegenName(
    const std::string &baseName,
    const std::vector<std::shared_ptr<zir::Type>> &arguments) {
  std::string suffix;
  for (size_t i = 0; i < arguments.size(); ++i) {
    if (i != 0) {
      suffix += "$";
    }
    suffix += sanitizeTypeName(arguments[i] ? arguments[i]->toString()
                                            : std::string("<?>"));
  }
  return baseName + "$g$" + suffix;
}

std::shared_ptr<zir::RecordType>
makeVariadicViewType(const std::shared_ptr<zir::Type> &elementType) {
  auto suffix = sanitizeTypeName(elementType->toString());
  auto type = std::make_shared<zir::RecordType>("__zap_varargs_" + suffix,
                                                "__zap_varargs_" + suffix);
  type->addField("data", std::make_shared<zir::PointerType>(elementType));
  type->addField("len",
                 std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int));
  return type;
}

bool isVariadicViewType(const std::shared_ptr<zir::Type> &type) {
  return type && type->getKind() == zir::TypeKind::Record &&
         static_cast<zir::RecordType *>(type.get())
                 ->getName()
                 .rfind("__zap_varargs_", 0) == 0;
}

std::vector<std::string> splitQualified(const std::string &value) {
  std::vector<std::string> parts;
  std::stringstream ss(value);
  std::string item;
  while (std::getline(ss, item, '.')) {
    if (!item.empty()) {
      parts.push_back(item);
    }
  }
  return parts;
}

bool extractQualifiedPath(const ExpressionNode *expr,
                          std::vector<std::string> &parts) {
  if (auto id = dynamic_cast<const ConstId *>(expr)) {
    parts.push_back(id->value_);
    return true;
  }

  if (auto member = dynamic_cast<const MemberAccessNode *>(expr)) {
    if (!extractQualifiedPath(member->left_.get(), parts)) {
      return false;
    }
    parts.push_back(member->member_);
    return true;
  }

  return false;
}

std::vector<std::shared_ptr<FunctionSymbol>>
collectOverloads(const std::shared_ptr<Symbol> &symbol) {
  if (!symbol) {
    return {};
  }
  if (auto function = std::dynamic_pointer_cast<FunctionSymbol>(symbol)) {
    return {function};
  }
  if (auto set = std::dynamic_pointer_cast<OverloadSetSymbol>(symbol)) {
    return set->overloads;
  }
  return {};
}

bool sameFunctionSignature(const FunctionSymbol &lhs,
                           const FunctionSymbol &rhs) {
  if (lhs.parameters.size() != rhs.parameters.size() ||
      lhs.isCVariadic != rhs.isCVariadic) {
    return false;
  }
  for (size_t i = 0; i < lhs.parameters.size(); ++i) {
    const auto &left = lhs.parameters[i];
    const auto &right = rhs.parameters[i];
    if (left->is_ref != right->is_ref ||
        left->is_variadic_pack != right->is_variadic_pack) {
      return false;
    }
    if (!left->type || !right->type ||
        left->type->toString() != right->type->toString()) {
      return false;
    }
  }
  return true;
}
} // namespace

Binder::Binder(zap::DiagnosticEngine &diag, bool allowUnsafe)
    : _diag(diag), allowUnsafe_(allowUnsafe), hadError_(false) {}

std::unique_ptr<BoundRootNode> Binder::bind(RootNode &root) {
  (void)root;
  _diag.report(
      SourceSpan(), zap::DiagnosticLevel::Error,
      "Internal error: single-file binder entry point is unsupported.");
  return nullptr;
}

std::unique_ptr<BoundRootNode> Binder::bind(std::vector<ModuleInfo> &modules) {
  hadError_ = false;
  boundRoot_ = std::make_unique<BoundRootNode>();
  modules_.clear();
  currentScope_.reset();
  currentFunction_.reset();
  currentModuleId_.clear();
  declaredFunctionSymbols_.clear();
  recordTypeDeclarationNodes_.clear();
  structTypeDeclarationNodes_.clear();
  classTypeDeclarationNodes_.clear();
  typeDeclarationModuleIds_.clear();
  functionDeclarationNodes_.clear();
  functionGenericParamNames_.clear();
  genericFunctionInstantiations_.clear();
  genericTypeInstantiations_.clear();
  genericFunctionDeclarationKeys_.clear();
  genericInstantiationEmitted_.clear();
  genericInstantiationInProgress_.clear();
  activeGenericBindingsStack_.clear();
  unsafeDepth_ = 0;
  unsafeTypeContextDepth_ = 0;
  externTypeContextDepth_ = 0;

  initializeBuiltins();

  for (auto &module : modules) {
    ModuleState state;
    state.info = &module;
    state.scope = std::make_shared<SymbolTable>(builtinScope_);
    state.symbol =
        std::make_shared<ModuleSymbol>(module.moduleName, module.moduleId);
    modules_[module.moduleId] = state;
  }

  for (auto &[_, module] : modules_) {
    predeclareModuleTypes(module);
  }

  for (auto &[_, module] : modules_) {
    applyImports(module, true);
  }

  for (auto &[_, module] : modules_) {
    predeclareModuleAliases(module);
  }

  for (auto &[_, module] : modules_) {
    applyImports(module, true);
  }

  for (auto &[_, module] : modules_) {
    ensureModuleValuesReady(module);
  }

  for (auto &[_, module] : modules_) {
    currentModuleId_ = module.info->moduleId;
    currentScope_ = module.scope;
    for (const auto &child : module.info->root->children) {
      if (dynamic_cast<RecordDecl *>(child.get()) ||
          dynamic_cast<ClassDecl *>(child.get()) ||
          dynamic_cast<StructDeclarationNode *>(child.get()) ||
          dynamic_cast<EnumDecl *>(child.get())) {
        child->accept(*this);
      }
    }
  }

  for (auto &[_, module] : modules_) {
    currentModuleId_ = module.info->moduleId;
    currentScope_ = module.scope;
    for (const auto &child : module.info->root->children) {
      if (dynamic_cast<ImportNode *>(child.get()) ||
          dynamic_cast<RecordDecl *>(child.get()) ||
          dynamic_cast<ClassDecl *>(child.get()) ||
          dynamic_cast<StructDeclarationNode *>(child.get()) ||
          dynamic_cast<EnumDecl *>(child.get()) ||
          dynamic_cast<TypeAliasDecl *>(child.get())) {
        continue;
      }
      child->accept(*this);
    }
  }

  return (hadError_ || _diag.hadErrors()) ? nullptr : std::move(boundRoot_);
}

void Binder::initializeBuiltins() {
  builtinScope_ = std::make_shared<SymbolTable>();

  auto declareType = [&](const std::string &name, zir::TypeKind kind) {
    builtinScope_->declare(name,
                           std::make_shared<TypeSymbol>(
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

  builtinScope_->declare(
      "String",
      std::make_shared<TypeSymbol>(
          "String", std::make_shared<zir::RecordType>("String", "String"),
          "String", "", Visibility::Public));
}

std::string Binder::mangleName(const std::string &modulePath,
                               const std::string &name) const {
  std::string mangled = "zap$";
  for (char c : modulePath) {
    if (std::isalnum(static_cast<unsigned char>(c))) {
      mangled += c;
    } else if (c == '/' || c == '\\' || c == '.' || c == '-') {
      mangled += '$';
    } else {
      mangled += '_';
    }
  }
  if (!mangled.empty() && mangled.back() != '$') {
    mangled += '$';
  }
  mangled += name;
  return mangled;
}

std::string Binder::functionSignatureKey(const FunctionSymbol &function) const {
  std::string key;
  for (const auto &param : function.parameters) {
    if (!key.empty()) {
      key += "|";
    }
    if (param->is_ref) {
      key += "ref:";
    }
    if (param->is_variadic_pack) {
      key += "varargs:";
    }
    key += param->type ? param->type->toString() : "Void";
  }
  if (function.isCVariadic) {
    if (!key.empty()) {
      key += "|";
    }
    key += "cvarargs";
  }
  return key;
}

std::string
Binder::renderFunctionSignature(const FunctionSymbol &function) const {
  std::string rendered = function.name;
  auto genericParamNamesIt = functionGenericParamNames_.find(&function);
  const auto *genericParamNames =
      genericParamNamesIt != functionGenericParamNames_.end()
          ? &genericParamNamesIt->second
          : &function.genericParameterNames;
  if (genericParamNames && !genericParamNames->empty()) {
    rendered += "<";
    for (size_t i = 0; i < genericParamNames->size(); ++i) {
      if (i != 0) {
        rendered += ", ";
      }
      const auto &name = (*genericParamNames)[i];
      auto argIt = function.genericArguments.find(name);
      rendered += (argIt != function.genericArguments.end() && argIt->second)
                      ? argIt->second->toString()
                      : name;
    }
    rendered += ">";
  }

  rendered += "(";
  for (size_t i = 0; i < function.parameters.size(); ++i) {
    if (i != 0) {
      rendered += ", ";
    }
    const auto &param = function.parameters[i];
    if (param->is_ref) {
      rendered += "ref ";
    }
    if (param->is_variadic_pack) {
      rendered += "...";
      rendered += param->variadic_element_type
                      ? param->variadic_element_type->toString()
                      : (param->type ? param->type->toString() : "Void");
    } else {
      rendered += param->type ? param->type->toString() : "Void";
    }
  }
  if (function.isCVariadic) {
    if (!function.parameters.empty()) {
      rendered += ", ";
    }
    rendered += "...";
  }
  rendered += ")";
  return rendered;
}

std::string Binder::mangleFunctionName(const std::string &modulePath,
                                       const FunctionSymbol &function) const {
  return mangleName(modulePath,
                    function.name + "$" +
                        sanitizeTypeName(functionSignatureKey(function)));
}

std::shared_ptr<FunctionSymbol>
Binder::findFunctionBySignature(const std::shared_ptr<Symbol> &symbol,
                                const FunctionSymbol &prototype) const {
  for (const auto &candidate : collectOverloads(symbol)) {
    if (candidate && sameFunctionSignature(*candidate, prototype)) {
      return candidate;
    }
  }
  return nullptr;
}

std::shared_ptr<FunctionSymbol> Binder::ensureGenericFunctionInstantiation(
    const std::shared_ptr<FunctionSymbol> &baseFunction,
    const std::vector<std::pair<std::string, std::shared_ptr<zir::Type>>>
        &genericBindings,
    SourceSpan callSpan) {
  if (!baseFunction) {
    return nullptr;
  }

  if (baseFunction->genericParameterNames.empty()) {
    return baseFunction;
  }

  std::vector<std::string> missing;
  for (const auto &name : baseFunction->genericParameterNames) {
    auto it = std::find_if(
        genericBindings.begin(), genericBindings.end(),
        [&](const auto &entry) { return entry.first == name; });
    if (it == genericBindings.end()) {
      missing.push_back(name);
    }
  }
  if (!missing.empty()) {
    std::string msg = "Missing generic type arguments for function '" +
                      baseFunction->name + "': ";
    for (size_t i = 0; i < missing.size(); ++i) {
      if (i != 0) {
        msg += ", ";
      }
      msg += missing[i];
    }
    error(callSpan, msg);
    return nullptr;
  }

  std::string cacheKey = baseFunction->linkName + "<";
  for (size_t i = 0; i < baseFunction->genericParameterNames.size(); ++i) {
    if (i != 0) {
      cacheKey += ",";
    }
    const auto &name = baseFunction->genericParameterNames[i];
    auto it = std::find_if(
        genericBindings.begin(), genericBindings.end(),
        [&](const auto &entry) { return entry.first == name; });
    cacheKey += name + "=" + (it != genericBindings.end() && it->second
                                  ? it->second->toString()
                                  : std::string("<?>"));
  }
  cacheKey += ">";

  auto cachedIt = genericFunctionInstantiations_.find(cacheKey);
  if (cachedIt != genericFunctionInstantiations_.end()) {
    return cachedIt->second;
  }

  auto declIt = functionDeclarationNodes_.find(baseFunction.get());
  if (declIt == functionDeclarationNodes_.end() || !declIt->second) {
    error(callSpan, "Internal error: missing declaration for generic function '" +
                        baseFunction->name + "'.");
    return nullptr;
  }

  auto moduleIdIt = functionDeclarationModuleIds_.find(baseFunction.get());
  auto moduleId =
      moduleIdIt == functionDeclarationModuleIds_.end() ? currentModuleId_
                                                        : moduleIdIt->second;
  auto moduleIt = modules_.find(moduleId);
  if (moduleIt == modules_.end() || !moduleIt->second.info) {
    error(callSpan, "Internal error: current module not found for generic instantiation.");
    return nullptr;
  }

  std::unordered_map<std::string, std::shared_ptr<zir::Type>> genericBindingMap;
  genericBindingMap.reserve(genericBindings.size());
  for (const auto &[name, type] : genericBindings) {
    genericBindingMap[name] = type;
  }

  std::vector<std::shared_ptr<VariableSymbol>> instantiatedParams;
  instantiatedParams.reserve(baseFunction->parameters.size());
  for (const auto &param : baseFunction->parameters) {
    auto instType = substituteGenericType(param->type, genericBindingMap);
    auto instParam = std::make_shared<VariableSymbol>(
        param->name, instType, param->is_const, param->is_ref, param->linkName,
        param->moduleName, param->visibility);
    instParam->is_variadic_pack = param->is_variadic_pack;
    instParam->variadic_element_type =
        substituteGenericType(param->variadic_element_type, genericBindingMap);
    instantiatedParams.push_back(std::move(instParam));
  }

  auto instantiatedReturn =
      substituteGenericType(baseFunction->returnType, genericBindingMap);

  auto instantiated = std::make_shared<FunctionSymbol>(
      baseFunction->name, std::move(instantiatedParams), instantiatedReturn, "",
      baseFunction->moduleName, baseFunction->visibility, baseFunction->isUnsafe,
      baseFunction->isCVariadic);
  instantiated->isMethod = baseFunction->isMethod;
  instantiated->isStatic = baseFunction->isStatic;
  instantiated->isConstructor = baseFunction->isConstructor;
  instantiated->isDestructor = baseFunction->isDestructor;
  instantiated->vtableSlot = -1;
  instantiated->ownerTypeName = baseFunction->ownerTypeName;
  instantiated->isGenericInstantiation = true;
  instantiated->genericArguments.clear();
  for (const auto &[name, type] : genericBindings) {
    instantiated->genericArguments[name] = type;
  }

  std::string genericSuffix;
  for (size_t i = 0; i < baseFunction->genericParameterNames.size(); ++i) {
    if (i != 0) {
      genericSuffix += "$";
    }
    const auto &name = baseFunction->genericParameterNames[i];
    auto it = std::find_if(
        genericBindings.begin(), genericBindings.end(),
        [&](const auto &entry) { return entry.first == name; });
    if (it == genericBindings.end() || !it->second) {
      error(callSpan, "Missing binding for generic parameter '" + name + "'.");
      return nullptr;
    }
    genericSuffix += sanitizeTypeName(name + "_" + it->second->toString());
  }
  instantiated->linkName = baseFunction->linkName + "$g$" + genericSuffix;

  genericFunctionInstantiations_[cacheKey] = instantiated;
  functionGenericParamNames_[instantiated.get()] = {};
  functionDeclarationNodes_[instantiated.get()] = declIt->second;
  functionDeclarationModuleIds_[instantiated.get()] = moduleId;

  auto inProgressIt = std::find(genericInstantiationInProgress_.begin(),
                                genericInstantiationInProgress_.end(), cacheKey);
  if (inProgressIt != genericInstantiationInProgress_.end()) {
    return instantiated;
  }

  auto emittedIt = genericInstantiationEmitted_.find(instantiated.get());
  if (emittedIt != genericInstantiationEmitted_.end() && emittedIt->second) {
    return instantiated;
  }

  genericInstantiationInProgress_.push_back(cacheKey);

  auto oldScope = currentScope_;
  auto oldFunction = currentFunction_;
  auto oldModuleId = currentModuleId_;
  int oldUnsafeDepth = unsafeDepth_;

  currentModuleId_ = moduleIt->second.info->moduleId;
  currentScope_ = moduleIt->second.scope;
  currentFunction_ = instantiated;
  if (instantiated->isUnsafe) {
    ++unsafeDepth_;
  }

  pushScope();
  for (const auto &param : instantiated->parameters) {
    if (!currentScope_->declare(param->name, param)) {
      error(callSpan, "Parameter '" + param->name +
                          "' already declared in generic instantiation.");
    }
  }

  activeGenericBindingsStack_.push_back(
      std::unordered_map<std::string, std::shared_ptr<zir::Type>>(
          genericBindings.begin(), genericBindings.end()));
  auto boundBody = bindBody(declIt->second->body_.get(), false);
  activeGenericBindingsStack_.pop_back();

  popScope();

  currentScope_ = oldScope;
  currentFunction_ = oldFunction;
  currentModuleId_ = oldModuleId;
  unsafeDepth_ = oldUnsafeDepth;

  bool hasReturn = false;
  if (boundBody) {
    if (boundBody->result) {
      hasReturn = true;
    }
    for (const auto &stmt : boundBody->statements) {
      if (dynamic_cast<BoundReturnStatement *>(stmt.get())) {
        hasReturn = true;
        break;
      }
    }
  }

  if (!hasReturn && instantiated->linkName == "main" &&
      instantiated->returnType->isInteger()) {
    auto intType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int);
    auto lit = std::make_unique<BoundLiteral>("0", intType);
    boundBody->statements.push_back(
        std::make_unique<BoundReturnStatement>(std::move(lit)));
    hasReturn = true;
  }

  if (!hasReturn && instantiated->returnType->getKind() != zir::TypeKind::Void) {
    auto kind = instantiated->returnType->getKind();
    if (instantiated->returnType->isInteger() || kind == zir::TypeKind::Float ||
        kind == zir::TypeKind::Bool) {
      std::string litVal = "0";
      if (kind == zir::TypeKind::Float)
        litVal = "0.0";
      else if (kind == zir::TypeKind::Bool)
        litVal = "false";
      auto lit = std::make_unique<BoundLiteral>(litVal, instantiated->returnType);
      boundBody->statements.push_back(
          std::make_unique<BoundReturnStatement>(std::move(lit)));
    }

    _diag.report(callSpan, zap::DiagnosticLevel::Warning,
                 "Generic function '" + instantiated->name +
                     "' has non-void return type but no return on some paths.");
  }

  boundRoot_->functions.push_back(std::make_unique<BoundFunctionDeclaration>(
      instantiated, std::move(boundBody)));
  genericInstantiationEmitted_[instantiated.get()] = true;

  genericInstantiationInProgress_.pop_back();
  return instantiated;
}

bool Binder::isGenericTypeParameterName(std::string_view name) const {
  if (activeGenericBindingsStack_.empty()) {
    return false;
  }
  const auto &bindings = activeGenericBindingsStack_.back();
  return bindings.find(std::string(name)) != bindings.end();
}

std::shared_ptr<zir::Type> Binder::substituteGenericType(
    std::shared_ptr<zir::Type> type,
    const std::unordered_map<std::string, std::shared_ptr<zir::Type>>
        &genericBindings) const {
  if (!type) {
    return nullptr;
  }

  if (type->getKind() == zir::TypeKind::Record) {
    auto record = std::static_pointer_cast<zir::RecordType>(type);
    auto it = genericBindings.find(record->getName());
    if (it != genericBindings.end()) {
      return it->second;
    }
    if (record->isGenericInstance()) {
      std::vector<std::shared_ptr<zir::Type>> substitutedArgs;
      substitutedArgs.reserve(record->getGenericArguments().size());
      for (const auto &arg : record->getGenericArguments()) {
        substitutedArgs.push_back(substituteGenericType(arg, genericBindings));
      }

      auto substituted = std::make_shared<zir::RecordType>(
          renderGenericTypeName(record->getGenericBaseName(), substitutedArgs),
          renderGenericCodegenName(record->getGenericCodegenBaseName(),
                                   substitutedArgs));
      substituted->setGenericInstance(record->getGenericBaseName(),
                                      record->getGenericCodegenBaseName(),
                                      substitutedArgs);
      for (const auto &field : record->getFields()) {
        substituted->addField(field.name,
                              substituteGenericType(field.type, genericBindings),
                              field.visibility);
      }
      return substituted;
    }
    return type;
  }

  if (type->getKind() == zir::TypeKind::Class) {
    auto classType = std::static_pointer_cast<zir::ClassType>(type);
    auto it = genericBindings.find(classType->getName());
    if (it != genericBindings.end()) {
      return it->second;
    }
    if (classType->isGenericInstance()) {
      std::vector<std::shared_ptr<zir::Type>> substitutedArgs;
      substitutedArgs.reserve(classType->getGenericArguments().size());
      for (const auto &arg : classType->getGenericArguments()) {
        substitutedArgs.push_back(substituteGenericType(arg, genericBindings));
      }

      auto substituted = std::make_shared<zir::ClassType>(
          renderGenericTypeName(classType->getGenericBaseName(),
                                substitutedArgs),
          renderGenericCodegenName(classType->getGenericCodegenBaseName(),
                                   substitutedArgs));
      substituted->setGenericInstance(classType->getGenericBaseName(),
                                      classType->getGenericCodegenBaseName(),
                                      substitutedArgs);
      substituted->setWeak(classType->isWeak());
      if (auto base = classType->getBase()) {
        auto substitutedBase = substituteGenericType(base, genericBindings);
        if (substitutedBase &&
            substitutedBase->getKind() == zir::TypeKind::Class) {
          substituted->setBase(
              std::static_pointer_cast<zir::ClassType>(substitutedBase));
        }
      }
      for (const auto &field : classType->getFields()) {
        substituted->addField(field.name,
                              substituteGenericType(field.type, genericBindings),
                              field.visibility);
      }
      return substituted;
    }
    return type;
  }

  if (type->getKind() == zir::TypeKind::Pointer) {
    auto ptr = std::static_pointer_cast<zir::PointerType>(type);
    auto base = substituteGenericType(ptr->getBaseType(), genericBindings);
    return std::make_shared<zir::PointerType>(base);
  }

  if (type->getKind() == zir::TypeKind::Array) {
    auto arr = std::static_pointer_cast<zir::ArrayType>(type);
    auto base = substituteGenericType(arr->getBaseType(), genericBindings);
    return std::make_shared<zir::ArrayType>(base, arr->getSize());
  }

  return type;
}

bool Binder::validateGenericConstraints(
    const std::vector<GenericConstraint> &constraints,
    std::unordered_map<std::string, std::shared_ptr<zir::Type>> &bindings,
    std::string *failureReason) {
  for (const auto &constraint : constraints) {
    auto boundIt = bindings.find(constraint.parameterName);
    if (boundIt == bindings.end() || !boundIt->second) {
      if (failureReason) {
        *failureReason = "missing binding for constrained type parameter '" +
                         constraint.parameterName + "'";
      }
      return false;
    }
    if (!constraint.boundType) {
      continue;
    }

    activeGenericBindingsStack_.push_back(bindings);
    auto requiredType = mapType(*constraint.boundType);
    activeGenericBindingsStack_.pop_back();
    if (!requiredType) {
      if (failureReason) {
        *failureReason = "unknown constraint type for '" +
                         constraint.parameterName + "'";
      }
      return false;
    }
    if (!canConvert(boundIt->second, requiredType)) {
      if (failureReason) {
        *failureReason = "type parameter '" + constraint.parameterName +
                         "' with type '" + boundIt->second->toString() +
                         "' does not satisfy constraint '" +
                         constraint.parameterName + ": " +
                         requiredType->toString() + "'";
      }
      return false;
    }
  }
  return true;
}

std::vector<std::pair<std::string, std::shared_ptr<zir::Type>>>
Binder::orderedGenericBindings(
    const std::unordered_map<std::string, std::shared_ptr<zir::Type>>
        &genericBindings) const {
  std::vector<std::pair<std::string, std::shared_ptr<zir::Type>>> ordered;
  ordered.reserve(genericBindings.size());
  for (const auto &[name, type] : genericBindings) {
    ordered.emplace_back(name, type);
  }
  std::sort(ordered.begin(), ordered.end(),
            [](const auto &lhs, const auto &rhs) {
              return lhs.first < rhs.first;
            });
  return ordered;
}

std::shared_ptr<TypeSymbol> Binder::instantiateGenericTypeSymbol(
    const std::shared_ptr<TypeSymbol> &baseSymbol, const TypeNode &typeNode) {
  if (!baseSymbol || baseSymbol->genericParameterNames.empty()) {
    return baseSymbol;
  }

  std::string cacheKey = baseSymbol->linkName + "<";
  std::shared_ptr<zir::RecordType> instantiatedType;
  const RecordDecl *recordDecl = nullptr;
  const StructDeclarationNode *structDecl = nullptr;
  const ClassDecl *classDecl = nullptr;
  if (auto recordDeclIt = recordTypeDeclarationNodes_.find(baseSymbol.get());
      recordDeclIt != recordTypeDeclarationNodes_.end()) {
    recordDecl = recordDeclIt->second;
  } else if (auto structDeclIt = structTypeDeclarationNodes_.find(baseSymbol.get());
             structDeclIt != structTypeDeclarationNodes_.end()) {
    structDecl = structDeclIt->second;
  } else if (auto classDeclIt = classTypeDeclarationNodes_.find(baseSymbol.get());
             classDeclIt != classTypeDeclarationNodes_.end()) {
    classDecl = classDeclIt->second;
  } else {
    error(typeNode.span, "Generic type '" + typeNode.qualifiedName() +
                             "' is not instantiable yet.");
    return nullptr;
  }

  const std::vector<std::unique_ptr<TypeNode>> *declGenericParams =
      recordDecl ? &recordDecl->genericParams_
                 : (structDecl ? &structDecl->genericParams_
                               : &classDecl->genericParams_);
  const std::vector<GenericConstraint> *declGenericConstraints =
      recordDecl ? &recordDecl->genericConstraints_
                 : (structDecl ? &structDecl->genericConstraints_
                               : &classDecl->genericConstraints_);

  if (typeNode.genericArgs.size() > baseSymbol->genericParameterNames.size()) {
    error(typeNode.span, "Generic argument count mismatch for type '" +
                             typeNode.qualifiedName() + "'.");
    return nullptr;
  }

  std::vector<std::shared_ptr<zir::Type>> genericArgs;
  genericArgs.reserve(baseSymbol->genericParameterNames.size());
  std::unordered_map<std::string, std::shared_ptr<zir::Type>> genericBindings;
  for (size_t i = 0; i < baseSymbol->genericParameterNames.size(); ++i) {
    std::shared_ptr<zir::Type> mapped;
    if (i < typeNode.genericArgs.size()) {
      mapped = mapType(*typeNode.genericArgs[i]);
      if (!mapped) {
        error(typeNode.genericArgs[i]->span,
              "Unknown generic type argument in type '" +
                  typeNode.qualifiedName() + "'.");
        return nullptr;
      }
    } else {
      const auto &declParam = (*declGenericParams)[i];
      if (!declParam || !declParam->defaultType) {
        error(typeNode.span, "Missing generic type arguments for type '" +
                                 typeNode.qualifiedName() + "'.");
        return nullptr;
      }
      activeGenericBindingsStack_.push_back(genericBindings);
      mapped = mapType(*declParam->defaultType);
      activeGenericBindingsStack_.pop_back();
      if (!mapped) {
        error(declParam->defaultType->span,
              "Unknown default generic type argument in type '" +
                  typeNode.qualifiedName() + "'.");
        return nullptr;
      }
    }
    genericArgs.push_back(mapped);
    genericBindings[baseSymbol->genericParameterNames[i]] = mapped;
  }

  std::string constraintFailure;
  if (declGenericConstraints &&
      !validateGenericConstraints(*declGenericConstraints, genericBindings,
                                  &constraintFailure)) {
    error(typeNode.span, "Generic constraints not satisfied for type '" +
                             typeNode.qualifiedName() + "': " +
                             constraintFailure);
    return nullptr;
  }

  for (size_t i = 0; i < genericArgs.size(); ++i) {
    if (i != 0) {
      cacheKey += ",";
    }
    cacheKey += genericArgs[i] ? genericArgs[i]->toString() : "<?>";
  }
  cacheKey += ">";

  auto cachedIt = genericTypeInstantiations_.find(cacheKey);
  if (cachedIt != genericTypeInstantiations_.end()) {
    return cachedIt->second;
  }

  auto moduleIdIt = typeDeclarationModuleIds_.find(baseSymbol.get());
  if (moduleIdIt == typeDeclarationModuleIds_.end()) {
    error(typeNode.span,
          "Internal error: missing module information for generic type '" +
              baseSymbol->name + "'.");
    return nullptr;
  }

  auto moduleIt = modules_.find(moduleIdIt->second);
  if (moduleIt == modules_.end() || !moduleIt->second.info) {
    error(typeNode.span,
          "Internal error: missing declaration module for generic type '" +
              baseSymbol->name + "'.");
    return nullptr;
  }

  auto baseRecordType = std::static_pointer_cast<zir::RecordType>(baseSymbol->type);
  auto displayName =
      renderGenericTypeName(baseRecordType->getName(), genericArgs);
  auto codegenName =
      renderGenericCodegenName(baseRecordType->getCodegenName(), genericArgs);
  if (classDecl) {
    instantiatedType = std::make_shared<zir::ClassType>(displayName, codegenName);
  } else {
    instantiatedType = std::make_shared<zir::RecordType>(displayName, codegenName);
  }
  instantiatedType->setGenericInstance(baseRecordType->getName(),
                                       baseRecordType->getCodegenName(),
                                       genericArgs);

  auto instantiatedSymbol = std::make_shared<TypeSymbol>(
      baseSymbol->name, instantiatedType, codegenName, baseSymbol->moduleName,
      baseSymbol->visibility, baseSymbol->isUnsafe, classDecl != nullptr);
  instantiatedSymbol->isGenericInstantiation = true;
  instantiatedSymbol->genericArguments = {
      genericBindings.begin(), genericBindings.end()};
  genericTypeInstantiations_[cacheKey] = instantiatedSymbol;

  if (classDecl) {
    auto instantiatedClassType =
        std::static_pointer_cast<zir::ClassType>(instantiatedType);
    ClassInfo classInfo;
    classInfo.typeSymbol = instantiatedSymbol;
    classInfo.classType = instantiatedClassType;
    classInfo.ownerQualifiedName = instantiatedClassType->getName();
    classInfos_[instantiatedClassType->getName()] = classInfo;
  }

  int oldUnsafeTypeContextDepth = unsafeTypeContextDepth_;
  int oldExternTypeContextDepth = externTypeContextDepth_;
  auto oldScope = currentScope_;
  auto oldModuleId = currentModuleId_;
  if (structDecl && structDecl->isUnsafe_) {
    ++unsafeTypeContextDepth_;
  }
  if (structDecl) {
    ++externTypeContextDepth_;
  }

  currentScope_ = moduleIt->second.scope;
  currentModuleId_ = moduleIt->second.info->moduleId;

  activeGenericBindingsStack_.push_back(genericBindings);
  if (classDecl && classDecl->baseType_) {
    bool hasOwnCtor = false;
    bool hasOwnDtor = false;
    for (const auto &methodDecl : classDecl->methods_) {
      hasOwnCtor = hasOwnCtor || methodDecl->name_ == "init";
      hasOwnDtor = hasOwnDtor || methodDecl->name_ == "deinit";
    }
    auto baseType = mapType(*classDecl->baseType_);
    if (baseType && baseType->getKind() == zir::TypeKind::Class) {
      auto instantiatedClassType =
          std::static_pointer_cast<zir::ClassType>(instantiatedType);
      auto baseClass = std::static_pointer_cast<zir::ClassType>(baseType);
      instantiatedClassType->setBase(baseClass);
      auto &classInfo = classInfos_[instantiatedClassType->getName()];
      auto baseIt = classInfos_.find(baseClass->getName());
      if (baseIt != classInfos_.end()) {
        if (!hasOwnCtor) {
          classInfo.constructor = baseIt->second.constructor;
        }
        if (!hasOwnDtor) {
          classInfo.destructor = baseIt->second.destructor;
        }
        classInfo.fields = baseIt->second.fields;
        classInfo.methods.insert(baseIt->second.methods.begin(),
                                 baseIt->second.methods.end());
        classInfo.nextVirtualSlot = baseIt->second.nextVirtualSlot;
      }
      for (const auto &field : baseClass->getFields()) {
        instantiatedClassType->addField(field.name, field.type, field.visibility);
      }
    } else if (baseType) {
      error(classDecl->baseType_->span, "Base type of class '" + classDecl->name_ +
                                            "' must be a class.");
    }
  }

  const auto &fields =
      recordDecl ? recordDecl->fields_
                 : (structDecl ? structDecl->fields_ : classDecl->fields_);
  for (const auto &field : fields) {
    auto fieldType = mapType(*field->type);
    if (!fieldType) {
      error(field->span, "Unknown type: " + field->type->qualifiedName());
      fieldType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
    }
    instantiatedType->addField(field->name, fieldType,
                               static_cast<int>(field->visibility_));
    if (classDecl) {
      auto instantiatedClassType =
          std::static_pointer_cast<zir::ClassType>(instantiatedType);
      auto &classInfo = classInfos_[instantiatedClassType->getName()];
      classInfo.fields[field->name] = std::make_shared<VariableSymbol>(
          field->name, fieldType, false, false, field->name,
          moduleIt->second.info->moduleName, field->visibility_);
    }
  }

  activeGenericBindingsStack_.pop_back();

  unsafeTypeContextDepth_ = oldUnsafeTypeContextDepth;
  externTypeContextDepth_ = oldExternTypeContextDepth;
  currentScope_ = oldScope;
  currentModuleId_ = oldModuleId;

  if (classDecl) {
    auto instantiatedClassType =
        std::static_pointer_cast<zir::ClassType>(instantiatedType);
    auto &classInfo = classInfos_[instantiatedClassType->getName()];
    auto oldScope = currentScope_;
    auto oldModuleId = currentModuleId_;
    currentScope_ = moduleIt->second.scope;
    currentModuleId_ = moduleIt->second.info->moduleId;

    for (const auto &methodDecl : classDecl->methods_) {
      if (methodDecl->isUnsafe_) {
        requireUnsafeEnabled(methodDecl->span, "'unsafe fun'");
        ++unsafeTypeContextDepth_;
      }

      std::unordered_map<std::string, std::shared_ptr<zir::Type>>
          methodGenericBindings;
      for (const auto &genericParam : methodDecl->genericParams_) {
        if (genericParam) {
          auto placeholder = std::make_shared<zir::RecordType>(
              genericParam->typeName, genericParam->typeName);
          methodGenericBindings[genericParam->typeName] = placeholder;
        }
      }

      std::vector<std::shared_ptr<VariableSymbol>> params;
      if (!methodDecl->isStatic_) {
        params.push_back(std::make_shared<VariableSymbol>(
            "self", instantiatedClassType, false, false, "self",
            moduleIt->second.info->moduleName, Visibility::Private));
      }

      activeGenericBindingsStack_.push_back(genericBindings);
      if (!methodGenericBindings.empty()) {
        activeGenericBindingsStack_.push_back(methodGenericBindings);
      }
      for (size_t i = 0; i < methodDecl->params_.size(); ++i) {
        const auto &p = methodDecl->params_[i];
        auto mappedType = mapType(*p->type);
        if (!mappedType) {
          error(p->span, "Unknown type: " + p->type->qualifiedName());
          mappedType =
              std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
        }
        params.push_back(std::make_shared<VariableSymbol>(
            p->name, mappedType, false, p->isRef, p->name,
            moduleIt->second.info->moduleName, Visibility::Private));
      }

      std::shared_ptr<zir::Type> retType;
      bool isCtor = methodDecl->name_ == "init";
      bool isDtor = methodDecl->name_ == "deinit";
      if (isDtor && (!methodDecl->params_.empty() || methodDecl->returnType_)) {
        error(methodDecl->span,
              "Destructor 'deinit' cannot have parameters or a return type.");
      }
      if (isCtor || isDtor) {
        retType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
      } else if (methodDecl->returnType_) {
        retType = mapType(*methodDecl->returnType_);
      } else {
        retType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
      }
      if (!methodGenericBindings.empty()) {
        activeGenericBindingsStack_.pop_back();
      }
      activeGenericBindingsStack_.pop_back();

      if (!retType) {
        retType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
      }

      if (methodDecl->isUnsafe_) {
        --unsafeTypeContextDepth_;
      }

      auto methodSymbol = std::make_shared<FunctionSymbol>(
          methodDecl->name_, std::move(params), std::move(retType), "",
          moduleIt->second.info->moduleName, methodDecl->visibility_,
          methodDecl->isUnsafe_);
      for (const auto &genericParam : methodDecl->genericParams_) {
        if (genericParam) {
          methodSymbol->genericParameterNames.push_back(genericParam->typeName);
        }
      }
      methodSymbol->isMethod = !methodDecl->isStatic_;
      methodSymbol->isStatic = methodDecl->isStatic_;
      methodSymbol->isConstructor = isCtor;
      methodSymbol->isDestructor = isDtor;
      methodSymbol->ownerTypeName = instantiatedClassType->getName();
      if (methodSymbol->isMethod && !methodSymbol->isStatic &&
          !methodSymbol->isConstructor && !methodSymbol->isDestructor) {
        auto existingIt = classInfo.methods.find(methodDecl->name_);
        if (existingIt != classInfo.methods.end()) {
          auto existingMethod =
              std::dynamic_pointer_cast<FunctionSymbol>(existingIt->second);
          if (existingMethod && existingMethod->vtableSlot >= 0) {
            methodSymbol->vtableSlot = existingMethod->vtableSlot;
          }
        }
        if (methodSymbol->vtableSlot < 0) {
          methodSymbol->vtableSlot = classInfo.nextVirtualSlot++;
        }
      }
      methodSymbol->linkName =
          mangleName(moduleIt->second.info->linkPath.empty()
                         ? moduleIt->second.info->moduleId
                         : moduleIt->second.info->linkPath,
                     instantiatedClassType->getCodegenName() + "$" +
                         methodDecl->name_ + "$" +
                         sanitizeTypeName(functionSignatureKey(*methodSymbol)));
      functionDeclarationNodes_[methodSymbol.get()] = methodDecl.get();
      functionDeclarationModuleIds_[methodSymbol.get()] =
          moduleIt->second.info->moduleId;
      functionGenericParamNames_[methodSymbol.get()] =
          methodSymbol->genericParameterNames;
      classInfo.methods[methodDecl->name_] = methodSymbol;
      if (isCtor) {
        classInfo.constructor = methodSymbol;
      } else if (isDtor) {
        classInfo.destructor = methodSymbol;
      }

      if (!methodSymbol->genericParameterNames.empty() &&
          !methodSymbol->isGenericInstantiation) {
        continue;
      }

      auto oldScope = currentScope_;
      auto oldFunction = currentFunction_;
      auto oldModuleId = currentModuleId_;
      auto oldClassStack = currentClassStack_;
      int oldUnsafeDepth = unsafeDepth_;

      currentModuleId_ = moduleIt->second.info->moduleId;
      currentScope_ = moduleIt->second.scope;
      currentFunction_ = methodSymbol;
      currentClassStack_.push_back(instantiatedClassType->getName());
      if (methodSymbol->isUnsafe) {
        ++unsafeDepth_;
      }

      pushScope();
      for (const auto &param : methodSymbol->parameters) {
        if (!currentScope_->declare(param->name, param)) {
          error(methodDecl->span,
                "Parameter '" + param->name + "' already declared.");
        }
      }

      activeGenericBindingsStack_.push_back(genericBindings);
      auto boundBody = bindBody(methodDecl->body_.get(), false);
      activeGenericBindingsStack_.pop_back();
      popScope();

      currentScope_ = oldScope;
      currentFunction_ = oldFunction;
      currentModuleId_ = oldModuleId;
      currentClassStack_ = oldClassStack;
      unsafeDepth_ = oldUnsafeDepth;

      bool hasReturn = false;
      if (boundBody) {
        if (boundBody->result)
          hasReturn = true;
        for (const auto &stmt : boundBody->statements) {
          if (dynamic_cast<BoundReturnStatement *>(stmt.get())) {
            hasReturn = true;
            break;
          }
        }
      }

      if (!hasReturn &&
          methodSymbol->returnType->getKind() != zir::TypeKind::Void) {
        auto kind = methodSymbol->returnType->getKind();
        if (methodSymbol->returnType->isInteger() ||
            kind == zir::TypeKind::Float || kind == zir::TypeKind::Bool) {
          std::string litVal = "0";
          if (kind == zir::TypeKind::Float)
            litVal = "0.0";
          else if (kind == zir::TypeKind::Bool)
            litVal = "false";
          auto lit =
              std::make_unique<BoundLiteral>(litVal, methodSymbol->returnType);
          boundBody->statements.push_back(
              std::make_unique<BoundReturnStatement>(std::move(lit)));
        }

        _diag.report(methodDecl->span, zap::DiagnosticLevel::Warning,
                     "Function '" + methodDecl->name_ +
                         "' has non-void return type but no return on some "
                         "paths.");
      }

      boundRoot_->functions.push_back(
          std::make_unique<BoundFunctionDeclaration>(methodSymbol,
                                                     std::move(boundBody)));
    }

    currentScope_ = oldScope;
    currentModuleId_ = oldModuleId;

    auto boundRecord = std::make_unique<BoundRecordDeclaration>();
    boundRecord->type = instantiatedClassType;
    boundRoot_->records.push_back(std::move(boundRecord));
  }

  return instantiatedSymbol;
}

std::shared_ptr<zir::Type> Binder::mapTypeWithGenericBindings(
    const TypeNode &typeNode,
    const std::unordered_map<std::string, std::shared_ptr<zir::Type>>
        &genericBindings) {
  auto mapped = mapType(typeNode);
  return substituteGenericType(std::move(mapped), genericBindings);
}

std::unordered_map<std::string, std::shared_ptr<zir::Type>>
Binder::buildGenericBindings(
    const FunctionSymbol &function,
    const std::vector<std::unique_ptr<BoundExpression>> &arguments,
    const std::vector<std::unique_ptr<TypeNode>> &explicitTypeArgs,
    SourceSpan callSpan,
    std::string *failureReason) {
  (void)callSpan;
  std::unordered_map<std::string, std::shared_ptr<zir::Type>> bindings;

  if (function.genericParameterNames.empty()) {
    return bindings;
  }

  if (explicitTypeArgs.size() > function.genericParameterNames.size()) {
    if (failureReason) {
      *failureReason = "explicit generic argument count mismatch";
    }
    return {};
  }

  for (size_t i = 0; i < explicitTypeArgs.size(); ++i) {
    if (!explicitTypeArgs[i]) {
      continue;
    }
    auto mapped = mapType(*explicitTypeArgs[i]);
    if (!mapped) {
      if (failureReason) {
        *failureReason = "unknown generic type argument";
      }
      return {};
    }
    bindings[function.genericParameterNames[i]] = mapped;
  }

  auto declIt = functionDeclarationNodes_.find(&function);
  const auto *decl = declIt == functionDeclarationNodes_.end() ? nullptr
                                                               : declIt->second;

  std::function<bool(const std::shared_ptr<zir::Type> &,
                     const std::shared_ptr<zir::Type> &)>
      inferFrom =
          [&](const std::shared_ptr<zir::Type> &paramType,
              const std::shared_ptr<zir::Type> &argType) -> bool {
    if (!paramType || !argType) {
      return true;
    }

    if (paramType->getKind() == zir::TypeKind::Record) {
      auto rec = std::static_pointer_cast<zir::RecordType>(paramType);
      if (rec->isGenericInstance() && argType->getKind() == zir::TypeKind::Record) {
        auto argRecord = std::static_pointer_cast<zir::RecordType>(argType);
        if (!argRecord->isGenericInstance() ||
            rec->getGenericBaseName() != argRecord->getGenericBaseName() ||
            rec->getGenericArguments().size() !=
                argRecord->getGenericArguments().size()) {
          return false;
        }
        for (size_t i = 0; i < rec->getGenericArguments().size(); ++i) {
          if (!inferFrom(rec->getGenericArguments()[i],
                         argRecord->getGenericArguments()[i])) {
            return false;
          }
        }
        return true;
      }
      auto paramName = rec->getName();
      auto isGenericName = std::find(function.genericParameterNames.begin(),
                                     function.genericParameterNames.end(),
                                     paramName) !=
                           function.genericParameterNames.end();
      if (isGenericName) {
        auto it = bindings.find(paramName);
        if (it == bindings.end()) {
          bindings[paramName] = argType;
          return true;
        }
        return it->second->toString() == argType->toString();
      }
      return true;
    }

    if (paramType->getKind() == zir::TypeKind::Class) {
      auto cls = std::static_pointer_cast<zir::ClassType>(paramType);
      if (cls->isGenericInstance() && argType->getKind() == zir::TypeKind::Class) {
        auto argClass = std::static_pointer_cast<zir::ClassType>(argType);
        std::shared_ptr<zir::ClassType> matchingArgClass = argClass;
        while (matchingArgClass &&
               (!matchingArgClass->isGenericInstance() ||
                cls->getGenericBaseName() !=
                    matchingArgClass->getGenericBaseName())) {
          matchingArgClass = matchingArgClass->getBase();
        }
        if (!matchingArgClass ||
            matchingArgClass->getGenericArguments().size() !=
                cls->getGenericArguments().size()) {
          return false;
        }
        for (size_t i = 0; i < cls->getGenericArguments().size(); ++i) {
          if (!inferFrom(cls->getGenericArguments()[i],
                         matchingArgClass->getGenericArguments()[i])) {
            return false;
          }
        }
        return true;
      }
      auto paramName = cls->getName();
      auto isGenericName = std::find(function.genericParameterNames.begin(),
                                     function.genericParameterNames.end(),
                                     paramName) !=
                           function.genericParameterNames.end();
      if (isGenericName) {
        auto it = bindings.find(paramName);
        if (it == bindings.end()) {
          bindings[paramName] = argType;
          return true;
        }
        return it->second->toString() == argType->toString();
      }
      return true;
    }

    if (paramType->getKind() == zir::TypeKind::Pointer &&
        argType->getKind() == zir::TypeKind::Pointer) {
      auto pp = std::static_pointer_cast<zir::PointerType>(paramType);
      auto ap = std::static_pointer_cast<zir::PointerType>(argType);
      return inferFrom(pp->getBaseType(), ap->getBaseType());
    }

    if (paramType->getKind() == zir::TypeKind::Array &&
        argType->getKind() == zir::TypeKind::Array) {
      auto pa = std::static_pointer_cast<zir::ArrayType>(paramType);
      auto aa = std::static_pointer_cast<zir::ArrayType>(argType);
      if (pa->getSize() != aa->getSize()) {
        return false;
      }
      return inferFrom(pa->getBaseType(), aa->getBaseType());
    }

    return true;
          };

  size_t fixedCount = function.fixedParameterCount();
  for (size_t i = 0; i < arguments.size() && i < fixedCount; ++i) {
    if (!inferFrom(function.parameters[i]->type, arguments[i]->type)) {
      if (failureReason) {
        *failureReason = "conflicting generic type inference";
      }
      return {};
    }
  }

  for (const auto &name : function.genericParameterNames) {
    if (bindings.find(name) == bindings.end()) {
      bool filledFromDefault = false;
      if (decl) {
        auto paramIndex = static_cast<size_t>(
            std::distance(function.genericParameterNames.begin(),
                          std::find(function.genericParameterNames.begin(),
                                    function.genericParameterNames.end(),
                                    name)));
        if (paramIndex < decl->genericParams_.size() &&
            decl->genericParams_[paramIndex] &&
            decl->genericParams_[paramIndex]->defaultType) {
          activeGenericBindingsStack_.push_back(bindings);
          auto mapped = mapType(*decl->genericParams_[paramIndex]->defaultType);
          activeGenericBindingsStack_.pop_back();
          if (mapped) {
            bindings[name] = mapped;
            filledFromDefault = true;
          }
        }
      }
      if (!filledFromDefault) {
        if (failureReason) {
          *failureReason = "cannot infer generic type parameter '" + name + "'";
        }
        return {};
      }
    }
  }

  if (decl) {
    if (!validateGenericConstraints(decl->genericConstraints_, bindings,
                                    failureReason)) {
      return {};
    }
  }

  return bindings;
}

std::unique_ptr<BoundExpression>
Binder::bindExpressionWithExpected(ExpressionNode *expr,
                                   std::shared_ptr<zir::Type> expectedType) {
  if (!expr) {
    return nullptr;
  }

  expectedExpressionTypes_.push_back(std::move(expectedType));
  expr->accept(*this);
  expectedExpressionTypes_.pop_back();

  if (expressionStack_.empty()) {
    return nullptr;
  }

  auto boundExpr = std::move(expressionStack_.top());
  expressionStack_.pop();
  return boundExpr;
}

std::shared_ptr<zir::Type> Binder::currentExpectedExpressionType() const {
  if (expectedExpressionTypes_.empty()) {
    return nullptr;
  }
  return expectedExpressionTypes_.back();
}

bool Binder::isSignedIntegerType(std::shared_ptr<zir::Type> type) const {
  if (!type) {
    return false;
  }

  switch (type->getKind()) {
  case zir::TypeKind::Int8:
  case zir::TypeKind::Int16:
  case zir::TypeKind::Int32:
  case zir::TypeKind::Int64:
  case zir::TypeKind::Int:
    return true;
  default:
    return false;
  }
}

bool Binder::isUnsignedIntegerType(std::shared_ptr<zir::Type> type) const {
  return type && type->isInteger() && type->isUnsigned();
}

int Binder::typeBitWidth(std::shared_ptr<zir::Type> type) const {
  if (!type) {
    return 0;
  }

  switch (type->getKind()) {
  case zir::TypeKind::Bool:
    return 1;
  case zir::TypeKind::Char:
  case zir::TypeKind::Int8:
  case zir::TypeKind::UInt8:
    return 8;
  case zir::TypeKind::Int16:
  case zir::TypeKind::UInt16:
    return 16;
  case zir::TypeKind::Int32:
  case zir::TypeKind::UInt32:
  case zir::TypeKind::Int:
  case zir::TypeKind::UInt:
  case zir::TypeKind::Float:
  case zir::TypeKind::Float32:
    return 32;
  case zir::TypeKind::Int64:
  case zir::TypeKind::UInt64:
  case zir::TypeKind::Float64:
    return 64;
  default:
    return 0;
  }
}

int Binder::conversionCost(std::shared_ptr<zir::Type> from,
                           std::shared_ptr<zir::Type> to) const {
  if (!from || !to) {
    return 1000;
  }
  if (from->toString() == to->toString()) {
    return 0;
  }
  if (isNullType(from) && isPointerType(to)) {
    return 3;
  }
  if (!canConvert(from, to)) {
    return 1000;
  }

  if (from->isFloatingPoint() && to->isFloatingPoint()) {
    return typeBitWidth(to) >= typeBitWidth(from) ? 1 : 5;
  }

  if (isSignedIntegerType(from) && isSignedIntegerType(to)) {
    return typeBitWidth(to) >= typeBitWidth(from) ? 1 : 4;
  }

  if (isUnsignedIntegerType(from) && isUnsignedIntegerType(to)) {
    return typeBitWidth(to) >= typeBitWidth(from) ? 1 : 4;
  }

  if (from->isInteger() && to->isFloatingPoint()) {
    return typeBitWidth(to) >= typeBitWidth(from) ? 2 : 5;
  }

  if (from->isFloatingPoint() && to->isInteger()) {
    return 6;
  }

  if (from->isInteger() && to->isInteger()) {
    return 5;
  }

  return 7;
}

std::string Binder::describeConversion(std::shared_ptr<zir::Type> from,
                                       std::shared_ptr<zir::Type> to) const {
  if (!from || !to) {
    return "invalid conversion";
  }
  int cost = conversionCost(from, to);
  if (cost == 0) {
    return "exact match";
  }
  if (isNullType(from) &&
      (isPointerType(to) || to->getKind() == zir::TypeKind::Class)) {
    return "null-to-reference conversion";
  }
  if (from->isFloatingPoint() && to->isFloatingPoint()) {
    return typeBitWidth(to) >= typeBitWidth(from) ? "floating widening"
                                                  : "floating narrowing";
  }
  if (isSignedIntegerType(from) && isSignedIntegerType(to)) {
    return typeBitWidth(to) >= typeBitWidth(from) ? "signed widening"
                                                  : "signed narrowing";
  }
  if (isUnsignedIntegerType(from) && isUnsignedIntegerType(to)) {
    return typeBitWidth(to) >= typeBitWidth(from) ? "unsigned widening"
                                                  : "unsigned narrowing";
  }
  if (from->isInteger() && to->isFloatingPoint()) {
    return "integer-to-float conversion";
  }
  if (from->isFloatingPoint() && to->isInteger()) {
    return "float-to-integer conversion";
  }
  if (from->isInteger() && to->isInteger()) {
    return "signedness-changing integer conversion";
  }
  return cost >= 1000 ? "not convertible" : "implicit conversion";
}

std::string Binder::displayTypeName(const std::string &moduleName,
                                    const std::string &name) const {
  if (moduleName.empty() || moduleName == "__single_module__") {
    return name;
  }
  return moduleName + "." + name;
}

std::string
Binder::renderTypeForUser(const std::shared_ptr<zir::Type> &type) const {
  if (!type) {
    return "<unknown>";
  }

  switch (type->getKind()) {
  case zir::TypeKind::Void:
    return "Void";
  case zir::TypeKind::Bool:
    return "Bool";
  case zir::TypeKind::Char:
    return "Char";
  case zir::TypeKind::Int:
    return "Int";
  case zir::TypeKind::Int8:
    return "Int8";
  case zir::TypeKind::Int16:
    return "Int16";
  case zir::TypeKind::Int32:
    return "Int32";
  case zir::TypeKind::Int64:
    return "Int64";
  case zir::TypeKind::UInt:
    return "UInt";
  case zir::TypeKind::UInt8:
    return "UInt8";
  case zir::TypeKind::UInt16:
    return "UInt16";
  case zir::TypeKind::UInt32:
    return "UInt32";
  case zir::TypeKind::UInt64:
    return "UInt64";
  case zir::TypeKind::Float:
    return "Float";
  case zir::TypeKind::Float32:
    return "Float32";
  case zir::TypeKind::Float64:
    return "Float64";
  case zir::TypeKind::NullPtr:
    return "null";
  case zir::TypeKind::Pointer: {
    auto ptr = std::static_pointer_cast<zir::PointerType>(type);
    return "*" + renderTypeForUser(ptr->getBaseType());
  }
  case zir::TypeKind::Array: {
    auto arr = std::static_pointer_cast<zir::ArrayType>(type);
    return "[" + std::to_string(arr->getSize()) + "]" +
           renderTypeForUser(arr->getBaseType());
  }
  case zir::TypeKind::Record: {
    auto rec = std::static_pointer_cast<zir::RecordType>(type);
    return rec->getName();
  }
  case zir::TypeKind::Class: {
    auto cls = std::static_pointer_cast<zir::ClassType>(type);
    return std::string(cls->isWeak() ? "weak " : "") + cls->getName();
  }
  case zir::TypeKind::Enum: {
    auto en = std::static_pointer_cast<zir::EnumType>(type);
    return en->getName();
  }
  }

  return type->toString();
}

std::string Binder::currentModuleLinkPath() const {
  auto it = modules_.find(currentModuleId_);
  if (it == modules_.end() || !it->second.info) {
    return currentModuleId_;
  }
  return it->second.info->linkPath.empty() ? it->second.info->moduleId
                                           : it->second.info->linkPath;
}

void Binder::predeclareModuleTypes(ModuleState &module) {
  currentModuleId_ = module.info->moduleId;
  currentScope_ = module.scope;

  for (const auto &child : module.info->root->children) {
    if (auto recordDecl = dynamic_cast<RecordDecl *>(child.get())) {
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
      for (const auto &genericParam : recordDecl->genericParams_) {
        if (genericParam) {
          symbol->genericParameterNames.push_back(genericParam->typeName);
        }
      }
      recordTypeDeclarationNodes_[symbol.get()] = recordDecl;
      typeDeclarationModuleIds_[symbol.get()] = module.info->moduleId;
      if (!module.scope->declare(recordDecl->name_, symbol)) {
        error(recordDecl->span,
              "Type '" + recordDecl->name_ + "' already declared.");
      }
      module.symbol->members[recordDecl->name_] = symbol;
      if (recordDecl->visibility_ == Visibility::Public) {
        module.symbol->exports[recordDecl->name_] = symbol;
      }
    } else if (auto classDecl = dynamic_cast<ClassDecl *>(child.get())) {
      auto type = std::make_shared<zir::ClassType>(
          displayTypeName(module.info->moduleName, classDecl->name_),
          mangleName(module.info->linkPath.empty() ? module.info->moduleId
                                                   : module.info->linkPath,
                     classDecl->name_));
      auto symbol = std::make_shared<TypeSymbol>(
          classDecl->name_, type,
          mangleName(module.info->linkPath.empty() ? module.info->moduleId
                                                   : module.info->linkPath,
                     classDecl->name_),
          module.info->moduleName, classDecl->visibility_, false, true);
      for (const auto &genericParam : classDecl->genericParams_) {
        if (genericParam) {
          symbol->genericParameterNames.push_back(genericParam->typeName);
        }
      }
      classTypeDeclarationNodes_[symbol.get()] = classDecl;
      typeDeclarationModuleIds_[symbol.get()] = module.info->moduleId;
      if (!module.scope->declare(classDecl->name_, symbol)) {
        error(classDecl->span,
              "Type '" + classDecl->name_ + "' already declared.");
      }
      module.symbol->members[classDecl->name_] = symbol;
      if (classDecl->visibility_ == Visibility::Public) {
        module.symbol->exports[classDecl->name_] = symbol;
      }

      if (symbol->genericParameterNames.empty()) {
        ClassInfo info;
        info.typeSymbol = symbol;
        info.classType = type;
        info.ownerQualifiedName = type->getName();
        classInfos_[type->getName()] = info;
      }
    } else if (auto structDecl =
                   dynamic_cast<StructDeclarationNode *>(child.get())) {
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
      for (const auto &genericParam : structDecl->genericParams_) {
        if (genericParam) {
          symbol->genericParameterNames.push_back(genericParam->typeName);
        }
      }
      structTypeDeclarationNodes_[symbol.get()] = structDecl;
      typeDeclarationModuleIds_[symbol.get()] = module.info->moduleId;
      if (!module.scope->declare(structDecl->name_, symbol)) {
        error(structDecl->span,
              "Type '" + structDecl->name_ + "' already declared.");
      }
      module.symbol->members[structDecl->name_] = symbol;
      if (structDecl->visibility_ == Visibility::Public) {
        module.symbol->exports[structDecl->name_] = symbol;
      }
    } else if (auto enumDecl = dynamic_cast<EnumDecl *>(child.get())) {
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
      if (!module.scope->declare(enumDecl->name_, symbol)) {
        error(enumDecl->span,
              "Type '" + enumDecl->name_ + "' already declared.");
      }
      module.symbol->members[enumDecl->name_] = symbol;
      if (enumDecl->visibility_ == Visibility::Public) {
        module.symbol->exports[enumDecl->name_] = symbol;
      }
    }
  }
}

void Binder::predeclareModuleAliases(ModuleState &module) {
  currentModuleId_ = module.info->moduleId;
  currentScope_ = module.scope;

  for (const auto &child : module.info->root->children) {
    if (auto aliasDecl = dynamic_cast<TypeAliasDecl *>(child.get())) {
      auto type = mapType(*aliasDecl->type_);
      if (!type) {
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
      if (!module.scope->declare(aliasDecl->name_, symbol)) {
        error(aliasDecl->span,
              "Type '" + aliasDecl->name_ + "' already declared.");
      }
      module.symbol->members[aliasDecl->name_] = symbol;
      if (aliasDecl->visibility_ == Visibility::Public) {
        module.symbol->exports[aliasDecl->name_] = symbol;
      }
    }
  }
}

void Binder::applyImports(ModuleState &module, bool allowIncomplete) {
  std::map<std::string, std::string> namespaceOwners;

  for (const auto &import : module.info->imports) {
    if (!import.moduleAlias.empty() && import.targetModuleIds.size() != 1) {
      if (!allowIncomplete) {
        error(import.span, "Module alias imports are only allowed when the "
                           "path resolves to a single module.");
      }
      continue;
    }

    for (const auto &targetId : import.targetModuleIds) {
      auto targetIt = modules_.find(targetId);
      if (targetIt == modules_.end()) {
        continue;
      }

      auto &target = targetIt->second;
      auto alias = import.moduleAlias.empty() ? target.info->moduleName
                                              : import.moduleAlias;

      auto existingOwner = namespaceOwners.find(alias);
      if (existingOwner != namespaceOwners.end() &&
          existingOwner->second != targetId) {
        if (!allowIncomplete) {
          error(import.span, "Import namespace '" + alias +
                                 "' is ambiguous because multiple files share "
                                 "that module name.");
        }
        continue;
      }

      if (!module.scope->lookupLocal(alias)) {
        module.scope->declare(alias, target.symbol);
        namespaceOwners[alias] = targetId;
      } else if (module.scope->lookupLocal(alias) != target.symbol) {
        if (!allowIncomplete) {
          error(import.span, "Cannot import module '" + alias +
                                 "' because that name is already declared in "
                                 "the current file.");
        }
        continue;
      }

      if (import.bindings.empty()) {
        if (import.visibility == Visibility::Public) {
          module.symbol->exports[alias] = target.symbol;
          for (const auto &exported : target.symbol->exports) {
            module.symbol->exports[exported.first] = exported.second;
          }
        }
        continue;
      }

      if (import.targetModuleIds.size() != 1) {
        if (!allowIncomplete) {
          error(import.span, "Selective imports are only allowed when the path "
                             "resolves to a single module.");
        }
        continue;
      }

      for (const auto &binding : import.bindings) {
        auto exportedIt = target.symbol->exports.find(binding.sourceName);
        if (exportedIt == target.symbol->exports.end()) {
          if (allowIncomplete) {
            continue;
          }
          auto memberIt = target.symbol->members.find(binding.sourceName);
          if (memberIt != target.symbol->members.end() &&
              memberIt->second->visibility != Visibility::Public) {
            error(import.span, "Member '" + binding.sourceName +
                                   "' of module '" + alias + "' is private.");
          } else {
            error(import.span, "Module '" + alias + "' has no public member '" +
                                   binding.sourceName + "'.");
          }
          continue;
        }

        auto existing = module.scope->lookupLocal(binding.localName);
        if (existing && existing != exportedIt->second) {
          if (!allowIncomplete) {
            error(import.span, "Imported name '" + binding.localName +
                                   "' conflicts with an existing declaration "
                                   "in the current file.");
          }
          continue;
        }
        if (!existing) {
          module.scope->declare(binding.localName, exportedIt->second);
        }
        if (import.visibility == Visibility::Public) {
          module.symbol->exports[binding.localName] = exportedIt->second;
        }
      }
    }
  }
}

void Binder::ensureModuleValuesReady(ModuleState &module) {
  if (module.finalImportsApplied) {
    return;
  }
  if (module.valuesPreparationInProgress) {
    return;
  }

  module.valuesPreparationInProgress = true;

  for (const auto &import : module.info->imports) {
    for (const auto &targetId : import.targetModuleIds) {
      auto targetIt = modules_.find(targetId);
      if (targetIt != modules_.end()) {
        ensureModuleValuesReady(targetIt->second);
      }
    }
  }

  if (!module.valuesPredeclared) {
    predeclareModuleValues(module);
    module.valuesPredeclared = true;
  }

  if (!module.finalImportsApplied) {
    applyImports(module, false);
    module.finalImportsApplied = true;
  }

  module.valuesPreparationInProgress = false;
}

void Binder::predeclareModuleValues(ModuleState &module) {
  currentModuleId_ = module.info->moduleId;
  currentScope_ = module.scope;

  for (const auto &child : module.info->root->children) {
    if (auto funDecl = dynamic_cast<FunDecl *>(child.get())) {
      if (funDecl->isUnsafe_) {
        requireUnsafeEnabled(funDecl->span, "'unsafe fun'");
        ++unsafeTypeContextDepth_;
      }

      std::unordered_map<std::string, std::shared_ptr<zir::Type>> genericBindings;
      for (const auto &genericParam : funDecl->genericParams_) {
        if (!genericParam) {
          continue;
        }
        auto placeholder = std::make_shared<zir::RecordType>(
            genericParam->typeName, genericParam->typeName);
        genericBindings[genericParam->typeName] = placeholder;
      }

      if (!genericBindings.empty()) {
        activeGenericBindingsStack_.push_back(genericBindings);
      }

      std::vector<std::shared_ptr<VariableSymbol>> params;
      for (size_t i = 0; i < funDecl->params_.size(); ++i) {
        const auto &p = funDecl->params_[i];
        if (p->isVariadic && i + 1 != funDecl->params_.size()) {
          error(p->span, "Variadic parameter must be the last parameter.");
        }
        if (p->isVariadic && p->isRef) {
          error(p->span, "Variadic parameter cannot be passed by 'ref'.");
        }
        auto mappedType = mapType(*p->type);
        if (!mappedType) {
          error(p->span, "Unknown type: " + p->type->qualifiedName());
          mappedType =
              std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
        }
        auto symbol = std::make_shared<VariableSymbol>(
            p->name, mappedType, false, p->isRef, p->name,
            module.info->moduleName, Visibility::Private);
        if (p->isVariadic) {
          symbol->is_variadic_pack = true;
          symbol->variadic_element_type = mappedType;
          symbol->type = makeVariadicViewType(mappedType);
        }
        params.push_back(std::move(symbol));
      }

      std::shared_ptr<zir::Type> retType = nullptr;
      if (funDecl->returnType_) {
        retType = mapType(*funDecl->returnType_);
      } else if (funDecl->name_ == "main" && module.info->isEntry) {
        retType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int);
      } else {
        retType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
      }

      if (!genericBindings.empty()) {
        activeGenericBindingsStack_.pop_back();
      }

      if (!retType) {
        error(funDecl->span,
              "Unknown return type in function '" + funDecl->name_ + "'.");
        retType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
      }

      if (funDecl->isUnsafe_) {
        --unsafeTypeContextDepth_;
      }

      auto linkName = (funDecl->name_ == "main" && module.info->isEntry)
                          ? std::string("main")
                          : std::string();
      auto symbol = std::make_shared<FunctionSymbol>(
          funDecl->name_, std::move(params), std::move(retType), "",
          module.info->moduleName, funDecl->visibility_, funDecl->isUnsafe_);
      for (const auto &genericParam : funDecl->genericParams_) {
        if (genericParam) {
          symbol->genericParameterNames.push_back(genericParam->typeName);
        }
      }
      functionGenericParamNames_[symbol.get()] = symbol->genericParameterNames;
      functionDeclarationNodes_[symbol.get()] = funDecl;
      functionDeclarationModuleIds_[symbol.get()] = module.info->moduleId;
      if (linkName != "main") {
        symbol->linkName = mangleFunctionName(module.info->linkPath.empty()
                                                  ? module.info->moduleId
                                                  : module.info->linkPath,
                                              *symbol);
      } else {
        symbol->linkName = linkName;
      }

      auto existing = module.scope->lookupLocal(funDecl->name_);
      if (findFunctionBySignature(existing, *symbol)) {
        error(funDecl->span,
              "Function '" + funDecl->name_ + "' already declared.");
        continue;
      }
      auto overloads = module.scope->declareFunction(funDecl->name_, symbol);
      if (!overloads) {
        error(funDecl->span, "Function '" + funDecl->name_ +
                                 "' conflicts with an existing declaration.");
        continue;
      }
      declaredFunctionSymbols_[funDecl] = symbol;
      module.symbol->members[funDecl->name_] = overloads;
      if (funDecl->visibility_ == Visibility::Public) {
        auto exportIt = module.symbol->exports.find(funDecl->name_);
        std::shared_ptr<OverloadSetSymbol> exportSet;
        if (exportIt == module.symbol->exports.end()) {
          exportSet = std::make_shared<OverloadSetSymbol>(
              funDecl->name_, module.info->moduleName, Visibility::Public);
          module.symbol->exports[funDecl->name_] = exportSet;
        } else {
          exportSet =
              std::dynamic_pointer_cast<OverloadSetSymbol>(exportIt->second);
        }
        if (!exportSet) {
          error(funDecl->span, "Function '" + funDecl->name_ +
                                   "' conflicts with an exported declaration.");
        } else {
          exportSet->addOverload(symbol);
        }
      }
    } else if (auto classDecl = dynamic_cast<ClassDecl *>(child.get())) {
      auto classSymbol = std::dynamic_pointer_cast<TypeSymbol>(
          module.scope->lookup(classDecl->name_));
      if (!classSymbol || !classSymbol->isClass ||
          !classSymbol->genericParameterNames.empty()) {
        continue;
      }
      auto classType =
          std::static_pointer_cast<zir::ClassType>(classSymbol->type);
      auto &classInfo = classInfos_[classType->getName()];

      if (classDecl->baseType_) {
        bool hasOwnCtor = false;
        bool hasOwnDtor = false;
        for (const auto &methodDecl : classDecl->methods_) {
          hasOwnCtor = hasOwnCtor || methodDecl->name_ == "init";
          hasOwnDtor = hasOwnDtor || methodDecl->name_ == "deinit";
        }
        auto baseType = mapType(*classDecl->baseType_);
        if (baseType && baseType->getKind() == zir::TypeKind::Class) {
          auto baseClass = std::static_pointer_cast<zir::ClassType>(baseType);
          auto baseIt = classInfos_.find(baseClass->getName());
          if (baseIt != classInfos_.end()) {
            if (!hasOwnCtor) {
              classInfo.constructor = baseIt->second.constructor;
            }
            if (!hasOwnDtor) {
              classInfo.destructor = baseIt->second.destructor;
            }
            classInfo.methods.insert(baseIt->second.methods.begin(),
                                     baseIt->second.methods.end());
            classInfo.nextVirtualSlot = baseIt->second.nextVirtualSlot;
          }
        }
      }

      for (const auto &methodDecl : classDecl->methods_) {
        if (methodDecl->isUnsafe_) {
          requireUnsafeEnabled(methodDecl->span, "'unsafe fun'");
          ++unsafeTypeContextDepth_;
        }

        std::unordered_map<std::string, std::shared_ptr<zir::Type>>
            methodGenericBindings;
        for (const auto &genericParam : methodDecl->genericParams_) {
          if (genericParam) {
            auto placeholder = std::make_shared<zir::RecordType>(
                genericParam->typeName, genericParam->typeName);
            methodGenericBindings[genericParam->typeName] = placeholder;
          }
        }
        if (!methodGenericBindings.empty()) {
          activeGenericBindingsStack_.push_back(methodGenericBindings);
        }

        std::vector<std::shared_ptr<VariableSymbol>> params;
        if (!methodDecl->isStatic_) {
          params.push_back(std::make_shared<VariableSymbol>(
              "self", classType, false, false, "self", module.info->moduleName,
              Visibility::Private));
        }

        for (size_t i = 0; i < methodDecl->params_.size(); ++i) {
          const auto &p = methodDecl->params_[i];
          auto mappedType = mapType(*p->type);
          if (!mappedType) {
            error(p->span, "Unknown type: " + p->type->qualifiedName());
            mappedType =
                std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
          }
          params.push_back(std::make_shared<VariableSymbol>(
              p->name, mappedType, false, p->isRef, p->name,
              module.info->moduleName, Visibility::Private));
        }

        std::shared_ptr<zir::Type> retType;
        bool isCtor = methodDecl->name_ == "init";
        bool isDtor = methodDecl->name_ == "deinit";
        if (isDtor &&
            (!methodDecl->params_.empty() || methodDecl->returnType_)) {
          error(methodDecl->span,
                "Destructor 'deinit' cannot have parameters or a return type.");
        }
        if (isCtor) {
          retType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
        } else if (isDtor) {
          retType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
        } else if (methodDecl->returnType_) {
          retType = mapType(*methodDecl->returnType_);
        } else {
          retType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
        }
        if (!methodGenericBindings.empty()) {
          activeGenericBindingsStack_.pop_back();
        }
        if (!retType) {
          retType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
        }

        if (methodDecl->isUnsafe_) {
          --unsafeTypeContextDepth_;
        }

        auto symbol = std::make_shared<FunctionSymbol>(
            methodDecl->name_, std::move(params), std::move(retType), "",
            module.info->moduleName, methodDecl->visibility_,
            methodDecl->isUnsafe_);
        for (const auto &genericParam : methodDecl->genericParams_) {
          if (genericParam) {
            symbol->genericParameterNames.push_back(genericParam->typeName);
          }
        }
        symbol->isMethod = !methodDecl->isStatic_;
        symbol->isStatic = methodDecl->isStatic_;
        symbol->isConstructor = isCtor;
        symbol->isDestructor = isDtor;
        symbol->ownerTypeName = classType->getName();
        if (symbol->isMethod && !symbol->isStatic && !symbol->isConstructor &&
            !symbol->isDestructor) {
          auto existingIt = classInfo.methods.find(methodDecl->name_);
          if (existingIt != classInfo.methods.end()) {
            auto existingMethod =
                std::dynamic_pointer_cast<FunctionSymbol>(existingIt->second);
            if (existingMethod && existingMethod->vtableSlot >= 0) {
              symbol->vtableSlot = existingMethod->vtableSlot;
            }
          }
          if (symbol->vtableSlot < 0) {
            symbol->vtableSlot = classInfo.nextVirtualSlot++;
          }
        }
        symbol->linkName =
            mangleName(module.info->linkPath.empty() ? module.info->moduleId
                                                     : module.info->linkPath,
                       classDecl->name_ + "$" + methodDecl->name_ + "$" +
                           sanitizeTypeName(functionSignatureKey(*symbol)));
        declaredFunctionSymbols_[methodDecl.get()] = symbol;
        functionDeclarationNodes_[symbol.get()] = methodDecl.get();
        functionDeclarationModuleIds_[symbol.get()] = module.info->moduleId;
        functionGenericParamNames_[symbol.get()] = symbol->genericParameterNames;
        classInfo.methods[methodDecl->name_] = symbol;
        if (isCtor) {
          classInfo.constructor = symbol;
        } else if (isDtor) {
          classInfo.destructor = symbol;
        }
      }
    } else if (auto extDecl = dynamic_cast<ExtDecl *>(child.get())) {
      ++externTypeContextDepth_;
      std::vector<std::shared_ptr<VariableSymbol>> params;
      for (const auto &p : extDecl->params_) {
        if (p->isVariadic) {
          error(p->span, "Variadic parameters are only supported in Zap "
                         "function declarations.");
        }
        auto mappedType = mapType(*p->type);
        if (!mappedType) {
          error(p->span, "Unknown type: " + p->type->qualifiedName());
          mappedType =
              std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
        }
        params.push_back(std::make_shared<VariableSymbol>(
            p->name, mappedType, false, p->isRef, p->name,
            module.info->moduleName, Visibility::Private));
      }

      auto retType =
          extDecl->returnType_
              ? mapType(*extDecl->returnType_)
              : std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
      if (!retType) {
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
      if (isStdFsModule && extDecl->name_ == "mkdir") {
        linkName = "zap_fs_mkdir";
      } else if (isStdFsModule && extDecl->name_ == "remove") {
        linkName = "zap_fs_remove";
      } else if (isStdFsModule && extDecl->name_ == "rename") {
        linkName = "zap_fs_rename";
      } else if (isStdIoModule && extDecl->name_ == "printf") {
        linkName = "zap_printf";
      } else if (isStdIoModule && extDecl->name_ == "printfln") {
        linkName = "zap_printfln";
      } else if (isStdPathModule && extDecl->name_ == "basename") {
        linkName = "zap_path_basename";
      }

      auto symbol = std::make_shared<FunctionSymbol>(
          extDecl->name_, std::move(params), std::move(retType), linkName,
          module.info->moduleName, extDecl->visibility_, false,
          extDecl->isCVariadic_);
      auto existing = module.scope->lookupLocal(extDecl->name_);
      if (findFunctionBySignature(existing, *symbol)) {
        error(extDecl->span,
              "External function '" + extDecl->name_ + "' already declared.");
        continue;
      }
      auto overloads = module.scope->declareFunction(extDecl->name_, symbol);
      if (!overloads) {
        error(extDecl->span, "External function '" + extDecl->name_ +
                                 "' conflicts with an existing declaration.");
        continue;
      }
      declaredFunctionSymbols_[extDecl] = symbol;
      module.symbol->members[extDecl->name_] = overloads;
      if (extDecl->visibility_ == Visibility::Public) {
        auto exportIt = module.symbol->exports.find(extDecl->name_);
        std::shared_ptr<OverloadSetSymbol> exportSet;
        if (exportIt == module.symbol->exports.end()) {
          exportSet = std::make_shared<OverloadSetSymbol>(
              extDecl->name_, module.info->moduleName, Visibility::Public);
          module.symbol->exports[extDecl->name_] = exportSet;
        } else {
          exportSet =
              std::dynamic_pointer_cast<OverloadSetSymbol>(exportIt->second);
        }
        if (!exportSet) {
          error(extDecl->span, "External function '" + extDecl->name_ +
                                   "' conflicts with an exported declaration.");
        } else {
          exportSet->addOverload(symbol);
        }
      }
    } else if (auto varDecl = dynamic_cast<VarDecl *>(child.get())) {
      if (!varDecl->isGlobal_) {
        continue;
      }
      auto type = mapType(*varDecl->type_);
      if (!type) {
        error(varDecl->span,
              "Unknown type: " + varDecl->type_->qualifiedName());
        type = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
      }
      auto symbol = std::make_shared<VariableSymbol>(
          varDecl->name_, type, false, false,
          mangleName(module.info->linkPath.empty() ? module.info->moduleId
                                                   : module.info->linkPath,
                     varDecl->name_),
          module.info->moduleName, varDecl->visibility_);
      if (!module.scope->declare(varDecl->name_, symbol)) {
        error(varDecl->span,
              "Variable '" + varDecl->name_ + "' already declared.");
      }
      module.symbol->members[varDecl->name_] = symbol;
      if (varDecl->visibility_ == Visibility::Public) {
        module.symbol->exports[varDecl->name_] = symbol;
      }
    } else if (auto constDecl = dynamic_cast<ConstDecl *>(child.get())) {
      auto type = mapType(*constDecl->type_);
      if (!type) {
        error(constDecl->span,
              "Unknown type: " + constDecl->type_->qualifiedName());
        type = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
      }
      auto symbol = std::make_shared<VariableSymbol>(
          constDecl->name_, type, true, false,
          mangleName(module.info->linkPath.empty() ? module.info->moduleId
                                                   : module.info->linkPath,
                     constDecl->name_),
          module.info->moduleName, constDecl->visibility_);
      if (!module.scope->declare(constDecl->name_, symbol)) {
        error(constDecl->span,
              "Identifier '" + constDecl->name_ + "' already declared.");
      }
      module.symbol->members[constDecl->name_] = symbol;
      if (constDecl->visibility_ == Visibility::Public) {
        module.symbol->exports[constDecl->name_] = symbol;
      }
    }
  }
}

std::shared_ptr<Symbol>
Binder::lookupVisibleSymbol(const std::string &name) const {
  return currentScope_ ? currentScope_->lookup(name) : nullptr;
}

std::shared_ptr<Symbol>
Binder::resolveModuleMember(const std::string &moduleName,
                            const std::string &memberName, SourceSpan span) {
  auto moduleSym = std::dynamic_pointer_cast<ModuleSymbol>(
      currentScope_->lookup(moduleName));
  if (!moduleSym) {
    error(span, "Undefined module: " + moduleName);
    return nullptr;
  }

  auto exportedIt = moduleSym->exports.find(memberName);
  if (exportedIt != moduleSym->exports.end()) {
    return exportedIt->second;
  }

  auto memberIt = moduleSym->members.find(memberName);
  if (memberIt == moduleSym->members.end()) {
    error(span,
          "Module '" + moduleName + "' has no member '" + memberName + "'.");
    return nullptr;
  }
  error(span, "Member '" + memberName + "' of module '" + moduleName +
                  "' is private.");
  return nullptr;
}

std::shared_ptr<Symbol>
Binder::resolveQualifiedSymbol(const std::vector<std::string> &parts,
                               SourceSpan span, SymbolKind expectedKind,
                               bool allowAnyKind) {
  if (parts.empty()) {
    return nullptr;
  }

  if (parts.size() == 1) {
    auto symbol = lookupVisibleSymbol(parts.front());
    if (!symbol) {
      error(span, "Undefined identifier: " + parts.front());
      return nullptr;
    }
    if (!allowAnyKind && symbol->getKind() != expectedKind &&
        !(expectedKind == SymbolKind::Function &&
          symbol->getKind() == SymbolKind::OverloadSet)) {
      return nullptr;
    }
    return symbol;
  }

  if (parts.size() != 2) {
    error(span, "Only single-level module qualification is supported.");
    return nullptr;
  }

  auto symbol = resolveModuleMember(parts[0], parts[1], span);
  if (!symbol) {
    return nullptr;
  }
  if (!allowAnyKind && symbol->getKind() != expectedKind &&
      !(expectedKind == SymbolKind::Function &&
        symbol->getKind() == SymbolKind::OverloadSet)) {
    return nullptr;
  }
  return symbol;
}

void Binder::visit(RootNode &node) {
  for (const auto &child : node.children) {
    if (dynamic_cast<ImportNode *>(child.get())) {
      continue;
    }
    child->accept(*this);
  }
}

void Binder::visit(ImportNode &node) { (void)node; }

void Binder::visit(ClassDecl &node) {
  auto symbol =
      std::dynamic_pointer_cast<TypeSymbol>(currentScope_->lookup(node.name_));
  if (!symbol || !symbol->isClass || !symbol->genericParameterNames.empty()) {
    return;
  }

  auto classType = std::static_pointer_cast<zir::ClassType>(symbol->type);
  auto &classInfo = classInfos_[classType->getName()];
  currentClassStack_.push_back(classType->getName());

  if (node.baseType_) {
    bool hasOwnCtor = false;
    bool hasOwnDtor = false;
    for (const auto &method : node.methods_) {
      hasOwnCtor = hasOwnCtor || method->name_ == "init";
      hasOwnDtor = hasOwnDtor || method->name_ == "deinit";
    }
    auto baseType = mapType(*node.baseType_);
    if (!baseType || baseType->getKind() != zir::TypeKind::Class) {
      error(node.baseType_->span, "Base type must be a class.");
    } else {
      auto baseClass = std::static_pointer_cast<zir::ClassType>(baseType);
      classType->setBase(baseClass);
      auto baseIt = classInfos_.find(baseClass->getName());
      if (baseIt != classInfos_.end()) {
        if (!hasOwnCtor) {
          classInfo.constructor = baseIt->second.constructor;
        }
        if (!hasOwnDtor) {
          classInfo.destructor = baseIt->second.destructor;
        }
        classInfo.fields = baseIt->second.fields;
        classInfo.methods.insert(baseIt->second.methods.begin(),
                                 baseIt->second.methods.end());
        for (const auto &field : baseClass->getFields()) {
          classType->addField(field.name, field.type, field.visibility);
        }
      }
    }
  }

  for (const auto &field : node.fields_) {
    auto fieldType = mapType(*field->type);
    if (!fieldType) {
      error(field->span, "Unknown type: " + field->type->qualifiedName());
      fieldType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
    }
    classType->addField(field->name, fieldType,
                        static_cast<int>(field->visibility_));
    classInfo.fields[field->name] = std::make_shared<VariableSymbol>(
        field->name, fieldType, false, false, field->name,
        modules_[currentModuleId_].info->moduleName, field->visibility_);
  }

  auto boundRecord = std::make_unique<BoundRecordDeclaration>();
  boundRecord->type = classType;
  boundRoot_->records.push_back(std::move(boundRecord));

  for (const auto &method : node.methods_) {
    method->accept(*this);
  }

  currentClassStack_.pop_back();
}

void Binder::visit(FunDecl &node) {
  auto symbolIt = declaredFunctionSymbols_.find(&node);
  auto symbol =
      symbolIt == declaredFunctionSymbols_.end() ? nullptr : symbolIt->second;
  if (!symbol) {
    return;
  }

  if (!symbol->genericParameterNames.empty() && !symbol->isGenericInstantiation) {
    return;
  }

  pushScope();
  auto oldFunction = currentFunction_;
  currentFunction_ = symbol;
  int oldUnsafeDepth = unsafeDepth_;
  if (node.isUnsafe_) {
    requireUnsafeEnabled(node.span, "'unsafe fun'");
    ++unsafeDepth_;
  }

  for (const auto &param : symbol->parameters) {
    if (!currentScope_->declare(param->name, param)) {
      error(node.span, "Parameter '" + param->name + "' already declared.");
    }
  }

  auto boundBody = bindBody(node.body_.get(), false);

  popScope();
  currentFunction_ = oldFunction;
  unsafeDepth_ = oldUnsafeDepth;

  bool hasReturn = false;
  if (boundBody) {
    if (boundBody->result)
      hasReturn = true;
    for (const auto &stmt : boundBody->statements) {
      if (dynamic_cast<BoundReturnStatement *>(stmt.get())) {
        hasReturn = true;
        break;
      }
    }
  }

  if (!hasReturn && symbol->linkName == "main" &&
      symbol->returnType->isInteger()) {
    auto intType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int);
    auto lit = std::make_unique<BoundLiteral>("0", intType);
    boundBody->statements.push_back(
        std::make_unique<BoundReturnStatement>(std::move(lit)));
    hasReturn = true;
  }

  if (!hasReturn && symbol->returnType->getKind() != zir::TypeKind::Void) {
    auto kind = symbol->returnType->getKind();
    if (symbol->returnType->isInteger() || kind == zir::TypeKind::Float ||
        kind == zir::TypeKind::Bool) {
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

void Binder::visit(ExtDecl &node) {
  auto symbolIt = declaredFunctionSymbols_.find(&node);
  auto symbol =
      symbolIt == declaredFunctionSymbols_.end() ? nullptr : symbolIt->second;
  if (!symbol) {
    return;
  }

  for (const auto &existing : boundRoot_->externalFunctions) {
    if (existing->symbol->linkName == symbol->linkName) {
      return;
    }
  }

  boundRoot_->externalFunctions.push_back(
      std::make_unique<BoundExternalFunctionDeclaration>(symbol));
}

std::unique_ptr<BoundBlock> Binder::bindBody(BodyNode *body, bool createScope) {
  auto savedBlock = std::move(currentBlock_);
  if (createScope) {
    pushScope();
  }

  if (body) {
    body->accept(*this);
  }

  auto boundBody = std::make_unique<BoundBlock>();
  if (currentBlock_) {
    boundBody = std::move(currentBlock_);
  }

  if (createScope) {
    popScope();
  }

  currentBlock_ = std::move(savedBlock);
  return boundBody;
}

void Binder::visit(BodyNode &node) {
  currentBlock_ = std::make_unique<BoundBlock>();

  for (const auto &stmt : node.statements) {
    stmt->accept(*this);
    if (!statementStack_.empty()) {
      currentBlock_->statements.push_back(std::move(statementStack_.top()));
      statementStack_.pop();
    } else if (!expressionStack_.empty()) {
      auto expr = std::move(expressionStack_.top());
      expressionStack_.pop();
      currentBlock_->statements.push_back(
          std::make_unique<BoundExpressionStatement>(std::move(expr)));
    }
  }

  if (node.result) {
    node.result->accept(*this);
    if (!expressionStack_.empty()) {
      currentBlock_->result = std::move(expressionStack_.top());
      expressionStack_.pop();
    }
  }
}

void Binder::visit(UnsafeBlockNode &node) {
  requireUnsafeEnabled(node.span, "'unsafe' block");
  int oldUnsafeDepth = unsafeDepth_;
  ++unsafeDepth_;

  auto savedBlock = std::move(currentBlock_);
  pushScope();
  visit(static_cast<BodyNode &>(node));

  auto boundBody = std::make_unique<BoundBlock>();
  if (currentBlock_) {
    boundBody = std::move(currentBlock_);
  }

  popScope();
  currentBlock_ = std::move(savedBlock);
  unsafeDepth_ = oldUnsafeDepth;
  statementStack_.push(std::move(boundBody));
}

void Binder::visit(VarDecl &node) {
  auto existing = currentScope_->lookupLocal(node.name_);
  std::shared_ptr<VariableSymbol> symbol;
  if (existing) {
    symbol = std::dynamic_pointer_cast<VariableSymbol>(existing);
  }

  auto type = mapType(*node.type_);
  if (!type) {
    error(node.span, "Unknown type: " + node.type_->qualifiedName());
    type = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
  }

  std::unique_ptr<BoundExpression> initializer = nullptr;
  if (node.initializer_) {
    initializer = bindExpressionWithExpected(node.initializer_.get(), type);
    if (initializer) {
      if (!canConvert(initializer->type, type)) {
        error(node.initializer_->span, "Cannot assign expression of type '" +
                                           renderTypeForUser(initializer->type) +
                                           "' to variable of type '" +
                                           renderTypeForUser(type) + "'");
      } else {
        initializer = wrapInCast(std::move(initializer), type);
      }
    }
  }

  if (!symbol) {
    symbol = std::make_shared<VariableSymbol>(
        node.name_, type, false, false,
        node.isGlobal_ ? mangleName(currentModuleLinkPath(), node.name_)
                       : node.name_,
        modules_[currentModuleId_].info->moduleName, node.visibility_);
    if (!currentScope_->declare(node.name_, symbol)) {
      error(node.span, "Variable '" + node.name_ + "' already declared.");
    }
  }

  auto boundDecl = std::make_unique<BoundVariableDeclaration>(
      symbol, std::move(initializer));

  if (currentBlock_ && !node.isGlobal_) {
    statementStack_.push(std::move(boundDecl));
  } else {
    boundRoot_->globals.push_back(std::move(boundDecl));
  }
}

void Binder::visit(ConstDecl &node) {
  auto existing = currentScope_->lookupLocal(node.name_);
  std::shared_ptr<VariableSymbol> symbol;
  if (existing) {
    symbol = std::dynamic_pointer_cast<VariableSymbol>(existing);
  }

  auto type = mapType(*node.type_);
  if (!type) {
    error(node.span, "Unknown type: " + node.type_->qualifiedName());
    type = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
  }

  std::unique_ptr<BoundExpression> initializer = nullptr;
  if (node.initializer_) {
    initializer = bindExpressionWithExpected(node.initializer_.get(), type);
    if (initializer) {
      if (!canConvert(initializer->type, type)) {
        error(node.initializer_->span, "Cannot assign expression of type '" +
                                           renderTypeForUser(initializer->type) +
                                           "' to constant of type '" +
                                           renderTypeForUser(type) + "'");
      } else {
        initializer = wrapInCast(std::move(initializer), type);
      }
    }
  } else {
    error(node.span, "Constant '" + node.name_ + "' must be initialized.");
  }

  if (!symbol) {
    symbol = std::make_shared<VariableSymbol>(
        node.name_, type, true, false,
        mangleName(currentModuleLinkPath(), node.name_),
        modules_[currentModuleId_].info->moduleName, node.visibility_);
    if (!currentScope_->declare(node.name_, symbol)) {
      error(node.span, "Identifier '" + node.name_ + "' already declared.");
    }
  }

  if (initializer) {
    symbol->constant_value =
        std::shared_ptr<BoundExpression>(initializer->clone());
  }

  auto boundDecl = std::make_unique<BoundVariableDeclaration>(
      symbol, std::move(initializer));

  if (currentBlock_) {
    statementStack_.push(std::move(boundDecl));
  } else {
    boundRoot_->globals.push_back(std::move(boundDecl));
  }
}

void Binder::visit(ReturnNode &node) {
  std::unique_ptr<BoundExpression> expr = nullptr;
  if (node.returnValue) {
    expr = bindExpressionWithExpected(
        node.returnValue.get(),
        currentFunction_ ? currentFunction_->returnType : nullptr);
  }

  if (currentFunction_) {
    auto expectedType = currentFunction_->returnType;
    auto actualType =
        expr ? expr->type
             : std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
    if (!canConvert(actualType, expectedType)) {
      error(node.span, "Function '" + currentFunction_->name +
                           "' expects return type '" +
                           renderTypeForUser(expectedType) + "', but received '" +
                           renderTypeForUser(actualType) + "'");
    } else if (expr) {
      expr = wrapInCast(std::move(expr), expectedType);
    }
  }

  statementStack_.push(std::make_unique<BoundReturnStatement>(std::move(expr)));
}

void Binder::visit(BinExpr &node) {
  std::unique_ptr<BoundExpression> left;
  std::unique_ptr<BoundExpression> right;

  bool leftIsNullLiteral =
      dynamic_cast<ConstNull *>(node.left_.get()) != nullptr;
  bool rightIsNullLiteral =
      dynamic_cast<ConstNull *>(node.right_.get()) != nullptr;

  if (leftIsNullLiteral && !rightIsNullLiteral) {
    right = bindExpressionWithExpected(node.right_.get(), nullptr);
    if (!right)
      return;
    left = bindExpressionWithExpected(node.left_.get(), right->type);
    if (!left)
      return;
  } else if (rightIsNullLiteral && !leftIsNullLiteral) {
    left = bindExpressionWithExpected(node.left_.get(), nullptr);
    if (!left)
      return;
    right = bindExpressionWithExpected(node.right_.get(), left->type);
    if (!right)
      return;
  } else {
    node.left_->accept(*this);
    if (expressionStack_.empty())
      return;
    left = std::move(expressionStack_.top());
    expressionStack_.pop();

    node.right_->accept(*this);
    if (expressionStack_.empty())
      return;
    right = std::move(expressionStack_.top());
    expressionStack_.pop();
  }

  auto leftType = left->type;
  auto rightType = right->type;
  std::shared_ptr<zir::Type> resultType = leftType;

  if (node.op_ == "+" &&
      ((isStringType(leftType) || leftType->getKind() == zir::TypeKind::Char) ||
       (isStringType(rightType) || rightType->getKind() == zir::TypeKind::Char))) {
    bool leftOk =
        isStringType(leftType) || leftType->getKind() == zir::TypeKind::Char;
    bool rightOk =
        isStringType(rightType) || rightType->getKind() == zir::TypeKind::Char;
    bool hasString = isStringType(leftType) || isStringType(rightType);

    if (!leftOk || !rightOk || !hasString) {
      error(SourceSpan::merge(node.left_->span, node.right_->span),
            "Concatenation requires String and/or Char operands with at least "
            "one String, got '" +
                renderTypeForUser(leftType) + "' and '" +
                renderTypeForUser(rightType) + "'");
    }
    resultType = std::make_shared<zir::RecordType>("String", "String");
  } else if ((node.op_ == "+" || node.op_ == "-") &&
             (isPointerType(leftType) || isPointerType(rightType))) {
    requireUnsafeEnabled(node.span, "pointer arithmetic");

    if (node.op_ == "+" && isPointerType(leftType) && rightType->isInteger()) {
      resultType = leftType;
    } else if (node.op_ == "+" && leftType->isInteger() &&
               isPointerType(rightType)) {
      std::swap(left, right);
      std::swap(leftType, rightType);
      resultType = leftType;
    } else if (node.op_ == "-" && isPointerType(leftType) &&
               rightType->isInteger()) {
      resultType = leftType;
    } else if (node.op_ == "-" && isPointerType(leftType) &&
               isPointerType(rightType)) {
      if (leftType->toString() != rightType->toString()) {
        error(SourceSpan::merge(node.left_->span, node.right_->span),
              "Pointer subtraction requires operands of the same type.");
      }
      resultType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int);
    } else {
      error(SourceSpan::merge(node.left_->span, node.right_->span),
            "Invalid pointer arithmetic between '" +
                renderTypeForUser(leftType) + "' and '" +
                renderTypeForUser(rightType) + "'");
    }
  } else if (node.op_ == "+" || node.op_ == "-" || node.op_ == "*" ||
             node.op_ == "/" || node.op_ == "%") {
    if (!isNumeric(leftType) || !isNumeric(rightType)) {
      error(SourceSpan::merge(node.left_->span, node.right_->span),
            "Operator '" + node.op_ + "' cannot be applied to '" +
                renderTypeForUser(leftType) + "' and '" +
                renderTypeForUser(rightType) + "'");
    } else {
      resultType = getPromotedType(leftType, rightType);
      left = wrapInCast(std::move(left), resultType);
      right = wrapInCast(std::move(right), resultType);
    }
  } else if (node.op_ == "&" || node.op_ == "|" || node.op_ == "^") {
    if (!leftType->isInteger() || !rightType->isInteger()) {
      error(SourceSpan::merge(node.left_->span, node.right_->span),
            "Bitwise operator '" + node.op_ +
                "' requires integer operands, got '" +
                renderTypeForUser(leftType) + "' and '" +
                renderTypeForUser(rightType) + "'");
    } else {
      resultType = getPromotedType(leftType, rightType);
      left = wrapInCast(std::move(left), resultType);
      right = wrapInCast(std::move(right), resultType);
    }
  } else if (node.op_ == "<<" || node.op_ == ">>") {
    if (!leftType->isInteger() || !rightType->isInteger()) {
      error(SourceSpan::merge(node.left_->span, node.right_->span),
            "Shift operator '" + node.op_ + "' requires integer operands, got '" +
                renderTypeForUser(leftType) + "' and '" +
                renderTypeForUser(rightType) + "'");
    } else {
      // Keep the left-hand integer type for shift results.
      resultType = leftType;
      right = wrapInCast(std::move(right), resultType);

      if (auto shiftAmount = evaluateConstantInt(right.get())) {
        if (*shiftAmount < 0) {
          error(SourceSpan::merge(node.left_->span, node.right_->span),
                "Shift amount must be non-negative, got '" +
                    std::to_string(*shiftAmount) + "'.");
        } else {
          unsigned width =
              static_cast<unsigned>(typeBitWidth(resultType));
          if (width == 0 || static_cast<uint64_t>(*shiftAmount) >= width) {
            error(SourceSpan::merge(node.left_->span, node.right_->span),
                  "Shift amount '" + std::to_string(*shiftAmount) +
                      "' is out of range for type '" +
                      renderTypeForUser(resultType) + "' (" +
                      std::to_string(width) + "-bit width).");
          }
        }
      }
    }
  } else if (node.op_ == "==" || node.op_ == "!=" || node.op_ == "<" ||
             node.op_ == "<=" || node.op_ == ">" || node.op_ == ">=") {
    bool classOrNullComparison =
        (leftType->getKind() == zir::TypeKind::Class &&
         isNullType(rightType)) ||
        (rightType->getKind() == zir::TypeKind::Class && isNullType(leftType));
    if (!classOrNullComparison &&
        (isPointerType(leftType) || isPointerType(rightType) ||
         isNullType(leftType) || isNullType(rightType))) {
      requireUnsafeEnabled(node.span, "pointer comparisons");
    }

    // Reject comparisons of struct types
    if (leftType->getKind() == zir::TypeKind::Record ||
        rightType->getKind() == zir::TypeKind::Record) {
      error(SourceSpan::merge(node.left_->span, node.right_->span),
            "Cannot compare struct types '" + renderTypeForUser(leftType) +
                "' and '" + renderTypeForUser(rightType) + "'");
    }

    if (!canConvert(leftType, rightType) && !canConvert(rightType, leftType)) {
      error(SourceSpan::merge(node.left_->span, node.right_->span),
            "Cannot compare '" + renderTypeForUser(leftType) + "' and '" +
                renderTypeForUser(rightType) + "'");
    } else if (isNullType(leftType) && isPointerType(rightType)) {
      left = wrapInCast(std::move(left), rightType);
    } else if (isNullType(rightType) && isPointerType(leftType)) {
      right = wrapInCast(std::move(right), leftType);
    } else if (isNullType(leftType) &&
               rightType->getKind() == zir::TypeKind::Class) {
      left = wrapInCast(std::move(left), rightType);
    } else if (isNullType(rightType) &&
               leftType->getKind() == zir::TypeKind::Class) {
      right = wrapInCast(std::move(right), leftType);
    }
    resultType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Bool);
  } else if (node.op_ == "&&" || node.op_ == "||") {
    if (leftType->getKind() != zir::TypeKind::Bool ||
        rightType->getKind() != zir::TypeKind::Bool) {
      error(SourceSpan::merge(node.left_->span, node.right_->span),
            "Logical operator '" + node.op_ + "' requires Bool operands.");
    }
    resultType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Bool);
  }

  expressionStack_.push(std::make_unique<BoundBinaryExpression>(
      std::move(left), node.op_, std::move(right), resultType));
}

void Binder::visit(TernaryExpr &node) {
  node.condition_->accept(*this);
  if (expressionStack_.empty())
    return;
  auto condition = std::move(expressionStack_.top());
  expressionStack_.pop();

  if (condition->type->getKind() != zir::TypeKind::Bool) {
    error(node.condition_->span, "Ternary condition must be Bool, got '" +
                                     renderTypeForUser(condition->type) + "'");
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
      !canConvert(elseExpr->type, thenExpr->type)) {
    error(SourceSpan::merge(node.thenExpr_->span, node.elseExpr_->span),
          "Ternary branches must be compatible, got '" +
              renderTypeForUser(thenExpr->type) + "' and '" +
              renderTypeForUser(elseExpr->type) + "'");
  }

  auto resultType = canConvert(thenExpr->type, elseExpr->type) ? elseExpr->type
                                                               : thenExpr->type;
  thenExpr = wrapInCast(std::move(thenExpr), resultType);
  elseExpr = wrapInCast(std::move(elseExpr), resultType);

  expressionStack_.push(std::make_unique<BoundTernaryExpression>(
      std::move(condition), std::move(thenExpr), std::move(elseExpr),
      resultType));
}

void Binder::visit(ConstInt &node) {
  expressionStack_.push(std::make_unique<BoundLiteral>(
      std::to_string(node.value_),
      std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int)));
}

void Binder::visit(ConstFloat &node) {
  expressionStack_.push(std::make_unique<BoundLiteral>(
      std::to_string(node.value_),
      std::make_shared<zir::PrimitiveType>(zir::TypeKind::Float)));
}

void Binder::visit(ConstString &node) {
  expressionStack_.push(std::make_unique<BoundLiteral>(
      node.value_, std::make_shared<zir::RecordType>("String", "String")));
}

void Binder::visit(ConstChar &node) {
  expressionStack_.push(std::make_unique<BoundLiteral>(
      node.value_, std::make_shared<zir::PrimitiveType>(zir::TypeKind::Char)));
}

void Binder::visit(ConstNull &node) {
  auto expectedType = currentExpectedExpressionType();
  bool nullIsSafeHere =
      expectedType && expectedType->getKind() == zir::TypeKind::Class;

  if (!nullIsSafeHere) {
    requireUnsafeEnabled(node.span, "'null'");
  }
  expressionStack_.push(std::make_unique<BoundLiteral>(
      "0", std::make_shared<zir::PrimitiveType>(zir::TypeKind::NullPtr)));
}

void Binder::visit(CastExpr &node) {
  node.expr_->accept(*this);
  if (expressionStack_.empty())
    return;

  auto expr = std::move(expressionStack_.top());
  expressionStack_.pop();

  auto targetType = mapType(*node.type_);
  if (!targetType) {
    error(node.type_->span, "Unknown type: " + node.type_->qualifiedName());
    return;
  }

  requireUnsafeEnabled(node.span, "explicit casts");

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

  if (!castAllowed) {
    error(node.span, "Cannot cast from '" + renderTypeForUser(expr->type) +
                         "' to '" + renderTypeForUser(targetType) + "'");
    return;
  }

  expressionStack_.push(
      std::make_unique<BoundCast>(std::move(expr), targetType));
}

void Binder::visit(ConstId &node) {
  auto symbol = currentScope_->lookup(node.value_);
  if (!symbol) {
    error(node.span, "Undefined identifier: " + node.value_);
    return;
  }

  if (auto varSymbol = std::dynamic_pointer_cast<VariableSymbol>(symbol)) {
    expressionStack_.push(std::make_unique<BoundVariableExpression>(varSymbol));
  } else if (auto typeSymbol = std::dynamic_pointer_cast<TypeSymbol>(symbol)) {
    expressionStack_.push(std::make_unique<BoundLiteral>("", typeSymbol->type));
  } else if (auto moduleSymbol =
                 std::dynamic_pointer_cast<ModuleSymbol>(symbol)) {
    expressionStack_.push(std::make_unique<BoundModuleReference>(moduleSymbol));
  } else {
    error(node.span, "'" + node.value_ + "' is not a variable or type.");
  }
}

void Binder::visit(AssignNode &node) {
  node.target_->accept(*this);
  if (expressionStack_.empty())
    return;
  auto target = std::move(expressionStack_.top());
  expressionStack_.pop();

  bool isLValue =
      dynamic_cast<BoundVariableExpression *>(target.get()) ||
      dynamic_cast<BoundIndexAccess *>(target.get()) ||
      dynamic_cast<BoundMemberAccess *>(target.get()) ||
      (dynamic_cast<BoundUnaryExpression *>(target.get()) &&
       static_cast<BoundUnaryExpression *>(target.get())->op == "*");

  if (!isLValue) {
    error(node.span, "Target of assignment must be an l-value.");
    return;
  }

  if (auto varExpr = dynamic_cast<BoundVariableExpression *>(target.get())) {
    if (varExpr->symbol->is_const) {
      error(node.span,
            "Cannot assign to constant '" + varExpr->symbol->name + "'.");
      return;
    }
  } else if (auto indexExpr = dynamic_cast<BoundIndexAccess *>(target.get())) {
    if (isStringType(indexExpr->left->type)) {
      error(node.span, "Cannot assign through String index access.");
      return;
    }
  }

  auto expr = bindExpressionWithExpected(node.expr_.get(), target->type);
  if (!expr)
    return;

  if (!canConvert(expr->type, target->type)) {
    error(node.span, "Cannot assign expression of type '" +
                         renderTypeForUser(expr->type) + "' to type '" +
                         renderTypeForUser(target->type) + "'");
  } else {
    expr = wrapInCast(std::move(expr), target->type);
  }

  statementStack_.push(
      std::make_unique<BoundAssignment>(std::move(target), std::move(expr)));
}

void Binder::visit(IndexAccessNode &node) {
  node.left_->accept(*this);
  if (expressionStack_.empty())
    return;
  auto left = std::move(expressionStack_.top());
  expressionStack_.pop();

  if (left->type->getKind() != zir::TypeKind::Array &&
      !isVariadicViewType(left->type) && !isStringType(left->type)) {
    error(node.span,
          "Type '" + renderTypeForUser(left->type) +
              "' does not support indexing.");
    return;
  }

  node.index_->accept(*this);
  if (expressionStack_.empty())
    return;
  auto index = std::move(expressionStack_.top());
  expressionStack_.pop();

  if (!index->type->isInteger()) {
    error(node.span, "Array index must be an integer, but got '" +
                         renderTypeForUser(index->type) + "'");
  }

  std::shared_ptr<zir::Type> elementType;
  if (left->type->getKind() == zir::TypeKind::Array) {
    auto arrayType = std::static_pointer_cast<zir::ArrayType>(left->type);
    elementType = arrayType->getBaseType();
  } else if (isVariadicViewType(left->type)) {
    auto recordType = std::static_pointer_cast<zir::RecordType>(left->type);
    const auto &fields = recordType->getFields();
    if (fields.empty() || fields[0].type->getKind() != zir::TypeKind::Pointer) {
      error(node.span, "Internal error: invalid variadic view layout.");
      return;
    }
    elementType = std::static_pointer_cast<zir::PointerType>(fields[0].type)
                      ->getBaseType();
  } else {
    elementType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Char);
  }

  expressionStack_.push(std::make_unique<BoundIndexAccess>(
      std::move(left), std::move(index), elementType));
}

void Binder::visit(MemberAccessNode &node) {
  node.left_->accept(*this);
  if (expressionStack_.empty())
    return;

  auto left = std::move(expressionStack_.top());
  expressionStack_.pop();

  if (auto moduleRef = dynamic_cast<BoundModuleReference *>(left.get())) {
    auto memberIt = moduleRef->symbol->members.find(node.member_);
    if (memberIt == moduleRef->symbol->members.end()) {
      error(node.span, "Module '" + moduleRef->symbol->name +
                           "' has no member '" + node.member_ + "'");
      return;
    }
    if (memberIt->second->visibility != Visibility::Public) {
      error(node.span, "Member '" + node.member_ + "' of module '" +
                           moduleRef->symbol->name + "' is private.");
      return;
    }

    if (auto varSymbol =
            std::dynamic_pointer_cast<VariableSymbol>(memberIt->second)) {
      expressionStack_.push(
          std::make_unique<BoundVariableExpression>(varSymbol));
      return;
    }
    if (auto typeSymbol =
            std::dynamic_pointer_cast<TypeSymbol>(memberIt->second)) {
      expressionStack_.push(
          std::make_unique<BoundLiteral>("", typeSymbol->type));
      return;
    }
    if (auto nestedModule =
            std::dynamic_pointer_cast<ModuleSymbol>(memberIt->second)) {
      expressionStack_.push(
          std::make_unique<BoundModuleReference>(nestedModule));
      return;
    }

    error(node.span, "'" + node.member_ + "' is not a value or type.");
    return;
  }

  if (left->type->getKind() == zir::TypeKind::Enum) {
    auto enumType = std::static_pointer_cast<zir::EnumType>(left->type);
    int value = enumType->getVariantIndex(node.member_);
    if (value != -1) {
      expressionStack_.push(
          std::make_unique<BoundLiteral>(std::to_string(value), enumType));
      return;
    }
  } else if (left->type->getKind() == zir::TypeKind::Record) {
    auto recordType = std::static_pointer_cast<zir::RecordType>(left->type);
    for (const auto &field : recordType->getFields()) {
      if (field.name == node.member_) {
        expressionStack_.push(std::make_unique<BoundMemberAccess>(
            std::move(left), node.member_, field.type));
        return;
      }
    }
  } else if (left->type->getKind() == zir::TypeKind::Class) {
    auto classType = std::static_pointer_cast<zir::ClassType>(left->type);
    if (classType->isWeak()) {
      error(node.span, "Weak references cannot be accessed directly.");
      return;
    }
    auto infoIt = classInfos_.find(classType->getName());
    if (infoIt != classInfos_.end()) {
      auto fieldIt = infoIt->second.fields.find(node.member_);
      if (fieldIt != infoIt->second.fields.end()) {
        auto fieldVis = fieldIt->second->visibility;
        bool allowed =
            fieldVis == Visibility::Public ||
            (!currentClassStack_.empty() &&
             currentClassStack_.back() == classType->getName()) ||
            (fieldVis == Visibility::Protected && !currentClassStack_.empty());
        if (!allowed) {
          error(node.span, "Field '" + node.member_ + "' is not accessible.");
          return;
        }
        expressionStack_.push(std::make_unique<BoundMemberAccess>(
            std::move(left), node.member_, fieldIt->second->type));
        return;
      }
    }
  }

  error(node.span, "Member '" + node.member_ + "' not found in type '" +
                       renderTypeForUser(left->type) + "'");
}

void Binder::visit(FunCall &node) {
  if (bindWeakBuiltinCall(node)) {
    return;
  }

  if (auto member = dynamic_cast<MemberAccessNode *>(node.callee_.get())) {
    member->left_->accept(*this);
    if (expressionStack_.empty()) {
      return;
    }
    auto selfExpr = std::move(expressionStack_.top());
    expressionStack_.pop();
    if (selfExpr->type->getKind() != zir::TypeKind::Class) {
      // Not a class method call. Fall through to the normal qualified
      // function/module call resolution path below.
    } else {
      auto classType = std::static_pointer_cast<zir::ClassType>(selfExpr->type);
      if (classType->isWeak()) {
        error(node.span,
              "Weak references cannot be used to call methods directly.");
        return;
      }
      auto infoIt = classInfos_.find(classType->getName());
      if (infoIt == classInfos_.end()) {
        error(node.span, "Unknown class type: " + classType->getName());
        return;
      }
      auto methodIt = infoIt->second.methods.find(member->member_);
      if (methodIt == infoIt->second.methods.end()) {
        error(node.span, "Class '" + classType->getName() +
                             "' has no method '" + member->member_ + "'.");
        return;
      }
      auto funcSymbol =
          std::dynamic_pointer_cast<FunctionSymbol>(methodIt->second);
      if (!funcSymbol) {
        error(node.span, "'" + member->member_ + "' is not a method.");
        return;
      }
      bool methodAllowed =
          funcSymbol->visibility == Visibility::Public ||
          (!currentClassStack_.empty() &&
           currentClassStack_.back() == classType->getName()) ||
          (funcSymbol->visibility == Visibility::Protected &&
           !currentClassStack_.empty());
      if (!methodAllowed) {
        error(node.span, "Method '" + member->member_ + "' is not accessible.");
        return;
      }
      if (funcSymbol->isUnsafe) {
        requireUnsafeEnabled(node.span, "unsafe function calls");
        requireUnsafeContext(node.span, "unsafe function calls");
      }

      std::vector<std::unique_ptr<BoundExpression>> rawArgs;
      rawArgs.reserve(node.params_.size());
      for (size_t i = 0; i < node.params_.size(); ++i) {
        auto arg = bindExpressionWithExpected(node.params_[i]->value.get(), nullptr);
        if (!arg) {
          return;
        }
        rawArgs.push_back(std::move(arg));
      }

      if (!funcSymbol->genericParameterNames.empty()) {
        std::vector<std::unique_ptr<BoundExpression>> inferenceArgs;
        if (funcSymbol->isMethod) {
          inferenceArgs.push_back(selfExpr->clone());
        }
        for (const auto &rawArg : rawArgs) {
          inferenceArgs.push_back(rawArg->clone());
        }
        std::string genericBindingFailure;
        auto genericBindings = buildGenericBindings(
            *funcSymbol, inferenceArgs, node.genericArgs_, node.span,
            &genericBindingFailure);
        if (genericBindings.empty()) {
          error(node.span, "No matching overload for method '" + member->member_ +
                               "'. " +
                               (genericBindingFailure.empty()
                                    ? std::string("Generic type binding failed.")
                                    : genericBindingFailure));
          return;
        }
        funcSymbol = ensureGenericFunctionInstantiation(
            funcSymbol, orderedGenericBindings(genericBindings), node.span);
        if (!funcSymbol) {
          error(node.span, "Failed to instantiate generic method '" +
                               member->member_ + "'.");
          return;
        }
      } else if (!node.genericArgs_.empty()) {
        error(node.span, "Method '" + member->member_ +
                             "' does not accept generic arguments.");
        return;
      }

      std::vector<std::unique_ptr<BoundExpression>> args;
      std::vector<bool> argIsRef;
      if (funcSymbol->isMethod) {
        args.push_back(std::move(selfExpr));
        argIsRef.push_back(false);
      }

      size_t paramOffset = funcSymbol->isMethod ? 1 : 0;
      if (node.params_.size() + paramOffset != funcSymbol->parameters.size()) {
        error(node.span,
              "No matching overload for method '" + member->member_ + "'.");
        return;
      }

      for (size_t i = 0; i < node.params_.size(); ++i) {
        auto arg = rawArgs[i]->clone();
        auto expectedType = funcSymbol->parameters[i + paramOffset]->type;
        if (!canConvert(arg->type, expectedType)) {
          error(node.params_[i]->value->span,
                "Cannot convert method argument from '" +
                    renderTypeForUser(arg->type) + "' to '" +
                    renderTypeForUser(expectedType) + "'");
          return;
        }
        arg = wrapInCast(std::move(arg), expectedType);
        args.push_back(std::move(arg));
        argIsRef.push_back(node.params_[i]->isRef);
      }

      expressionStack_.push(std::make_unique<BoundFunctionCall>(
          funcSymbol, std::move(args), std::move(argIsRef)));
      return;
    }
  }

  std::vector<std::string> calleeParts;
  if (!node.callee_ || !extractQualifiedPath(node.callee_.get(), calleeParts)) {
    error(node.span, "Only direct function calls are supported.");
    return;
  }

  auto symbol =
      resolveQualifiedSymbol(calleeParts, node.span, SymbolKind::Function);
  if (!symbol) {
    return;
  }

  auto candidates = collectOverloads(symbol);
  if (candidates.empty()) {
    error(node.span, "'" + calleeParts.back() + "' is not a function.");
    return;
  }

  bool seenSpreadArg = false;
  std::vector<std::unique_ptr<BoundExpression>> rawArgs;
  std::vector<bool> rawArgIsRef;
  std::vector<bool> rawArgIsSpread;
  std::vector<std::string> rawArgNames;
  for (size_t i = 0; i < node.params_.size(); ++i) {
    if (seenSpreadArg) {
      error(node.params_[i]->value->span,
            "Spread argument must be the last argument in a function call.");
      return;
    }

    auto arg =
        bindExpressionWithExpected(node.params_[i]->value.get(), nullptr);
    if (!arg)
      return;
    rawArgNames.push_back(node.params_[i]->name);
    rawArgIsRef.push_back(node.params_[i]->isRef);
    rawArgIsSpread.push_back(node.params_[i]->isSpread);
    if (node.params_[i]->isSpread) {
      seenSpreadArg = true;
    }
    rawArgs.push_back(std::move(arg));
  }

  struct CandidateMatch {
    std::shared_ptr<FunctionSymbol> symbol;
    std::vector<std::unique_ptr<BoundExpression>> arguments;
    std::vector<bool> argumentIsRef;
    std::unique_ptr<BoundExpression> variadicPack;
    std::vector<int> cost;
    bool usedExtraArguments = false;
    int returnCost = 0;
    std::vector<std::string> notes;
  };

  auto compareCost = [](const CandidateMatch &lhs, const CandidateMatch &rhs) {
    if (lhs.cost != rhs.cost) {
      return lhs.cost < rhs.cost;
    }
    if (lhs.returnCost != rhs.returnCost) {
      return lhs.returnCost < rhs.returnCost;
    }
    if (lhs.usedExtraArguments != rhs.usedExtraArguments) {
      return !lhs.usedExtraArguments && rhs.usedExtraArguments;
    }
    if (lhs.symbol->acceptsExtraArguments() !=
        rhs.symbol->acceptsExtraArguments()) {
      return !lhs.symbol->acceptsExtraArguments() &&
             rhs.symbol->acceptsExtraArguments();
    }
    return false;
  };

  std::vector<CandidateMatch> matches;
  std::shared_ptr<FunctionSymbol> blockedUnsafeMatch = nullptr;
  std::vector<std::string> rejectionNotes;
  auto expectedReturnType = currentExpectedExpressionType();

  for (const auto &funcSymbol : candidates) {
    if (!funcSymbol) {
      continue;
    }

    size_t fixedParamCount = funcSymbol->fixedParameterCount();
    bool hasExplicitTypeArgs = !node.genericArgs_.empty();
    if (hasExplicitTypeArgs &&
        node.genericArgs_.size() > funcSymbol->genericParameterNames.size()) {
      rejectionNotes.push_back("'" + renderFunctionSignature(*funcSymbol) +
                               "': explicit generic argument count mismatch");
      continue;
    }

    if (!hasExplicitTypeArgs && !funcSymbol->genericParameterNames.empty()) {
      // inference is allowed; no early rejection
    }
    if (!funcSymbol->acceptsExtraArguments() &&
        node.params_.size() != funcSymbol->parameters.size()) {
      rejectionNotes.push_back("'" + renderFunctionSignature(*funcSymbol) +
                               "': wrong argument count");
      continue;
    }
    if (funcSymbol->acceptsExtraArguments() &&
        node.params_.size() < fixedParamCount) {
      rejectionNotes.push_back("'" + renderFunctionSignature(*funcSymbol) +
                               "': too few arguments");
      continue;
    }

    CandidateMatch match;
    match.symbol = funcSymbol;
    match.arguments.resize(fixedParamCount);
    match.argumentIsRef.resize(fixedParamCount, false);
    std::string genericBindingFailure;
    auto genericBindings = buildGenericBindings(
        *funcSymbol, rawArgs, node.genericArgs_, node.span,
        &genericBindingFailure);
    if (!funcSymbol->genericParameterNames.empty() && genericBindings.empty()) {
      rejectionNotes.push_back("'" + renderFunctionSignature(*funcSymbol) +
                               "': " + (genericBindingFailure.empty()
                                            ? "generic type binding failed"
                                            : genericBindingFailure));
      continue;
    }
    bool failed = false;
    std::string failureReason;
    auto variadicParam = funcSymbol->variadicParameter();
    std::vector<int> positionalToParameter(rawArgs.size(), -1);
    std::vector<bool> parameterAssigned(funcSymbol->parameters.size(), false);
    bool seenNamedArgument = false;

    for (size_t i = 0, positionalIndex = 0; i < rawArgs.size(); ++i) {
      const bool isSpread = rawArgIsSpread[i];
      const bool isNamed = !rawArgNames[i].empty();

      if (isSpread) {
        if (isNamed) {
          failed = true;
          failureReason = "named spread arguments are not supported";
          break;
        }
        positionalToParameter[i] = static_cast<int>(fixedParamCount);
        continue;
      }

      if (isNamed) {
        seenNamedArgument = true;
        bool found = false;
        for (size_t paramIndex = 0; paramIndex < fixedParamCount;
             ++paramIndex) {
          if (funcSymbol->parameters[paramIndex]->name != rawArgNames[i]) {
            continue;
          }
          if (parameterAssigned[paramIndex]) {
            failed = true;
            failureReason =
                "parameter '" + rawArgNames[i] + "' provided more than once";
            break;
          }
          positionalToParameter[i] = static_cast<int>(paramIndex);
          parameterAssigned[paramIndex] = true;
          found = true;
          break;
        }
        if (failed) {
          break;
        }
        if (!found) {
          failed = true;
          failureReason = "unknown named argument '" + rawArgNames[i] + "'";
          break;
        }
        continue;
      }

      if (seenNamedArgument) {
        failed = true;
        failureReason = "positional arguments cannot follow named arguments";
        break;
      }

      while (positionalIndex < fixedParamCount &&
             parameterAssigned[positionalIndex]) {
        ++positionalIndex;
      }

      if (positionalIndex < fixedParamCount) {
        positionalToParameter[i] = static_cast<int>(positionalIndex);
        parameterAssigned[positionalIndex] = true;
        ++positionalIndex;
      } else {
        positionalToParameter[i] = static_cast<int>(fixedParamCount);
      }
    }

    if (!failed) {
      for (size_t paramIndex = 0; paramIndex < fixedParamCount; ++paramIndex) {
        if (!parameterAssigned[paramIndex]) {
          failed = true;
          failureReason = "missing argument for parameter '" +
                          funcSymbol->parameters[paramIndex]->name + "'";
          break;
        }
      }
    }

    for (size_t i = 0; i < rawArgs.size(); ++i) {
      auto arg = rawArgs[i]->clone();
      bool argIsRef = rawArgIsRef[i];
      bool argIsSpread = rawArgIsSpread[i];
      int parameterIndex = positionalToParameter[i];

      if (failed) {
        break;
      }

      if (argIsSpread) {
        if (parameterIndex < static_cast<int>(fixedParamCount) || argIsRef ||
            !funcSymbol->hasVariadicParameter()) {
          failed = true;
          failureReason =
              "spread arguments can only target a Zap variadic parameter";
          break;
        }

        auto *varExpr = dynamic_cast<BoundVariableExpression *>(arg.get());
        if (!varExpr || !varExpr->symbol->is_variadic_pack || !variadicParam ||
            !varExpr->symbol->variadic_element_type ||
            varExpr->symbol->variadic_element_type->toString() !=
                variadicParam->variadic_element_type->toString()) {
          failed = true;
          failureReason =
              "spread argument type does not match variadic parameter";
          break;
        }

        match.variadicPack = std::move(arg);
        match.usedExtraArguments = true;
        match.notes.push_back("spread -> variadic pack");
        continue;
      }

      if (parameterIndex >= 0 &&
          parameterIndex < static_cast<int>(fixedParamCount)) {
        auto expectedType = funcSymbol->parameters[parameterIndex]->type;
        if (!genericBindings.empty()) {
          expectedType = substituteGenericType(expectedType, genericBindings);
        }
        const auto &parameter = funcSymbol->parameters[parameterIndex];
        if (argIsRef != parameter->is_ref) {
          failed = true;
          failureReason = "argument for parameter '" + parameter->name +
                          "' has mismatched ref-ness";
          break;
        }

        if (argIsRef) {
          auto varExpr = dynamic_cast<BoundVariableExpression *>(arg.get());
          if (!varExpr || arg->type->toString() != expectedType->toString()) {
            failed = true;
            failureReason = "ref argument for parameter '" + parameter->name +
                            "' must exactly match type '" +
                            renderTypeForUser(expectedType) + "'";
            break;
          }
          match.cost.push_back(0);
          match.notes.push_back("param " + parameter->name +
                                ": exact ref match");
        } else if (!canConvert(arg->type, expectedType)) {
          failed = true;
          failureReason = "argument for parameter '" + parameter->name +
                          "' is not convertible from '" +
                          renderTypeForUser(arg->type) + "' to '" +
                          renderTypeForUser(expectedType) + "'";
          break;
        } else {
          int cost = conversionCost(arg->type, expectedType);
          if (cost >= 1000) {
            failed = true;
            failureReason = "argument for parameter '" + parameter->name +
                            "' is not convertible from '" +
                            renderTypeForUser(arg->type) + "' to '" +
                            renderTypeForUser(expectedType) + "'";
            break;
          }
          match.cost.push_back(cost);
          match.notes.push_back("param " + parameter->name + ": " +
                                describeConversion(arg->type, expectedType));
          arg = wrapInCast(std::move(arg), expectedType);
        }
        match.argumentIsRef[parameterIndex] = argIsRef;
        match.arguments[parameterIndex] = std::move(arg);
      } else if (funcSymbol->hasVariadicParameter()) {
        if (argIsRef) {
          failed = true;
          failureReason = "variadic arguments cannot be passed by ref";
          break;
        }
        auto expectedType = variadicParam->variadic_element_type;
        if (!genericBindings.empty()) {
          expectedType = substituteGenericType(expectedType, genericBindings);
        }
        if (!canConvert(arg->type, expectedType)) {
          failed = true;
          failureReason = "variadic argument is not convertible from '" +
                          renderTypeForUser(arg->type) + "' to '" +
                          renderTypeForUser(expectedType) + "'";
          break;
        }
        int cost = conversionCost(arg->type, expectedType);
        match.cost.push_back(cost);
        match.usedExtraArguments = true;
        match.notes.push_back("variadic: " +
                              describeConversion(arg->type, expectedType));
        arg = wrapInCast(std::move(arg), expectedType);
        match.argumentIsRef.push_back(false);
        match.arguments.push_back(std::move(arg));
      } else if (funcSymbol->isCVariadic) {
        if (argIsRef) {
          failed = true;
          failureReason = "C variadic arguments cannot be passed by ref";
          break;
        }
        auto promotedType = getCVariadicArgumentType(arg->type);
        if (!promotedType) {
          failed = true;
          failureReason = "type '" + renderTypeForUser(arg->type) +
                          "' is not supported in C variadic arguments";
          break;
        }
        int cost = conversionCost(arg->type, promotedType);
        match.cost.push_back(cost);
        match.usedExtraArguments = true;
        match.notes.push_back("c variadic: " +
                              describeConversion(arg->type, promotedType));
        arg = wrapInCast(std::move(arg), promotedType);
        match.argumentIsRef.push_back(false);
        match.arguments.push_back(std::move(arg));
      } else {
        failed = true;
        failureReason = "too many arguments";
        break;
      }
    }

    if (failed) {
      rejectionNotes.push_back("'" + renderFunctionSignature(*funcSymbol) +
                               "': " + failureReason);
      continue;
    }

    if (expectedReturnType) {
      if (funcSymbol->returnType->toString() ==
          expectedReturnType->toString()) {
        match.returnCost = 0;
        match.notes.push_back("return: exact match");
      } else if (canConvert(funcSymbol->returnType, expectedReturnType)) {
        match.returnCost =
            conversionCost(funcSymbol->returnType, expectedReturnType);
        match.notes.push_back(
            "return: " +
            describeConversion(funcSymbol->returnType, expectedReturnType));
      } else {
        match.returnCost = 50;
        match.notes.push_back("return: incompatible with expected " +
                              renderTypeForUser(expectedReturnType));
      }
    }

    std::shared_ptr<FunctionSymbol> resolvedSymbol = funcSymbol;
    if (!funcSymbol->genericParameterNames.empty()) {
      resolvedSymbol = ensureGenericFunctionInstantiation(
          funcSymbol, orderedGenericBindings(genericBindings), node.span);
      if (!resolvedSymbol) {
        rejectionNotes.push_back("'" + renderFunctionSignature(*funcSymbol) +
                                 "': failed to instantiate generic function");
        continue;
      }

      std::vector<std::unique_ptr<BoundExpression>> remappedArgs;
      std::vector<bool> remappedRef;
      remappedArgs.reserve(match.arguments.size());
      remappedRef.reserve(match.argumentIsRef.size());

      for (size_t i = 0; i < match.arguments.size(); ++i) {
        auto argClone = match.arguments[i] ? match.arguments[i]->clone() : nullptr;
        if (i < resolvedSymbol->parameters.size() && argClone) {
          auto expected = resolvedSymbol->parameters[i]->type;
          if (!resolvedSymbol->parameters[i]->is_ref) {
            argClone = wrapInCast(std::move(argClone), expected);
          }
        }
        remappedArgs.push_back(std::move(argClone));
        remappedRef.push_back(i < match.argumentIsRef.size()
                                  ? match.argumentIsRef[i]
                                  : false);
      }

      match.arguments = std::move(remappedArgs);
      match.argumentIsRef = std::move(remappedRef);
      match.symbol = resolvedSymbol;
    }

    if (resolvedSymbol->isUnsafe && !isUnsafeActive()) {
      blockedUnsafeMatch = resolvedSymbol;
      rejectionNotes.push_back("'" + renderFunctionSignature(*resolvedSymbol) +
                               "': requires unsafe context");
      continue;
    }
    matches.push_back(std::move(match));
  }

  if (matches.empty()) {
    if (blockedUnsafeMatch) {
      requireUnsafeEnabled(node.span, "unsafe function calls");
      requireUnsafeContext(node.span, "unsafe function calls");
      return;
    }

    error(
        node.callee_->span,
        "No matching overload for function '" + calleeParts.back() + "'. " +
            (rejectionNotes.empty() ? std::string() : ("Candidates: " + [&]() {
              std::string details;
              for (size_t i = 0; i < rejectionNotes.size(); ++i) {
                if (i != 0) {
                  details += "; ";
                }
                details += rejectionNotes[i];
              }
              return details;
            }())));
    return;
  }

  size_t bestIndex = 0;
  for (size_t i = 1; i < matches.size(); ++i) {
    if (compareCost(matches[i], matches[bestIndex])) {
      bestIndex = i;
    }
  }

  std::vector<size_t> ambiguous = {bestIndex};
  for (size_t i = 0; i < matches.size(); ++i) {
    if (i == bestIndex) {
      continue;
    }
    if (!compareCost(matches[i], matches[bestIndex]) &&
        !compareCost(matches[bestIndex], matches[i])) {
      ambiguous.push_back(i);
    }
  }

  if (ambiguous.size() > 1) {
    std::string message =
        "Call to function '" + calleeParts.back() + "' is ambiguous between ";
    for (size_t i = 0; i < ambiguous.size(); ++i) {
      if (i != 0) {
        message += i + 1 == ambiguous.size() ? " and " : ", ";
      }
      message +=
          "'" + renderFunctionSignature(*matches[ambiguous[i]].symbol) + "'";
    }
    message += ".";
    if (expectedReturnType) {
      message +=
          " Expected result type: '" + expectedReturnType->toString() + "'.";
    }
    message += " Candidate details: ";
    for (size_t i = 0; i < ambiguous.size(); ++i) {
      if (i != 0) {
        message += "; ";
      }
      message +=
          "'" + renderFunctionSignature(*matches[ambiguous[i]].symbol) + "' [";
      for (size_t j = 0; j < matches[ambiguous[i]].notes.size(); ++j) {
        if (j != 0) {
          message += ", ";
        }
        message += matches[ambiguous[i]].notes[j];
      }
      message += "]";
    }
    error(node.callee_->span, message);
    return;
  }

  auto &best = matches[bestIndex];
  expressionStack_.push(std::make_unique<BoundFunctionCall>(
      best.symbol, std::move(best.arguments), std::move(best.argumentIsRef),
      std::move(best.variadicPack)));
}

bool Binder::bindWeakBuiltinCall(FunCall &node) {
  auto *calleeId = dynamic_cast<ConstId *>(node.callee_.get());
  if (!calleeId) {
    return false;
  }

  const bool isLock = calleeId->value_ == "lock";
  const bool isAlive = calleeId->value_ == "alive";
  if (!isLock && !isAlive) {
    return false;
  }

  if (node.params_.size() != 1 || node.params_[0]->isRef ||
      node.params_[0]->isSpread || !node.params_[0]->name.empty()) {
    error(node.span, "'" + calleeId->value_ +
                         "' expects exactly one positional argument.");
    return true;
  }

  auto weakExpr =
      bindExpressionWithExpected(node.params_[0]->value.get(), nullptr);
  if (!weakExpr) {
    return true;
  }

  if (weakExpr->type->getKind() != zir::TypeKind::Class) {
    error(node.params_[0]->value->span,
          "'" + calleeId->value_ + "' expects a weak class reference.");
    return true;
  }

  auto weakClassType = std::static_pointer_cast<zir::ClassType>(weakExpr->type);
  if (!weakClassType->isWeak()) {
    error(node.params_[0]->value->span,
          "'" + calleeId->value_ + "' expects a weak class reference.");
    return true;
  }

  if (isAlive) {
    expressionStack_.push(
        std::make_unique<BoundWeakAliveExpression>(std::move(weakExpr)));
    return true;
  }

  auto strongType = std::make_shared<zir::ClassType>(*weakClassType);
  strongType->setWeak(false);
  expressionStack_.push(std::make_unique<BoundWeakLockExpression>(
      std::move(weakExpr), strongType));
  return true;
}

void Binder::visit(NewExpr &node) {
  auto classType = mapType(*node.type_);
  if (!classType || classType->getKind() != zir::TypeKind::Class) {
    error(node.span, "'new' expects a class type.");
    return;
  }
  auto concreteType = std::static_pointer_cast<zir::ClassType>(classType);
  if (concreteType->isWeak()) {
    error(node.span, "'new' expects a strong class type, not 'weak'.");
    return;
  }
  auto infoIt = classInfos_.find(concreteType->getName());
  if (infoIt == classInfos_.end()) {
    error(node.span, "Unknown class type: " + concreteType->getName());
    return;
  }

  std::vector<std::unique_ptr<BoundExpression>> args;
  std::vector<bool> argRefs;
  auto ctor = infoIt->second.constructor;
  size_t ctorParamOffset = ctor && ctor->isMethod ? 1 : 0;
  size_t expectedArgCount =
      ctor ? (ctor->parameters.size() - ctorParamOffset) : 0;
  if (node.args_.size() != expectedArgCount) {
    error(node.span, "Constructor for class '" + concreteType->getName() +
                         "' expects " + std::to_string(expectedArgCount) +
                         " arguments, got " +
                         std::to_string(node.args_.size()) + ".");
    return;
  }

  for (size_t i = 0; i < node.args_.size(); ++i) {
    auto expected =
        ctor ? ctor->parameters[i + ctorParamOffset]->type : nullptr;
    auto arg = bindExpressionWithExpected(node.args_[i]->value.get(), expected);
    if (!arg) {
      return;
    }
    if (expected && !canConvert(arg->type, expected)) {
      error(node.args_[i]->value->span,
            "Cannot convert constructor argument from '" +
                renderTypeForUser(arg->type) + "' to '" +
                renderTypeForUser(expected) + "'");
      return;
    }
    if (expected) {
      arg = wrapInCast(std::move(arg), expected);
    }
    args.push_back(std::move(arg));
    argRefs.push_back(node.args_[i]->isRef);
  }

  expressionStack_.push(std::make_unique<BoundNewExpression>(
      concreteType, ctor, std::move(args), std::move(argRefs)));
}

void Binder::visit(IfNode &node) {
  node.condition_->accept(*this);
  if (expressionStack_.empty())
    return;

  auto cond = std::move(expressionStack_.top());
  expressionStack_.pop();

  if (cond->type->getKind() != zir::TypeKind::Bool) {
    error(node.condition_->span,
          "If condition must be Bool, got '" + renderTypeForUser(cond->type) +
              "'");
  }

  auto thenBody = bindBody(node.thenBody_.get(), true);

  std::unique_ptr<BoundBlock> elseBody = nullptr;
  if (node.elseBody_) {
    elseBody = bindBody(node.elseBody_.get(), true);
  }

  statementStack_.push(std::make_unique<BoundIfStatement>(
      std::move(cond), std::move(thenBody), std::move(elseBody)));
}

void Binder::visit(IfTypeNode &node) {
  if (activeGenericBindingsStack_.empty()) {
    error(node.span, "'iftype' can only be used inside a generic instantiation.");
    return;
  }

  std::shared_ptr<zir::Type> actualType = nullptr;
  for (auto it = activeGenericBindingsStack_.rbegin();
       it != activeGenericBindingsStack_.rend(); ++it) {
    auto bindingIt = it->find(node.parameterName_);
    if (bindingIt != it->end()) {
      actualType = bindingIt->second;
      break;
    }
  }
  if (!actualType) {
    error(node.span, "'iftype' expects an active generic type parameter, got '" +
                         node.parameterName_ + "'.");
    return;
  }

  auto matchType = mapType(*node.matchType_);
  if (!matchType) {
    error(node.matchType_->span,
          "Unknown type: " + node.matchType_->qualifiedName());
    return;
  }

  bool matched = renderTypeForUser(actualType) == renderTypeForUser(matchType);
  std::unique_ptr<BoundBlock> selectedBody = nullptr;
  if (matched) {
    selectedBody = bindBody(node.thenBody_.get(), true);
  } else if (node.elseBody_) {
    selectedBody = bindBody(node.elseBody_.get(), true);
  } else {
    selectedBody = std::make_unique<BoundBlock>();
  }

  statementStack_.push(std::move(selectedBody));
}

void Binder::visit(WhileNode &node) {
  node.condition_->accept(*this);
  if (expressionStack_.empty())
    return;

  auto cond = std::move(expressionStack_.top());
  expressionStack_.pop();

  if (cond->type->getKind() != zir::TypeKind::Bool) {
    error(node.condition_->span,
          "While condition must be Bool, got '" +
              renderTypeForUser(cond->type) + "'");
  }

  ++loopDepth_;
  auto body = bindBody(node.body_.get(), true);
  --loopDepth_;

  statementStack_.push(
      std::make_unique<BoundWhileStatement>(std::move(cond), std::move(body)));
}

void Binder::visit(BreakNode &node) {
  if (loopDepth_ <= 0) {
    error(node.span, "'break' can only be used inside loops.");
    return;
  }
  statementStack_.push(std::make_unique<BoundBreakStatement>());
}

void Binder::visit(ContinueNode &node) {
  if (loopDepth_ <= 0) {
    error(node.span, "'continue' can only be used inside loops.");
    return;
  }
  statementStack_.push(std::make_unique<BoundContinueStatement>());
}

void Binder::pushScope() {
  currentScope_ = std::make_shared<SymbolTable>(currentScope_);
}

void Binder::popScope() {
  if (currentScope_) {
    currentScope_ = currentScope_->getParent();
  }
}

std::optional<int64_t>
Binder::evaluateConstantInt(const BoundExpression *expr) {
  if (auto lit = dynamic_cast<const BoundLiteral *>(expr)) {
    try {
      const std::string &v = lit->value;
      if (v.size() > 2 && v[0] == '0') {
        if (v[1] == 'x' || v[1] == 'X') {
          return static_cast<int64_t>(std::stoull(v, nullptr, 16));
        }
        if (v[1] == 'b' || v[1] == 'B') {
          return static_cast<int64_t>(std::stoull(v.substr(2), nullptr, 2));
        }
        if (v[1] == 'o' || v[1] == 'O') {
          return static_cast<int64_t>(std::stoull(v.substr(2), nullptr, 8));
        }
      }
      return std::stoll(v);
    } catch (...) {
      return std::nullopt;
    }
  }

  if (auto unary = dynamic_cast<const BoundUnaryExpression *>(expr)) {
    auto inner = evaluateConstantInt(unary->expr.get());
    if (!inner)
      return std::nullopt;
    if (unary->op == "-")
      return -*inner;
    return inner;
  }

  if (auto binary = dynamic_cast<const BoundBinaryExpression *>(expr)) {
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
      return *right == 0 ? std::nullopt
                         : std::optional<int64_t>(*left / *right);
    if (binary->op == "%")
      return *right == 0 ? std::nullopt
                         : std::optional<int64_t>(*left % *right);
    if (binary->op == "&")
      return *left & *right;
    if (binary->op == "|")
      return *left | *right;
    if (binary->op == "^")
      return *left ^ *right;
    if (binary->op == "<<") {
      if (*right < 0)
        return std::nullopt;
      unsigned width = binary->left->type
                           ? static_cast<unsigned>(typeBitWidth(binary->left->type))
                           : 0u;
      if (width == 0 || static_cast<uint64_t>(*right) >= width)
        return std::nullopt;
      return static_cast<int64_t>(
          static_cast<uint64_t>(*left) << static_cast<uint64_t>(*right));
    }
    if (binary->op == ">>") {
      if (*right < 0)
        return std::nullopt;
      unsigned width = binary->left->type
                           ? static_cast<unsigned>(typeBitWidth(binary->left->type))
                           : 0u;
      if (width == 0 || static_cast<uint64_t>(*right) >= width)
        return std::nullopt;
      return *left >> static_cast<uint64_t>(*right);
    }
  }

  return std::nullopt;
}

std::shared_ptr<zir::Type> Binder::mapType(const TypeNode &typeNode) {
  if (typeNode.isVarArgs) {
    if (!typeNode.baseType)
      return nullptr;
    return mapType(*typeNode.baseType);
  }

  if (!activeGenericBindingsStack_.empty() && typeNode.qualifiers.empty() &&
      typeNode.genericArgs.empty()) {
    const auto &bindings = activeGenericBindingsStack_.back();
    auto it = bindings.find(typeNode.typeName);
    if (it != bindings.end()) {
      auto mapped = it->second;
      if (typeNode.isWeak) {
        if (!mapped || mapped->getKind() != zir::TypeKind::Class) {
          error(typeNode.span, "'weak' can only be used with class types.");
          return nullptr;
        }
        auto weakType = std::make_shared<zir::ClassType>(
            *std::static_pointer_cast<zir::ClassType>(mapped));
        weakType->setWeak(true);
        return weakType;
      }
      return mapped;
    }
  }

  if (typeNode.isArray) {
    if (!typeNode.baseType)
      return nullptr;
    auto base = mapType(*typeNode.baseType);
    size_t size = 0;

    if (typeNode.arraySize) {
      typeNode.arraySize->accept(*this);
      if (!expressionStack_.empty()) {
        auto boundSize = std::move(expressionStack_.top());
        expressionStack_.pop();
        auto evaluated = evaluateConstantInt(boundSize.get());
        if (evaluated) {
          size = static_cast<size_t>(*evaluated);
        } else {
          error(typeNode.span,
                "Array size must be a constant integer expression.");
        }
      }
    }

    return std::make_shared<zir::ArrayType>(std::move(base), size);
  }

  if (typeNode.isPointer) {
    requireUnsafeEnabled(typeNode.span, "raw pointer types");
    if (!typeNode.baseType)
      return nullptr;
    auto base = mapType(*typeNode.baseType);
    return std::make_shared<zir::PointerType>(std::move(base));
  }

  std::vector<std::string> parts = typeNode.qualifiers;
  parts.push_back(typeNode.typeName);
  auto symbol = resolveQualifiedSymbol(parts, typeNode.span, SymbolKind::Type);
  if (symbol && symbol->getKind() == SymbolKind::Type) {
    auto typeSymbol = std::static_pointer_cast<TypeSymbol>(symbol);
    if (!typeSymbol->genericParameterNames.empty()) {
      typeSymbol = instantiateGenericTypeSymbol(typeSymbol, typeNode);
      if (!typeSymbol) {
        return nullptr;
      }
    } else if (!typeNode.genericArgs.empty()) {
      error(typeNode.span, "Type '" + typeNode.qualifiedName() +
                               "' is not generic.");
      return nullptr;
    }
    if (typeSymbol->isUnsafe) {
      requireUnsafeEnabled(typeNode.span, "unsafe struct types");
      requireUnsafeContext(typeNode.span, "unsafe struct types");
    }
    if (typeNode.isWeak) {
      if (!typeSymbol->isClass ||
          typeSymbol->type->getKind() != zir::TypeKind::Class) {
        error(typeNode.span, "'weak' can only be used with class types.");
        return nullptr;
      }
      auto weakType = std::make_shared<zir::ClassType>(
          *std::static_pointer_cast<zir::ClassType>(typeSymbol->type));
      weakType->setWeak(true);
      return weakType;
    }
    return typeSymbol->type;
  }

  return nullptr;
}

void Binder::visit(ConstBool &node) {
  expressionStack_.push(std::make_unique<BoundLiteral>(
      node.value_ ? "true" : "false",
      std::make_shared<zir::PrimitiveType>(zir::TypeKind::Bool)));
}

void Binder::visit(UnaryExpr &node) {
  node.expr_->accept(*this);
  if (expressionStack_.empty())
    return;
  auto expr = std::move(expressionStack_.top());
  expressionStack_.pop();

  auto type = expr->type;
  if (node.op_ == "&") {
    requireUnsafeEnabled(node.span, "address-of");

    bool isLValue =
        dynamic_cast<BoundVariableExpression *>(expr.get()) ||
        dynamic_cast<BoundIndexAccess *>(expr.get()) ||
        dynamic_cast<BoundMemberAccess *>(expr.get()) ||
        (dynamic_cast<BoundUnaryExpression *>(expr.get()) &&
         static_cast<BoundUnaryExpression *>(expr.get())->op == "*");
    if (!isLValue) {
      error(node.span, "Cannot take the address of a non-lvalue expression.");
    }

    type = std::make_shared<zir::PointerType>(expr->type);
  } else if (node.op_ == "*") {
    requireUnsafeEnabled(node.span, "pointer dereference");
    requireUnsafeContext(node.span, "pointer dereference");
    if (!isPointerType(type)) {
      error(node.span,
            "Cannot dereference non-pointer type '" + renderTypeForUser(type) +
                "'");
    } else {
      type = std::static_pointer_cast<zir::PointerType>(type)->getBaseType();
      if (type->getKind() == zir::TypeKind::Void) {
        error(node.span, "Cannot dereference '*Void' directly. Cast it to a "
                         "concrete pointer type first.");
      }
    }
  } else if (node.op_ == "-" || node.op_ == "+") {
    if (!isNumeric(type)) {
      error(node.span, "Operator '" + node.op_ +
                           "' cannot be applied to type '" +
                           renderTypeForUser(type) + "'");
    }
  } else if (node.op_ == "!") {
    if (type->getKind() != zir::TypeKind::Bool) {
      error(node.span, "Operator '!' cannot be applied to type '" +
                           renderTypeForUser(type) + "'");
    }
  } else if (node.op_ == "~") {
    if (!type->isInteger()) {
      error(node.span, "Operator '~' cannot be applied to type '" +
                           renderTypeForUser(type) + "'");
    }
  }

  expressionStack_.push(
      std::make_unique<BoundUnaryExpression>(node.op_, std::move(expr), type));
}

void Binder::visit(ArrayLiteralNode &node) {
  std::vector<std::unique_ptr<BoundExpression>> elements;
  std::shared_ptr<zir::Type> elementType = nullptr;

  for (const auto &el : node.elements_) {
    el->accept(*this);
    if (!expressionStack_.empty()) {
      auto boundEl = std::move(expressionStack_.top());
      expressionStack_.pop();

      if (!elementType) {
        elementType = boundEl->type;
      } else if (!canConvert(boundEl->type, elementType)) {
        error(el->span, "Array elements must have the same type. Expected '" +
                            renderTypeForUser(elementType) + "', but got '" +
                            renderTypeForUser(boundEl->type) + "'");
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

void Binder::visit(RecordDecl &node) {
  auto symbol =
      std::dynamic_pointer_cast<TypeSymbol>(currentScope_->lookup(node.name_));
  if (!symbol || !symbol->genericParameterNames.empty()) {
    return;
  }
  auto recordType = std::static_pointer_cast<zir::RecordType>(symbol->type);

  for (const auto &field : node.fields_) {
    auto fieldType = mapType(*field->type);
    if (!fieldType) {
      error(field->span, "Unknown type: " + field->type->qualifiedName());
      fieldType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
    }
    recordType->addField(field->name, fieldType);
  }

  auto boundRecord = std::make_unique<BoundRecordDeclaration>();
  boundRecord->type = recordType;
  boundRoot_->records.push_back(std::move(boundRecord));
}

void Binder::visit(StructDeclarationNode &node) {
  auto symbol =
      std::dynamic_pointer_cast<TypeSymbol>(currentScope_->lookup(node.name_));
  if (!symbol || !symbol->genericParameterNames.empty()) {
    return;
  }
  auto recordType = std::static_pointer_cast<zir::RecordType>(symbol->type);
  int oldUnsafeTypeContextDepth = unsafeTypeContextDepth_;
  int oldExternTypeContextDepth = externTypeContextDepth_;
  if (node.isUnsafe_) {
    requireUnsafeEnabled(node.span, "'unsafe struct'");
    ++unsafeTypeContextDepth_;
  }
  ++externTypeContextDepth_;

  for (const auto &field : node.fields_) {
    auto fieldType = mapType(*field->type);
    if (!fieldType) {
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

void Binder::visit(StructLiteralNode &node) {
  if (!node.type_) {
    error(node.span, "Missing struct literal type.");
    return;
  }

  auto parts = splitQualified(node.type_->qualifiedName());
  auto symbol = resolveQualifiedSymbol(parts, node.span, SymbolKind::Type);
  if (!symbol || symbol->getKind() != SymbolKind::Type) {
    error(node.span, "Unknown type: " + node.type_->qualifiedName());
    return;
  }

  auto typeSymbol = std::static_pointer_cast<TypeSymbol>(symbol);
  auto mappedType = mapType(*node.type_);
  if (!mappedType || mappedType->getKind() != zir::TypeKind::Record) {
    error(node.span, "'" + node.type_->qualifiedName() + "' is not a struct.");
    return;
  }

  if (typeSymbol->isUnsafe) {
    requireUnsafeEnabled(node.span, "unsafe struct literals");
    requireUnsafeContext(node.span, "unsafe struct literals");
  }

  auto recordType = std::static_pointer_cast<zir::RecordType>(mappedType);
  std::vector<std::pair<std::string, std::unique_ptr<BoundExpression>>>
      boundFields;

  for (auto &fieldInit : node.fields_) {
    fieldInit.value->accept(*this);
    if (expressionStack_.empty())
      continue;
    auto boundVal = std::move(expressionStack_.top());
    expressionStack_.pop();

    bool found = false;
    for (const auto &f : recordType->getFields()) {
      if (f.name == fieldInit.name) {
        if (!canConvert(boundVal->type, f.type)) {
          error(node.span,
                "Cannot assign type '" + renderTypeForUser(boundVal->type) +
                    "' to field '" + f.name + "' of type '" +
                    renderTypeForUser(f.type) + "'");
        }
        found = true;
        break;
      }
    }

    if (!found) {
      error(node.span, "Field '" + fieldInit.name + "' not found in struct '" +
                           node.type_->qualifiedName() + "'");
    }

    boundFields.push_back({fieldInit.name, std::move(boundVal)});
  }

  for (const auto &f : recordType->getFields()) {
    bool initialized = false;
    for (const auto &bf : boundFields) {
      if (bf.first == f.name) {
        initialized = true;
        break;
      }
    }
    if (!initialized) {
      error(node.span,
            "Field '" + f.name + "' of struct '" + node.type_->qualifiedName() +
                "' is not initialized.");
    }
  }

  expressionStack_.push(
      std::make_unique<BoundStructLiteral>(std::move(boundFields), recordType));
}

void Binder::visit(EnumDecl &node) {
  auto symbol =
      std::dynamic_pointer_cast<TypeSymbol>(currentScope_->lookup(node.name_));
  auto enumType = std::static_pointer_cast<zir::EnumType>(symbol->type);

  auto boundEnum = std::make_unique<BoundEnumDeclaration>();
  boundEnum->type = enumType;
  boundRoot_->enums.push_back(std::move(boundEnum));
}

bool Binder::isNumeric(std::shared_ptr<zir::Type> type) const {
  return type->isInteger() || type->isFloatingPoint();
}

bool Binder::isPointerType(std::shared_ptr<zir::Type> type) const {
  return type && type->getKind() == zir::TypeKind::Pointer;
}

bool Binder::isNullType(std::shared_ptr<zir::Type> type) const {
  return type && type->getKind() == zir::TypeKind::NullPtr;
}

bool Binder::isUnsafeActive() const {
  return unsafeDepth_ > 0 || unsafeTypeContextDepth_ > 0;
}

void Binder::requireUnsafeEnabled(SourceSpan span, const std::string &feature) {
  if (!allowUnsafe_) {
    error(span, "Using " + feature + " requires '--allow-unsafe'.");
  }
}

void Binder::requireUnsafeContext(SourceSpan span, const std::string &feature) {
  if (!isUnsafeActive()) {
    error(span, "Using " + feature + " is only allowed inside unsafe code.");
  }
}

bool Binder::canConvert(std::shared_ptr<zir::Type> from,
                        std::shared_ptr<zir::Type> to) const {
  if (!from || !to)
    return false;
  if (from->getKind() == to->getKind() && from->toString() == to->toString())
    return true;
  if (isNullType(from) &&
      (isPointerType(to) || to->getKind() == zir::TypeKind::Class))
    return true;
  if (from->getKind() == zir::TypeKind::Class &&
      to->getKind() == zir::TypeKind::Class) {
    auto fromClass = std::static_pointer_cast<zir::ClassType>(from);
    auto toClass = std::static_pointer_cast<zir::ClassType>(to);
    if (fromClass->isWeak() && !toClass->isWeak()) {
      return false;
    }
    for (auto current = fromClass; current; current = current->getBase()) {
      if (current->getName() == toClass->getName()) {
        return true;
      }
    }
  }
  if (isNumeric(from) && isNumeric(to))
    return true;
  return false;
}

std::shared_ptr<zir::Type>
Binder::getPromotedType(std::shared_ptr<zir::Type> t1,
                        std::shared_ptr<zir::Type> t2) {
  if (t1->toString() == t2->toString())
    return t1;

  if (t1->isFloatingPoint() || t2->isFloatingPoint()) {
    if (t1->getKind() == zir::TypeKind::Float64 ||
        t2->getKind() == zir::TypeKind::Float64) {
      return std::make_shared<zir::PrimitiveType>(zir::TypeKind::Float64);
    }
    return std::make_shared<zir::PrimitiveType>(zir::TypeKind::Float);
  }

  // Integer promotion: preserve unsignedness when both operands are unsigned,
  // and choose width-aware integer kinds instead of always forcing Int64.
  if (isUnsignedIntegerType(t1) && isUnsignedIntegerType(t2)) {
    int width = std::max(typeBitWidth(t1), typeBitWidth(t2));
    if (width <= 8)
      return std::make_shared<zir::PrimitiveType>(zir::TypeKind::UInt8);
    if (width <= 16)
      return std::make_shared<zir::PrimitiveType>(zir::TypeKind::UInt16);
    if (width <= 32)
      return std::make_shared<zir::PrimitiveType>(zir::TypeKind::UInt);
    return std::make_shared<zir::PrimitiveType>(zir::TypeKind::UInt64);
  }

  // Mixed signed/unsigned or both signed:
  // - preserve unsignedness when the unsigned operand width is >= signed width
  //   (e.g. Int + UInt -> UInt, Int16 + UInt16 -> UInt16, Int + UInt64 -> UInt64)
  // - otherwise use signed, width-aware promotion.
  if ((isUnsignedIntegerType(t1) && isSignedIntegerType(t2)) ||
      (isSignedIntegerType(t1) && isUnsignedIntegerType(t2))) {
    auto unsignedType = isUnsignedIntegerType(t1) ? t1 : t2;
    auto signedType = isUnsignedIntegerType(t1) ? t2 : t1;

    int unsignedWidth = typeBitWidth(unsignedType);
    int signedWidth = typeBitWidth(signedType);

    if (unsignedWidth >= signedWidth) {
      if (unsignedWidth <= 8)
        return std::make_shared<zir::PrimitiveType>(zir::TypeKind::UInt8);
      if (unsignedWidth <= 16)
        return std::make_shared<zir::PrimitiveType>(zir::TypeKind::UInt16);
      if (unsignedWidth <= 32)
        return std::make_shared<zir::PrimitiveType>(zir::TypeKind::UInt);
      return std::make_shared<zir::PrimitiveType>(zir::TypeKind::UInt64);
    }

    int width = std::max(unsignedWidth, signedWidth);
    if (width <= 8)
      return std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int8);
    if (width <= 16)
      return std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int16);
    if (width <= 32)
      return std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int);
    return std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int64);
  }

  // Both signed: keep signed result, width-aware.
  int width = std::max(typeBitWidth(t1), typeBitWidth(t2));
  if (width <= 8)
    return std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int8);
  if (width <= 16)
    return std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int16);
  if (width <= 32)
    return std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int);
  return std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int64);
}

std::shared_ptr<zir::Type>
Binder::getCVariadicArgumentType(std::shared_ptr<zir::Type> type) {
  if (!type)
    return nullptr;

  if (isPointerType(type))
    return type;

  switch (type->getKind()) {
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
                   std::shared_ptr<zir::Type> targetType) {
  if (!expr || !targetType)
    return expr;
  if (expr->type->getKind() == targetType->getKind() &&
      expr->type->toString() == targetType->toString()) {
    return expr;
  }
  if (isNullType(expr->type) && isPointerType(targetType)) {
    return std::make_unique<BoundCast>(std::move(expr), targetType);
  }
  return std::make_unique<BoundCast>(std::move(expr), targetType);
}

void Binder::error(SourceSpan span, const std::string &message) {
  hadError_ = true;
  _diag.report(span, zap::DiagnosticLevel::Error, message);
}

} // namespace sema
