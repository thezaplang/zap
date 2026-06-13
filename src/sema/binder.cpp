#include "binder.hpp"
#include "../ast/class_decl.hpp"
#include "../ast/const/const_char.hpp"
#include "../ast/record_decl.hpp"
#include "../utils/string_type_utils.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <functional>
#include <limits>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace sema {

std::string abiTypeKey(const std::shared_ptr<zir::Type> &type) {
  if (!type) {
    return "void";
  }

  switch (type->getKind()) {
  case zir::TypeKind::Void:
    return "v";
  case zir::TypeKind::Bool:
    return "b1";
  case zir::TypeKind::Char:
    return "c8";
  case zir::TypeKind::Int8:
    return "i8";
  case zir::TypeKind::Int16:
    return "i16";
  case zir::TypeKind::Int32:
    return "i32";
  case zir::TypeKind::Int64:
    return "i64";
  case zir::TypeKind::UInt8:
    return "u8";
  case zir::TypeKind::UInt16:
    return "u16";
  case zir::TypeKind::UInt32:
    return "u32";
  case zir::TypeKind::UInt64:
    return "u64";
  case zir::TypeKind::Int:
    return "is";
  case zir::TypeKind::UInt:
    return "us";
  case zir::TypeKind::Float:
  case zir::TypeKind::Float32:
    return "f32";
  case zir::TypeKind::Float64:
    return "f64";
  case zir::TypeKind::NullPtr:
    return "np";
  case zir::TypeKind::Pointer: {
    auto p = std::static_pointer_cast<zir::PointerType>(type);
    return "p_" + abiTypeKey(p->getBaseType());
  }
  case zir::TypeKind::Array: {
    auto a = std::static_pointer_cast<zir::ArrayType>(type);
    return "a" + std::to_string(a->getSize()) + "_" +
           abiTypeKey(a->getBaseType());
  }
  case zir::TypeKind::FunctionPointer: {
    auto fn = std::static_pointer_cast<zir::FunctionPointerType>(type);
    std::string out = "fp_";
    out += abiTypeKey(fn->getReturnType());
    out += "_";
    for (size_t i = 0; i < fn->getParams().size(); ++i) {
      if (i != 0) {
        out += "_";
      }
      out += abiTypeKey(fn->getParams()[i]);
    }
    return out;
  }
  case zir::TypeKind::Record: {
    auto r = std::static_pointer_cast<zir::RecordType>(type);
    return "r_" + sanitizeTypeName(r->getCodegenName());
  }
  case zir::TypeKind::Class: {
    auto c = std::static_pointer_cast<zir::ClassType>(type);
    return std::string(c->isWeak() ? "wc_" : "c_") +
           sanitizeTypeName(c->getCodegenName());
  }
  case zir::TypeKind::Enum: {
    auto e = std::static_pointer_cast<zir::EnumType>(type);
    return "e_" + sanitizeTypeName(e->getCodegenName());
  }
  }

  return sanitizeTypeName(type->toString());
}

bool isStringType(const std::shared_ptr<zir::Type> &type) {
  return zap::text::isStringType(type);
}

bool isFailableType(const std::shared_ptr<zir::Type> &type) {
  if (!type || type->getKind() != zir::TypeKind::Record) {
    return false;
  }

  auto record = std::static_pointer_cast<zir::RecordType>(type);
  const auto &name = record->getName();
  if (name.rfind(kFailablePrefix, 0) != 0) {
    return false;
  }

  const auto &fields = record->getFields();
  return fields.size() == 3 && fields[0].name == "ok" &&
         fields[1].name == "value" && fields[2].name == "error";
}

std::shared_ptr<zir::Type>
failableValueType(const std::shared_ptr<zir::Type> &type) {
  if (!isFailableType(type)) {
    return nullptr;
  }
  auto record = std::static_pointer_cast<zir::RecordType>(type);
  return record->getFields()[1].type;
}

std::shared_ptr<zir::Type>
failableErrorType(const std::shared_ptr<zir::Type> &type) {
  if (!isFailableType(type)) {
    return nullptr;
  }
  auto record = std::static_pointer_cast<zir::RecordType>(type);
  return record->getFields()[2].type;
}

std::shared_ptr<zir::RecordType>
makeFailableType(const std::shared_ptr<zir::Type> &valueType,
                 const std::shared_ptr<zir::Type> &errorType) {
  auto suffix = sanitizeTypeName((valueType ? valueType->toString() : "<?>") +
                                 std::string("$") +
                                 (errorType ? errorType->toString() : "<?>"));
  auto typeName = std::string(kFailablePrefix) + suffix;
  auto type = std::make_shared<zir::RecordType>(typeName, typeName);
  type->addField("ok",
                 std::make_shared<zir::PrimitiveType>(zir::TypeKind::Bool));
  type->addField("value", valueType);
  type->addField("error", errorType);
  return type;
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

std::unique_ptr<BoundExpression>
makeDefaultValueExpr(const std::shared_ptr<zir::Type> &type) {
  if (!type) {
    return std::make_unique<BoundLiteral>(
        "0", std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void));
  }

  switch (type->getKind()) {
  case zir::TypeKind::Bool:
    return std::make_unique<BoundLiteral>(
        "false", std::make_shared<zir::PrimitiveType>(zir::TypeKind::Bool));
  case zir::TypeKind::Float:
  case zir::TypeKind::Float32:
  case zir::TypeKind::Float64:
    return std::make_unique<BoundLiteral>("0.0", type);
  case zir::TypeKind::Pointer:
  case zir::TypeKind::NullPtr:
  case zir::TypeKind::Class:
    return std::make_unique<BoundLiteral>(
        "0", std::make_shared<zir::PrimitiveType>(zir::TypeKind::NullPtr));
  case zir::TypeKind::Record:
    if (isFailableType(type)) {
      auto okType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Bool);
      auto valueType = failableValueType(type);
      auto errorType = failableErrorType(type);
      std::vector<std::pair<std::string, std::unique_ptr<BoundExpression>>>
          fields;
      fields.push_back({"ok", std::make_unique<BoundLiteral>("false", okType)});
      fields.push_back({"value", makeDefaultValueExpr(valueType)});
      fields.push_back({"error", makeDefaultValueExpr(errorType)});
      return std::make_unique<BoundStructLiteral>(std::move(fields), type);
    }
    return std::make_unique<BoundLiteral>("0", type);
  default:
    if (type->isInteger() || type->getKind() == zir::TypeKind::Enum) {
      return std::make_unique<BoundLiteral>("0", type);
    }
    return std::make_unique<BoundLiteral>("0", type);
  }
}

std::unique_ptr<BoundExpression>
makeFailableValueExpr(std::unique_ptr<BoundExpression> valueExpr,
                      const std::shared_ptr<zir::Type> &failableType) {
  auto okType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Bool);
  auto valueType = failableValueType(failableType);
  auto errorType = failableErrorType(failableType);

  std::vector<std::pair<std::string, std::unique_ptr<BoundExpression>>> fields;
  fields.push_back({"ok", std::make_unique<BoundLiteral>("true", okType)});
  fields.push_back({"value", std::move(valueExpr)});
  fields.push_back({"error", makeDefaultValueExpr(errorType)});
  return std::make_unique<BoundStructLiteral>(std::move(fields), failableType);
}

std::unique_ptr<BoundExpression>
makeFailableErrorExpr(std::unique_ptr<BoundExpression> errorExpr,
                      const std::shared_ptr<zir::Type> &failableType) {
  auto okType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Bool);
  auto valueType = failableValueType(failableType);

  std::vector<std::pair<std::string, std::unique_ptr<BoundExpression>>> fields;
  fields.push_back({"ok", std::make_unique<BoundLiteral>("false", okType)});
  fields.push_back({"value", makeDefaultValueExpr(valueType)});
  fields.push_back({"error", std::move(errorExpr)});
  return std::make_unique<BoundStructLiteral>(std::move(fields), failableType);
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

bool blockAlwaysReturns(const BoundBlock *block) {
  if (!block)
    return false;
  if (block->result)
    return true;
  for (const auto &s : block->statements) {
    if (stmtAlwaysReturns(s.get()))
      return true;
  }
  return false;
}

bool stmtAlwaysReturns(const BoundStatement *stmt) {
  if (!stmt)
    return false;
  if (dynamic_cast<const BoundReturnStatement *>(stmt))
    return true;
  if (auto *blk = dynamic_cast<const BoundBlock *>(stmt))
    return blockAlwaysReturns(blk);
  if (auto *ifStmt = dynamic_cast<const BoundIfStatement *>(stmt))
    return ifStmt->elseBody && blockAlwaysReturns(ifStmt->thenBody.get()) &&
           blockAlwaysReturns(ifStmt->elseBody.get());
  return false;
}

std::unique_ptr<BoundExpression>
deriveValueExpressionFromIf(const BoundIfStatement &stmt) {
  if (!stmt.thenBody || !stmt.elseBody) {
    return nullptr;
  }

  auto thenExpr = deriveValueExpressionFromBlock(*stmt.thenBody);
  auto elseExpr = deriveValueExpressionFromBlock(*stmt.elseBody);
  if (!thenExpr || !elseExpr || !thenExpr->type || !elseExpr->type) {
    return nullptr;
  }

  if (thenExpr->type->toString() != elseExpr->type->toString()) {
    return nullptr;
  }
  auto resultType = thenExpr->type;

  return std::make_unique<BoundTernaryExpression>(
      stmt.condition->clone(), std::move(thenExpr), std::move(elseExpr),
      resultType);
}

std::unique_ptr<BoundExpression>
deriveValueExpressionFromBlock(const BoundBlock &block) {
  if (block.result && block.statements.empty()) {
    return block.result->clone();
  }

  if (block.result) {
    return nullptr;
  }

  if (block.statements.empty()) {
    return nullptr;
  }

  const auto *tail = block.statements.back().get();

  if (auto *exprStmt = dynamic_cast<const BoundExpressionStatement *>(tail)) {
    return exprStmt->expression ? exprStmt->expression->clone() : nullptr;
  }

  if (auto *ifStmt = dynamic_cast<const BoundIfStatement *>(tail)) {
    return deriveValueExpressionFromIf(*ifStmt);
  }

  return nullptr;
}

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
  syntheticLoopCounter_ = 0;
  unsafeDepth_ = 0;
  unsafeTypeContextDepth_ = 0;
  externTypeContextDepth_ = 0;
  sawPrivacyError_ = false;

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
  if (hadError_ || _diag.hadErrors()) {
    return nullptr;
  }

  for (auto &[_, module] : modules_) {
    applyImports(module, true);
  }
  if (hadError_ || _diag.hadErrors()) {
    return nullptr;
  }

  for (auto &[_, module] : modules_) {
    predeclareModuleAliases(module);
  }
  if (hadError_ || _diag.hadErrors()) {
    return nullptr;
  }

  for (auto &[_, module] : modules_) {
    applyImports(module, true);
  }
  if (hadError_ || _diag.hadErrors()) {
    return nullptr;
  }

  for (auto &[_, module] : modules_) {
    ensureModuleValuesReady(module);
  }
  if (hadError_ || _diag.hadErrors()) {
    return nullptr;
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
  if (hadError_ || _diag.hadErrors()) {
    return nullptr;
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
  auto appendEscaped = [&](char c) {
    unsigned char uc = static_cast<unsigned char>(c);
    if (std::isalnum(uc)) {
      mangled += c;
      return;
    }
    char buf[8];
    std::snprintf(buf, sizeof(buf), "_%02X", static_cast<unsigned int>(uc));
    mangled += buf;
  };
  for (char c : modulePath) {
    appendEscaped(c);
  }
  if (!mangled.empty() && mangled.back() != '$') {
    mangled += '$';
  }
  for (char c : name) {
    appendEscaped(c);
  }
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
    key += abiTypeKey(param->type);
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
    if (arr->getSize() == 0) {
      return "[]" + renderTypeForUser(arr->getBaseType());
    }
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

  auto symbol = lookupVisibleSymbol(parts.front());
  if (!symbol) {
    error(span, "Undefined identifier: " + parts.front());
    return nullptr;
  }

  for (size_t i = 1; i < parts.size(); ++i) {
    auto moduleSym = std::dynamic_pointer_cast<ModuleSymbol>(symbol);
    if (!moduleSym) {
      error(span, "'" + parts[i - 1] + "' is not a module.");
      return nullptr;
    }

    auto memberIt = moduleSym->exports.find(parts[i]);
    if (memberIt == moduleSym->exports.end()) {
      auto privateIt = moduleSym->members.find(parts[i]);
      if (privateIt != moduleSym->members.end()) {
        error(span, "Member '" + parts[i] + "' of module '" + moduleSym->name +
                        "' is private.");
      } else {
        error(span, "Module '" + moduleSym->name + "' has no member '" +
                        parts[i] + "'.");
      }
      return nullptr;
    }

    symbol = memberIt->second;
  }

  if (!allowAnyKind && symbol->getKind() != expectedKind &&
      !(expectedKind == SymbolKind::Function &&
        symbol->getKind() == SymbolKind::OverloadSet)) {
    return nullptr;
  }
  return symbol;
}

std::string Binder::makeSyntheticLoopName(std::string_view prefix) {
  while (true) {
    std::string candidate = "__for_" + std::string(prefix) + "_" +
                            std::to_string(syntheticLoopCounter_++);
    if (!currentScope_ || !currentScope_->lookupLocal(candidate)) {
      return candidate;
    }
  }
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
      unsigned width =
          binary->left->type
              ? static_cast<unsigned>(typeBitWidth(binary->left->type))
              : 0u;
      if (width == 0 || static_cast<uint64_t>(*right) >= width)
        return std::nullopt;
      return static_cast<int64_t>(static_cast<uint64_t>(*left)
                                  << static_cast<uint64_t>(*right));
    }
    if (binary->op == ">>") {
      if (*right < 0)
        return std::nullopt;
      unsigned width =
          binary->left->type
              ? static_cast<unsigned>(typeBitWidth(binary->left->type))
              : 0u;
      if (width == 0 || static_cast<uint64_t>(*right) >= width)
        return std::nullopt;
      return *left >> static_cast<uint64_t>(*right);
    }
  }

  return std::nullopt;
}

std::unique_ptr<BoundExpression>
Binder::foldConstantBinary(const BoundBinaryExpression *binary) {
  if (!binary)
    return nullptr;

  auto *left = dynamic_cast<const BoundLiteral *>(binary->left.get());
  auto *right = dynamic_cast<const BoundLiteral *>(binary->right.get());
  if (!left || !right)
    return nullptr;

  // String literal concatenation.
  if (binary->op == "+" && isStringType(binary->type)) {
    if (isStringType(left->type) && isStringType(right->type)) {
      return std::make_unique<BoundLiteral>(
          left->value + right->value,
          std::make_shared<zir::RecordType>("StringView", "StringView"));
    }
    return nullptr;
  }

  // Integer arithmetic/bitwise.
  if (binary->type && binary->type->isInteger()) {
    if (auto value = evaluateConstantInt(binary))
      return std::make_unique<BoundLiteral>(std::to_string(*value),
                                            binary->type);
  }

  return nullptr;
}

void Binder::error(SourceSpan span, const std::string &message) {
  if (message.find(" is private.") != std::string::npos) {
    sawPrivacyError_ = true;
  }

  if (sawPrivacyError_ &&
      (message.find("Undefined identifier: ") != std::string::npos ||
       message.find("Unknown type: ") != std::string::npos ||
       message.find("Unknown return type in function ") != std::string::npos ||
       message.find("Unknown generic type argument in type ") !=
           std::string::npos)) {
    return;
  }

  hadError_ = true;
  _diag.report(span, zap::DiagnosticLevel::Error, message);
}

} // namespace sema
