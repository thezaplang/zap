#include "../ast/class_decl.hpp"
#include "../ast/const/const_char.hpp"
#include "../ast/record_decl.hpp"
#include "../ir/string_type.hpp"
#include "binder.hpp"
#include "constant_evaluator.hpp"
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

void Binder::visit(RootNode &node) {
  for (const auto &child : node.children) {
    if (dynamic_cast<ImportNode *>(child.get())) {
      continue;
    }
    child->accept(*this);
  }
}

void Binder::visit(ImportNode &node) { (void)node; }

namespace {
bool matchesInterfaceMethodSignature(const FunctionSymbol &candidate,
                                     const FunctionSymbol &interfaceMethod) {
  if (candidate.parameters.size() != interfaceMethod.parameters.size()) {
    return false;
  }
  for (size_t i = 1; i < candidate.parameters.size(); ++i) {
    const auto &left = candidate.parameters[i];
    const auto &right = interfaceMethod.parameters[i];
    if (left->is_ref != right->is_ref || left->is_sink != right->is_sink ||
        !left->type || !right->type ||
        !zir::sameType(left->type, right->type)) {
      return false;
    }
  }
  if (!candidate.returnType || !interfaceMethod.returnType ||
      !zir::sameType(candidate.returnType, interfaceMethod.returnType)) {
    return false;
  }
  return true;
}
} // namespace

std::shared_ptr<zir::ClassType> Binder::resolveClassImplementsList(
    const ClassDecl &node,
    std::vector<std::shared_ptr<zir::ClassType>> &interfaces) {
  std::shared_ptr<zir::ClassType> base;
  for (const auto &typeNode : node.implementsList_) {
    auto resolved = mapType(*typeNode);
    if (!resolved) {
      error(typeNode->span, "Unknown type: " + typeNode->qualifiedName());
      continue;
    }
    if (resolved->getKind() != zir::TypeKind::Class) {
      error(typeNode->span, "'" + typeNode->qualifiedName() +
                                "' is not a class or an interface.");
      continue;
    }
    auto classType = std::static_pointer_cast<zir::ClassType>(resolved);
    if (classType->isInterface()) {
      interfaces.push_back(classType);
    } else if (base) {
      error(typeNode->span, "Class '" + node.name_ +
                                "' cannot have more than one base class.");
    } else {
      base = classType;
    }
  }
  return base;
}

void Binder::bindInterfaceConformances(
    const ClassDecl &node, const std::shared_ptr<zir::ClassType> &classType,
    ClassInfo &classInfo,
    const std::vector<std::shared_ptr<zir::ClassType>> &interfaces) {
  for (const auto &interfaceType : interfaces) {
    if (classType->implementsInterface(interfaceType->getCodegenName())) {
      continue;
    }
    auto infoIt = interfaceInfos_.find(interfaceType->getCodegenName());
    if (infoIt == interfaceInfos_.end()) {
      continue;
    }

    zir::ClassType::InterfaceConformance conformance;
    conformance.interfaceCodegenName = interfaceType->getCodegenName();
    bool ok = true;
    for (const auto &methodMeta : interfaceType->getInterfaceMethods()) {
      auto interfaceMethodIt = infoIt->second.methods.find(methodMeta.name);
      if (interfaceMethodIt == infoIt->second.methods.end()) {
        continue;
      }
      auto interfaceMethod =
          std::dynamic_pointer_cast<FunctionSymbol>(interfaceMethodIt->second);
      if (!interfaceMethod) {
        continue;
      }

      std::shared_ptr<FunctionSymbol> matched;
      auto methodIt = classInfo.methods.find(methodMeta.name);
      if (methodIt != classInfo.methods.end()) {
        for (const auto &overload : collectOverloads(methodIt->second)) {
          if (matchesInterfaceMethodSignature(*overload, *interfaceMethod)) {
            matched = overload;
            break;
          }
        }
      }
      if (!matched) {
        error(node.span, "Class '" + node.name_ + "' does not implement '" +
                             interfaceType->getName() + "." + methodMeta.name +
                             "'.");
        ok = false;
        continue;
      }
      conformance.methodVtableSlots.push_back(matched->vtableSlot);
    }
    if (ok) {
      classType->addInterfaceConformance(std::move(conformance));
    }
  }
}

void Binder::visit(ClassDecl &node) {
  auto symbol =
      std::dynamic_pointer_cast<TypeSymbol>(currentScope_->lookup(node.name_));
  if (!symbol || !symbol->isClass || !symbol->genericParameterNames.empty()) {
    return;
  }

  auto classType = std::static_pointer_cast<zir::ClassType>(symbol->type);
  auto &classInfo = classInfos_[classType->getCodegenName()];
  currentClassStack_.push_back(classType->getName());

  {
    std::vector<std::shared_ptr<zir::ClassType>> interfaces;
    auto baseClass = resolveClassImplementsList(node, interfaces);
    if (baseClass) {
      bool hasOwnCtor = false;
      bool hasOwnDtor = false;
      for (const auto &method : node.methods_) {
        hasOwnCtor = hasOwnCtor || method->name_ == "init";
        hasOwnDtor = hasOwnDtor || method->name_ == "deinit";
      }
      classType->setBase(baseClass);
      auto baseIt = classInfos_.find(baseClass->getCodegenName());
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
        field->name, fieldType, BindingKind::Mutable, false, field->name,
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

  if (!symbol->genericParameterNames.empty() &&
      !symbol->isGenericInstantiation) {
    return;
  }

  pushScope();
  auto oldFunction = currentFunction_;
  currentFunction_ = symbol;
  int oldUnsafeDepth = unsafeDepth_;
  if (node.isUnsafe_) {
    ++unsafeDepth_;
  }

  for (size_t i = 0; i < symbol->parameters.size(); ++i) {
    const auto &param = symbol->parameters[i];
    if (!currentScope_->declare(param->name, param)) {
      error(node.span, "Parameter '" + param->name + "' already declared.");
    }
    if (semanticInfo_ && i < node.params_.size() && node.params_[i]) {
    semanticInfo_->recordSymbol(node.params_[i].get(), param);
    semanticInfo_->recordDeclaration(node.params_[i].get(), param);
      semanticInfo_->recordType(node.params_[i].get(), param->type);
    }
  }

  auto boundBody = bindBody(node.body_.get(), false);

  popScope();
  currentFunction_ = oldFunction;
  unsafeDepth_ = oldUnsafeDepth;

  if (!symbol->returnType) {
    std::vector<BoundReturnStatement *> returns;
    std::function<void(BoundBlock *)> collectReturns =
        [&](BoundBlock *block) {
          if (!block)
            return;
          for (auto &stmt : block->statements) {
            if (auto *ret = dynamic_cast<BoundReturnStatement *>(stmt.get())) {
              returns.push_back(ret);
            } else if (auto *ifStmt =
                           dynamic_cast<BoundIfStatement *>(stmt.get())) {
              collectReturns(ifStmt->thenBody.get());
              collectReturns(ifStmt->elseBody.get());
            } else if (auto *whileStmt =
                           dynamic_cast<BoundWhileStatement *>(stmt.get())) {
              collectReturns(whileStmt->body.get());
            } else if (auto *forStmt =
                           dynamic_cast<BoundForStatement *>(stmt.get())) {
              collectReturns(forStmt->body.get());
            } else if (auto *nested =
                           dynamic_cast<BoundBlock *>(stmt.get())) {
              collectReturns(nested);
            }
          }
        };
    collectReturns(boundBody.get());

    if (returns.empty()) {
      symbol->returnType =
          std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
    } else {
      auto returnType = [](const BoundReturnStatement *ret) {
        return ret->expression
                   ? ret->expression->type
                   : std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
      };
      auto inferred = returnType(returns[0]);
      bool conflict = false;
      for (size_t i = 1; i < returns.size(); ++i) {
        auto candidate = returnType(returns[i]);
        auto join = conversions_.joinTypes(inferred, candidate);
        if (!join) {
          error(node.span, "Cannot infer return type of function '" +
                               node.name_ + "': conflicting return types '" +
                               renderTypeForUser(inferred) + "' and '" +
                               renderTypeForUser(candidate) +
                               "'. Add an explicit return type annotation.");
          conflict = true;
          break;
        }
        inferred = join->type;
      }
      if (!conflict) {
        for (auto *ret : returns) {
          if (!ret->expression) {
            continue;
          }
          auto conversion =
              conversions_.classifyImplicit(ret->expression->type, inferred);
          if (!conversion) {
            error(node.span,
                  "Internal error: inferred return type cannot represent all "
                  "return expressions.");
            conflict = true;
            break;
          }
          ret->expression =
              applyConversion(std::move(ret->expression), *conversion);
        }
      }
      symbol->returnType =
          conflict ? std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void)
                   : inferred;
    }
  }

  bool hasReturn = blockAlwaysReturns(boundBody.get());

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
    if (symbol->returnType->isInteger() ||
        symbol->returnType->isFloatingPoint() ||
        kind == zir::TypeKind::Bool) {
      std::string litVal = "0";
      if (symbol->returnType->isFloatingPoint())
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

void Binder::visit(BindingDecl &node) {
  auto existing = currentScope_->lookupLocal(node.name_);
  std::shared_ptr<VariableSymbol> symbol;
  if (existing) {
    symbol = std::dynamic_pointer_cast<VariableSymbol>(existing);
  }

  const bool isConstant = node.kind_ == BindingKind::CompileTimeConstant;
  const bool isImmutable = node.kind_ == BindingKind::Immutable;
  const bool isRef = !isConstant && node.type_ && node.type_->isReference;
  const std::string declarationKind =
      isConstant ? "constant"
                 : (isImmutable ? "immutable binding" : "variable");

  std::shared_ptr<zir::Type> type;
  std::unique_ptr<BoundExpression> initializer = nullptr;

  if (node.type_) {
    type = mapType(*node.type_);
    if (!type) {
      error(node.span, "Unknown type: " + node.type_->qualifiedName());
      type = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
    }

    if (node.initializer_) {
      initializer = bindExpressionWithExpected(node.initializer_.get(), type);
      if (initializer && !isRef) {
        auto conversion =
            conversions_.classifyImplicit(initializer->type, type);
        if (!conversion) {
          error(node.initializer_->span,
                "Cannot assign expression of type '" +
                    renderTypeForUser(initializer->type) + "' to " +
                    declarationKind + " of type '" + renderTypeForUser(type) +
                    "'");
        } else {
          initializer = applyConversion(std::move(initializer), *conversion);
        }
      }
    } else if (isConstant || isImmutable) {
      error(node.span,
            std::string(isConstant ? "Constant '" : "Immutable binding '") +
                node.name_ + "' must be initialized.");
    } else if (isRef) {
      error(node.span,
            "Reference variable '" + node.name_ + "' must be initialized.");
    }
  } else {
    if (!node.initializer_) {
      if (isConstant || isImmutable) {
        error(node.span,
              std::string(isConstant ? "Constant '" : "Immutable binding '") +
                  node.name_ + "' must be initialized.");
      } else {
        error(node.span, "Variable '" + node.name_ +
                             "' needs a type annotation or an initializer.");
      }
      type = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
    } else {
      initializer =
          bindExpressionWithExpected(node.initializer_.get(), nullptr);
      if (initializer && initializer->type &&
          initializer->type->getKind() != zir::TypeKind::Void) {
        type = initializer->type;
      } else {
        error(node.initializer_->span, "Cannot infer type of " +
                                           declarationKind + " '" + node.name_ +
                                           "' from a void expression.");
        type = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
      }
    }
  }

  if (isRef && initializer &&
      !requireMutablePlace(*initializer, node.span,
                           MutablePlaceUse::MutableReference)) {
    initializer.reset();
  }

  if (!symbol) {
    std::string linkName = node.name_;
    if (isConstant || node.isGlobal_) {
      linkName = node.isExternal_
                     ? node.name_
                     : mangleName(currentModuleLinkPath(), node.name_);
    }
    symbol = std::make_shared<VariableSymbol>(
        node.name_, type, node.kind_, isRef, std::move(linkName),
        modules_[currentModuleId_].info->moduleName, node.visibility_);
    symbol->is_external = node.isExternal_;
    if (!currentScope_->declare(node.name_, symbol)) {
      if (isConstant) {
        error(node.span, "Identifier '" + node.name_ + "' already declared.");
      } else {
        error(node.span, "Variable '" + node.name_ + "' already declared.");
      }
    }
  } else if (!node.type_ && type) {
    symbol->type = type;
  }
  symbol->is_ref = isRef;
  symbol->is_external = node.isExternal_;
  symbol->is_global = !currentBlock_;

  if (isConstant && initializer) {
    std::string failureReason;
    if (!ConstantEvaluator::isConstant(*initializer, &failureReason)) {
      error(node.initializer_->span,
            "Constant '" + node.name_ +
                "' must be initialized with a compile-time expression: " +
                failureReason + ".");
    } else {
      symbol->constant_value =
          std::shared_ptr<BoundExpression>(initializer->clone());
    }
  }
  if (semanticInfo_) {
    semanticInfo_->recordSymbol(&node, symbol);
    semanticInfo_->recordDeclaration(&node, symbol);
    semanticInfo_->recordType(&node, symbol->type);
  }

  auto boundDecl = std::make_unique<BoundVariableDeclaration>(
      symbol, std::move(initializer));

  if (currentBlock_ && !node.isGlobal_) {
    statementStack_.push(std::move(boundDecl));
  } else {
    boundRoot_->globals.push_back(std::move(boundDecl));
  }
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

void Binder::visit(EnumDecl &node) {
  auto symbol =
      std::dynamic_pointer_cast<TypeSymbol>(currentScope_->lookup(node.name_));
  if (symbol->type->getKind() == zir::TypeKind::TaggedUnion) {
    auto taggedUnionType =
        std::static_pointer_cast<zir::TaggedUnionType>(symbol->type);
    auto boundTaggedUnion = std::make_unique<BoundTaggedUnionDeclaration>();
    boundTaggedUnion->type = taggedUnionType;
    boundRoot_->taggedUnions.push_back(std::move(boundTaggedUnion));
    return;
  }

  auto enumType = std::static_pointer_cast<zir::EnumType>(symbol->type);
  auto boundEnum = std::make_unique<BoundEnumDeclaration>();
  boundEnum->type = enumType;
  boundRoot_->enums.push_back(std::move(boundEnum));
}
} // namespace sema
