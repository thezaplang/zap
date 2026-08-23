#include "../ast/class_decl.hpp"
#include "../ast/const/const_char.hpp"
#include "../ast/record_decl.hpp"
#include "binder.hpp"
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

void Binder::visit(AsmStmtNode &node) {
  requireUnsafeContext(node.span, "inline 'asm'");

  std::vector<BoundAsmOperand> outputs;
  for (auto &operand : node.outputs) {
    operand.expr->accept(*this);
    if (expressionStack_.empty())
      return;
    auto bound = std::move(expressionStack_.top());
    expressionStack_.pop();

    if (!requireMutablePlace(*bound, node.span, MutablePlaceUse::AsmOutput)) {
      return;
    }
    outputs.push_back({operand.constraint, std::move(bound)});
  }

  std::vector<BoundAsmOperand> inputs;
  for (auto &operand : node.inputs) {
    operand.expr->accept(*this);
    if (expressionStack_.empty())
      return;
    auto bound = std::move(expressionStack_.top());
    expressionStack_.pop();
    inputs.push_back({operand.constraint, std::move(bound)});
  }

  statementStack_.push(std::make_unique<BoundAsmStatement>(
      node.assembly, std::move(outputs), std::move(inputs), node.clobbers));
}

void Binder::visit(ReturnNode &node) {
  std::unique_ptr<BoundExpression> expr = nullptr;
  bool expressionHadDiagnostic = false;
  if (node.returnValue) {
    bool hadErrorBefore = hadError_;
    expr = bindExpressionWithExpected(
        node.returnValue.get(),
        currentFunction_ ? currentFunction_->returnType : nullptr);
    expressionHadDiagnostic = (hadError_ != hadErrorBefore);
    if (!expr) {
      // The return expression already produced a diagnostic.
      // Avoid cascading with a secondary "received Void" return-type error.
      statementStack_.push(std::make_unique<BoundReturnStatement>(
          nullptr, currentFunction_ && currentFunction_->returnsRef));
      return;
    }
  }

  if (currentFunction_ && currentFunction_->returnType) {
    auto expectedType = currentFunction_->returnType;
    auto actualType =
        expr ? expr->type
             : std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);

    if (isFailableType(expectedType) && !isFailableType(actualType)) {
      auto expectedValueType = failableValueType(expectedType);
      auto conversion =
          conversions_.classifyImplicit(actualType, expectedValueType);
      if (!conversion) {
        error(node.span,
              "Function '" + currentFunction_->name +
                  "' expects return type '" + renderTypeForUser(expectedType) +
                  "', but received '" + renderTypeForUser(actualType) + "'");
      } else if (expr) {
        expr = applyConversion(std::move(expr), *conversion);
        expr = makeFailableValueExpr(std::move(expr), expectedType);
      } else {
        expr = makeFailableValueExpr(makeDefaultValueExpr(expectedValueType),
                                     expectedType);
      }
    } else {
      auto conversion = conversions_.classifyImplicit(actualType, expectedType);
      if (!conversion) {
        if (expressionHadDiagnostic) {
          statementStack_.push(std::make_unique<BoundReturnStatement>(
              std::move(expr),
              currentFunction_ && currentFunction_->returnsRef));
          return;
        }
        error(node.span,
              "Function '" + currentFunction_->name +
                  "' expects return type '" + renderTypeForUser(expectedType) +
                  "', but received '" + renderTypeForUser(actualType) + "'");
      } else if (expr) {
        expr = applyConversion(std::move(expr), *conversion);
      }
    }
  }

  if (expr && currentFunction_ && currentFunction_->returnsRef &&
      !requireMutablePlace(*expr, node.span,
                           MutablePlaceUse::MutableReference)) {
    statementStack_.push(std::make_unique<BoundReturnStatement>(
        std::move(expr), currentFunction_->returnsRef));
    return;
  }

  statementStack_.push(std::make_unique<BoundReturnStatement>(
      std::move(expr), currentFunction_ && currentFunction_->returnsRef));
}

void Binder::visit(FailNode &node) {
  if (!currentFunction_ || !isFailableType(currentFunction_->returnType)) {
    error(node.span, "'fail' can only be used inside failable functions.");
    return;
  }

  auto propagatedType = currentFunction_->returnType;
  auto expectedErrorType = failableErrorType(propagatedType);

  auto errExpr =
      bindExpressionWithExpected(node.errorValue_.get(), expectedErrorType);
  if (!errExpr) {
    return;
  }

  auto conversion =
      conversions_.classifyImplicit(errExpr->type, expectedErrorType);
  if (!conversion) {
    error(node.errorValue_->span,
          "Cannot fail with error type '" + renderTypeForUser(errExpr->type) +
              "', expected '" + renderTypeForUser(expectedErrorType) + "'");
    return;
  }
  errExpr = applyConversion(std::move(errExpr), *conversion);

  statementStack_.push(std::make_unique<BoundFailStatement>(
      std::move(errExpr), propagatedType, expectedErrorType));
}

void Binder::visit(IfNode &node) {
  node.condition_->accept(*this);
  if (expressionStack_.empty())
    return;

  auto cond = std::move(expressionStack_.top());
  expressionStack_.pop();

  if (cond->type->getKind() != zir::TypeKind::Bool) {
    error(node.condition_->span, "If condition must be Bool, got '" +
                                     renderTypeForUser(cond->type) + "'");
  }

  std::shared_ptr<VariableSymbol> narrowedSource;
  std::shared_ptr<VariableSymbol> narrowedVariable;
  BoundClassTypeTest *typeTest = dynamic_cast<BoundClassTypeTest *>(cond.get());
  if (!typeTest) {
    if (auto *binary = dynamic_cast<BoundBinaryExpression *>(cond.get());
        binary && binary->op == "&&") {
      typeTest = dynamic_cast<BoundClassTypeTest *>(binary->left.get());
    }
  }
  if (typeTest) {
    if (auto *variable = dynamic_cast<BoundVariableExpression *>(
            typeTest->expression.get())) {
      narrowedSource = variable->symbol;
      narrowedVariable = std::make_shared<VariableSymbol>(*narrowedSource);
      narrowedVariable->type = typeTest->targetType;
    }
  }

  std::unique_ptr<BoundBlock> thenBody;
  if (narrowedVariable) {
    pushScope();
    currentScope_->declare(narrowedVariable->name, narrowedVariable);
    thenBody = bindBody(node.thenBody_.get(), false);
    popScope();
  } else {
    thenBody = bindBody(node.thenBody_.get(), true);
  }

  std::unique_ptr<BoundBlock> elseBody = nullptr;
  if (node.elseBody_) {
    elseBody = bindBody(node.elseBody_.get(), true);
  }

  statementStack_.push(std::make_unique<BoundIfStatement>(
      std::move(cond), std::move(thenBody), std::move(elseBody),
      std::move(narrowedSource), std::move(narrowedVariable)));
}

void Binder::visit(CaseNode &node) {
  auto scrutinee = bindExpressionWithExpected(node.scrutinee.get(), nullptr);
  if (!scrutinee) {
    return;
  }

  const auto scrutineeType = scrutinee->type;
  const bool supportsCasePatterns =
      scrutineeType->isInteger() ||
      scrutineeType->getKind() == zir::TypeKind::Bool ||
      scrutineeType->getKind() == zir::TypeKind::Char ||
      isStringType(scrutineeType) ||
      scrutineeType->getKind() == zir::TypeKind::Record ||
      scrutineeType->getKind() == zir::TypeKind::Enum ||
      scrutineeType->getKind() == zir::TypeKind::TaggedUnion;
  if (!supportsCasePatterns) {
    error(node.scrutinee->span, "Case scrutinee must be an integer, Bool, "
                                "Char, String, Record/struct, or enum; got '" +
                                    renderTypeForUser(scrutineeType) + "'.");
    for (const auto &arm : node.arms) {
      bindBody(arm.body.get(), true);
    }
    return;
  }

  auto normalizeIntegerPattern = [&](int64_t value,
                                     const std::shared_ptr<zir::Type> &type)
      -> std::optional<std::string> {
    const int width = typeBitWidth(type);
    if (width <= 0 || width > 64) {
      return std::nullopt;
    }

    if (type->isUnsigned()) {
      if (value < 0) {
        return std::nullopt;
      }
      const uint64_t maximum = width == 64
                                   ? std::numeric_limits<uint64_t>::max()
                                   : (uint64_t{1} << width) - 1;
      const uint64_t normalized = static_cast<uint64_t>(value);
      if (normalized > maximum) {
        return std::nullopt;
      }
      return std::to_string(normalized);
    }

    const int64_t minimum = width == 64 ? std::numeric_limits<int64_t>::min()
                                        : -(int64_t{1} << (width - 1));
    const int64_t maximum = width == 64 ? std::numeric_limits<int64_t>::max()
                                        : (int64_t{1} << (width - 1)) - 1;
    if (value < minimum || value > maximum) {
      return std::nullopt;
    }
    return std::to_string(value);
  };

  bool sawElse = false;
  std::unordered_set<std::string> seenPatterns;
  std::unordered_set<int64_t> coveredVariants;
  bool hasRecordPattern = false;
  std::vector<BoundCaseArm> boundArms;
  boundArms.reserve(node.arms.size());
  for (const auto &arm : node.arms) {
    std::vector<BoundCasePattern> boundPatterns;
    std::shared_ptr<VariableSymbol> payloadBinding;
    std::vector<std::shared_ptr<VariableSymbol>> recordBindings;
    if (arm.isElse) {
      if (sawElse) {
        error(arm.span, "Duplicate 'else' case arm.");
      }
      sawElse = true;
    } else {
      if (sawElse) {
        error(arm.span, "Case arm after 'else' is unreachable.");
      }

      for (const auto &pattern : arm.patterns) {
        if (pattern.kind == CasePatternKind::Record) {
          if (scrutineeType->getKind() != zir::TypeKind::Record ||
              arm.patterns.size() != 1) {
            error(pattern.span, "Record patterns require a Record/struct "
                                "scrutinee and no alternatives.");
            continue;
          }
          auto typeSymbol = resolveQualifiedSymbol(
              pattern.recordPath, pattern.span, SymbolKind::Type);
          if (!typeSymbol || !zir::sameType(typeSymbol->type, scrutineeType)) {
            error(pattern.span,
                  "Record pattern does not match scrutinee type '" +
                      renderTypeForUser(scrutineeType) + "'.");
            continue;
          }
          auto bindRecord = [&](const auto &self, const CasePattern &record,
                                std::shared_ptr<zir::RecordType> type)
              -> std::unique_ptr<BoundCasePattern> {
            std::vector<BoundCaseRecordField> fields;
            std::unordered_set<std::string> names;
            for (const auto &field : record.recordFields) {
              if (!names.insert(field.name).second) {
                error(field.span,
                      "Duplicate record pattern field '" + field.name + "'.");
                continue;
              }
              int index = -1;
              std::shared_ptr<zir::Type> fieldType;
              for (size_t i = 0; i < type->getFields().size(); ++i) {
                if (type->getFields()[i].name == field.name) {
                  index = static_cast<int>(i);
                  fieldType = type->getFields()[i].type;
                  break;
                }
              }
              if (index < 0) {
                error(field.span, "Record type '" + type->getName() +
                                      "' has no field '" + field.name + "'.");
                continue;
              }
              if (field.nested) {
                if (field.nested->kind == CasePatternKind::Record) {
                  if (fieldType->getKind() != zir::TypeKind::Record) {
                    error(field.span, "Nested record pattern requires a "
                                      "Record/struct field.");
                    continue;
                  }
                  auto nestedSymbol = resolveQualifiedSymbol(
                      field.nested->recordPath, field.nested->span,
                      SymbolKind::Type);
                  if (!nestedSymbol ||
                      !zir::sameType(nestedSymbol->type, fieldType)) {
                    error(field.nested->span,
                          "Nested record pattern does not match field '" +
                              field.name + "'.");
                    continue;
                  }
                  fields.push_back(
                      {index,
                       self(self, *field.nested,
                            std::static_pointer_cast<zir::RecordType>(
                                fieldType)),
                       nullptr});
                  continue;
                }

                if (field.nested->kind == CasePatternKind::Variant) {
                  const auto &path = field.nested->variantPath;
                  if (fieldType->getKind() != zir::TypeKind::Enum ||
                      path.size() < 2) {
                    error(field.nested->span,
                          "Enum field pattern does not match field '" +
                              field.name + "'.");
                    continue;
                  }
                  std::vector<std::string> typePath(path.begin(),
                                                    path.end() - 1);
                  auto enumSymbol = resolveQualifiedSymbol(
                      typePath, field.nested->span, SymbolKind::Type);
                  if (!enumSymbol ||
                      !zir::sameType(enumSymbol->type, fieldType)) {
                    error(field.nested->span,
                          "Enum field pattern does not match field '" +
                              field.name + "'.");
                    continue;
                  }
                  auto enumType =
                      std::static_pointer_cast<zir::EnumType>(fieldType);
                  const auto tag =
                      enumType->getVariantDiscriminant(path.back());
                  if (tag < 0 || field.nested->payloadKind !=
                                     CasePayloadPatternKind::None) {
                    error(field.nested->span,
                          "Invalid enum field pattern for '" + field.name +
                              "'.");
                    continue;
                  }
                  fields.push_back({index,
                                    std::make_unique<BoundCasePattern>(
                                        BoundCasePatternKind::EnumVariant, tag),
                                    nullptr});
                  continue;
                }

                if (field.nested->kind != CasePatternKind::Literal) {
                  error(field.nested->span,
                        "Record fields currently support literal, enum, or "
                        "record patterns.");
                  continue;
                }
                auto value = bindExpressionWithExpected(
                    field.nested->literal.get(), fieldType);
                if (!value) {
                  continue;
                }
                if (fieldType->isInteger()) {
                  auto integerValue = evaluateConstantInt(value.get());
                  if (!integerValue ||
                      !normalizeIntegerPattern(*integerValue, fieldType)) {
                    error(field.nested->span, "Record field pattern is not "
                                              "representable by field type '" +
                                                  renderTypeForUser(fieldType) +
                                                  "'.");
                    continue;
                  }
                  if (!zir::sameType(value->type, fieldType)) {
                    value = std::make_unique<BoundCast>(std::move(value),
                                                        fieldType);
                  }
                } else if (auto *literal =
                               dynamic_cast<BoundLiteral *>(value.get())) {
                  auto conversion =
                      conversions_.classifyImplicit(value->type, fieldType);
                  if (!conversion) {
                    error(field.nested->span,
                          "Record field pattern type '" +
                              renderTypeForUser(value->type) +
                              "' does not match field type '" +
                              renderTypeForUser(fieldType) + "'.");
                    continue;
                  }
                  value = applyConversion(std::move(value), *conversion);
                } else {
                  error(field.nested->span,
                        "Record field pattern must be a literal.");
                  continue;
                }
                fields.push_back(
                    {index,
                     std::make_unique<BoundCasePattern>(std::move(value)),
                     nullptr});
              } else {
                auto binding = std::make_shared<VariableSymbol>(
                    field.binding, fieldType, BindingKind::Immutable, false,
                    field.binding, currentModuleId_);
                recordBindings.push_back(binding);
                fields.push_back({index, nullptr, std::move(binding)});
              }
            }
            return std::make_unique<BoundCasePattern>(type, std::move(fields));
          };
          auto isIrrefutableRecord = [&](const auto &self,
                                         const CasePattern &record) -> bool {
            for (const auto &field : record.recordFields) {
              if (field.nested &&
                  (field.nested->kind != CasePatternKind::Record ||
                   !self(self, *field.nested))) {
                return false;
              }
            }
            return true;
          };
          auto boundRecord = bindRecord(
              bindRecord, pattern,
              std::static_pointer_cast<zir::RecordType>(scrutineeType));
          boundPatterns.push_back(std::move(*boundRecord));
          hasRecordPattern = hasRecordPattern ||
                             isIrrefutableRecord(isIrrefutableRecord, pattern);
          continue;
        }
        if (pattern.kind == CasePatternKind::Variant) {
          const auto &path = pattern.variantPath;
          if (path.size() < 2) {
            error(pattern.span,
                  "Case variant does not belong to scrutinee type '" +
                      renderTypeForUser(scrutineeType) + "'.");
            continue;
          }

          std::vector<std::string> typePath(path.begin(), path.end() - 1);
          auto typeSymbol =
              resolveQualifiedSymbol(typePath, pattern.span, SymbolKind::Type);
          if (!typeSymbol || !zir::sameType(typeSymbol->type, scrutineeType)) {
            error(pattern.span,
                  "Case variant does not belong to scrutinee type '" +
                      renderTypeForUser(scrutineeType) + "'.");
            continue;
          }

          const auto &variantName = path.back();
          if (scrutineeType->getKind() == zir::TypeKind::Enum) {
            auto enumType =
                std::static_pointer_cast<zir::EnumType>(scrutineeType);
            const auto tag = enumType->getVariantDiscriminant(variantName);
            if (tag < 0) {
              error(pattern.span, "Enum '" + enumType->getName() +
                                      "' has no variant '" + variantName +
                                      "'.");
              continue;
            }
            if (pattern.payloadKind != CasePayloadPatternKind::None) {
              error(pattern.span, "Enum variant '" + variantName +
                                      "' does not take a payload pattern.");
              continue;
            }
            const auto key = "enum:" + std::to_string(tag);
            if (!seenPatterns.insert(key).second) {
              error(pattern.span, "Duplicate case pattern.");
            }
            coveredVariants.insert(tag);
            boundPatterns.emplace_back(BoundCasePatternKind::EnumVariant, tag);
            continue;
          }

          auto taggedUnion =
              std::static_pointer_cast<zir::TaggedUnionType>(scrutineeType);
          const auto *variant = taggedUnion->findVariant(variantName);
          if (!variant) {
            error(pattern.span, "Enum '" + taggedUnion->getName() +
                                    "' has no variant '" + variantName + "'.");
            continue;
          }
          if (!variant->payloadType &&
              pattern.payloadKind != CasePayloadPatternKind::Empty) {
            error(pattern.span,
                  "Enum variant '" + variantName +
                      "' requires an empty payload pattern '()'.");
            continue;
          }
          if (variant->payloadType &&
              pattern.payloadKind != CasePayloadPatternKind::Binding &&
              pattern.payloadKind != CasePayloadPatternKind::Wildcard) {
            error(pattern.span, "Enum variant '" + variantName +
                                    "' expects one payload binding.");
            continue;
          }
          if (pattern.payloadKind == CasePayloadPatternKind::Binding) {
            if (arm.patterns.size() != 1) {
              error(pattern.span, "A case arm with a payload binding cannot "
                                  "have alternatives.");
              continue;
            }
            payloadBinding = std::make_shared<VariableSymbol>(
                pattern.payloadBinding, variant->payloadType,
                BindingKind::Immutable, false, pattern.payloadBinding,
                currentModuleId_);
          }
          const auto key = "union:" + std::to_string(variant->tag);
          if (!seenPatterns.insert(key).second) {
            error(pattern.span, "Duplicate case pattern.");
          }
          coveredVariants.insert(variant->tag);
          boundPatterns.emplace_back(BoundCasePatternKind::TaggedUnionVariant,
                                     variant->tag, variant->payloadType);
          continue;
        }

        if (scrutineeType->getKind() == zir::TypeKind::Enum ||
            scrutineeType->getKind() == zir::TypeKind::TaggedUnion) {
          error(pattern.span, "Case enum patterns must name a variant.");
          continue;
        }

        auto value =
            bindExpressionWithExpected(pattern.literal.get(), scrutineeType);
        if (!value) {
          continue;
        }

        std::string patternKey = renderTypeForUser(scrutineeType) + ":";
        if (scrutineeType->isInteger()) {
          auto integerValue = evaluateConstantInt(value.get());
          if (!integerValue) {
            error(pattern.span, "Case integer pattern must be constant.");
            continue;
          }
          auto normalized =
              normalizeIntegerPattern(*integerValue, scrutineeType);
          if (!normalized) {
            error(
                pattern.span,
                "Case pattern value is not representable by scrutinee type '" +
                    renderTypeForUser(scrutineeType) + "'.");
            continue;
          }
          patternKey += *normalized;

          if (!zir::sameType(value->type, scrutineeType)) {
            value =
                std::make_unique<BoundCast>(std::move(value), scrutineeType);
          }
        } else if (auto *literal = dynamic_cast<BoundLiteral *>(value.get())) {
          auto conversion =
              conversions_.classifyImplicit(value->type, scrutineeType);
          if (!conversion) {
            error(pattern.span, "Case pattern type '" +
                                    renderTypeForUser(value->type) +
                                    "' does not match scrutinee type '" +
                                    renderTypeForUser(scrutineeType) + "'.");
            continue;
          }
          patternKey += literal->value;
          value = applyConversion(std::move(value), *conversion);
        } else {
          error(pattern.span, "Case pattern must be a literal.");
          continue;
        }

        if (!seenPatterns.insert(patternKey).second) {
          error(pattern.span, "Duplicate case pattern.");
        }

        boundPatterns.emplace_back(std::move(value));
      }
    }

    std::unique_ptr<BoundBlock> body;
    if (payloadBinding || !recordBindings.empty()) {
      pushScope();
      if (payloadBinding &&
          !currentScope_->declare(payloadBinding->name, payloadBinding)) {
        error(arm.span, "Duplicate payload binding '" + payloadBinding->name +
                            "' in case arm.");
      }
      for (const auto &binding : recordBindings) {
        if (!currentScope_->declare(binding->name, binding)) {
          error(arm.span, "Duplicate record binding '" + binding->name +
                              "' in case arm.");
        }
      }
      body = bindBody(arm.body.get(), false);
      popScope();
    } else {
      body = bindBody(arm.body.get(), true);
    }
    boundArms.emplace_back(arm.isElse, std::move(boundPatterns),
                           std::move(payloadBinding), std::move(recordBindings),
                           std::move(body));
  }
  bool exhaustive = false;
  if (scrutineeType->getKind() == zir::TypeKind::Enum) {
    const auto &variants =
        std::static_pointer_cast<zir::EnumType>(scrutineeType)->getVariants();
    exhaustive =
        std::all_of(variants.begin(), variants.end(), [&](const auto &variant) {
          return coveredVariants.count(variant.discriminant) != 0;
        });
  } else if (scrutineeType->getKind() == zir::TypeKind::TaggedUnion) {
    const auto &variants =
        std::static_pointer_cast<zir::TaggedUnionType>(scrutineeType)
            ->getVariants();
    exhaustive =
        std::all_of(variants.begin(), variants.end(), [&](const auto &variant) {
          return coveredVariants.count(variant.tag) != 0;
        });
  }
  exhaustive = exhaustive || hasRecordPattern;

  if (!sawElse && !exhaustive) {
    error(node.span, "Case statement requires an 'else' arm unless its "
                     "patterns are exhaustive.");
  }
  if (sawElse && exhaustive) {
    error(node.span, "'else' case arm is unreachable because earlier patterns "
                     "are exhaustive.");
  }

  statementStack_.push(std::make_unique<BoundCaseStatement>(
      std::move(scrutinee), std::move(boundArms), sawElse || exhaustive));
}

void Binder::visit(IfTypeNode &node) {
  if (activeGenericBindingsStack_.empty()) {
    error(node.span,
          "'iftype' can only be used inside a generic instantiation.");
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
    error(node.span,
          "'iftype' expects an active generic type parameter, got '" +
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
    error(node.condition_->span, "While condition must be Bool, got '" +
                                     renderTypeForUser(cond->type) + "'");
  }

  ++loopDepth_;
  auto body = bindBody(node.body_.get(), true);
  --loopDepth_;

  statementStack_.push(
      std::make_unique<BoundWhileStatement>(std::move(cond), std::move(body)));
}

void Binder::visit(ForNode &node) {
  pushScope();

  node.initializer_->accept(*this);
  std::unique_ptr<BoundStatement> initializer = nullptr;
  if (!statementStack_.empty()) {
    initializer = std::move(statementStack_.top());
    statementStack_.pop();
  }

  node.condition_->accept(*this);
  if (expressionStack_.empty()) {
    popScope();
    return;
  }

  auto condition = std::move(expressionStack_.top());
  expressionStack_.pop();
  if (condition->type->getKind() != zir::TypeKind::Bool) {
    error(node.condition_->span, "For condition must be Bool, got '" +
                                     renderTypeForUser(condition->type) + "'");
  }

  auto incrementTargetId =
      dynamic_cast<ConstId *>(node.increment_->target_.get());
  if (!incrementTargetId ||
      incrementTargetId->value_ != node.initializer_->name_) {
    error(node.increment_->target_->span,
          "For increment must assign to loop variable '" +
              node.initializer_->name_ + "'.");
  }

  node.increment_->accept(*this);
  std::unique_ptr<BoundStatement> increment = nullptr;
  if (!statementStack_.empty()) {
    increment = std::move(statementStack_.top());
    statementStack_.pop();
  }

  ++loopDepth_;
  auto body = bindBody(node.body_.get(), true);
  --loopDepth_;

  popScope();
  statementStack_.push(std::make_unique<BoundForStatement>(
      std::move(initializer), std::move(condition), std::move(increment),
      std::move(body)));
}

void Binder::visit(ForInNode &node) {
  const std::string moduleName =
      (modules_.count(currentModuleId_) && modules_[currentModuleId_].info)
          ? modules_[currentModuleId_].info->moduleName
          : "";

  pushScope();

  node.iterable_->accept(*this);
  if (expressionStack_.empty()) {
    popScope();
    return;
  }

  auto iterableValue = std::move(expressionStack_.top());
  expressionStack_.pop();

  auto intType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int);

  auto iterableName = makeSyntheticLoopName("iter");
  auto iterableSymbol = std::make_shared<VariableSymbol>(
      iterableName, iterableValue->type, BindingKind::Mutable, false,
      iterableName, moduleName, Visibility::Private);
  currentScope_->declare(iterableName, iterableSymbol);

  auto indexName = makeSyntheticLoopName("idx");
  auto indexSymbol = std::make_shared<VariableSymbol>(
      indexName, intType, BindingKind::Mutable, false, indexName, moduleName,
      Visibility::Private);
  currentScope_->declare(indexName, indexSymbol);

  auto initBlock = std::make_unique<BoundBlock>();
  initBlock->statements.push_back(std::make_unique<BoundVariableDeclaration>(
      iterableSymbol, std::move(iterableValue)));
  initBlock->statements.push_back(std::make_unique<BoundVariableDeclaration>(
      indexSymbol, std::make_unique<BoundLiteral>("0", intType)));

  auto makeIdExpr = [](const std::string &name) {
    return std::make_unique<ConstId>(name);
  };

  std::unique_ptr<ExpressionNode> conditionAst = nullptr;
  std::unique_ptr<ExpressionNode> elementAst = nullptr;
  auto iterableType = iterableSymbol->type;
  std::string accessName = iterableName;

  if (iterableType->getKind() == zir::TypeKind::Array) {
    auto arr = std::static_pointer_cast<zir::ArrayType>(iterableType);
    auto sliceName = makeSyntheticLoopName("slice");
    auto sliceType = makeVariadicViewType(arr->getBaseType());
    auto sliceSymbol = std::make_shared<VariableSymbol>(
        sliceName, sliceType, BindingKind::Mutable, false, sliceName,
        moduleName, Visibility::Private);
    currentScope_->declare(sliceName, sliceSymbol);
    initBlock->statements.push_back(std::make_unique<BoundVariableDeclaration>(
        sliceSymbol,
        std::make_unique<BoundCast>(
            std::make_unique<BoundVariableExpression>(iterableSymbol),
            sliceType)));
    accessName = sliceName;
    conditionAst = std::make_unique<BinExpr>(
        makeIdExpr(indexName), "<",
        std::make_unique<MemberAccessNode>(makeIdExpr(accessName), "len"));
    elementAst = std::make_unique<IndexAccessNode>(makeIdExpr(accessName),
                                                   makeIdExpr(indexName));
  } else if (isVariadicViewType(iterableType)) {
    conditionAst = std::make_unique<BinExpr>(
        makeIdExpr(indexName), "<",
        std::make_unique<MemberAccessNode>(makeIdExpr(accessName), "len"));
    elementAst = std::make_unique<IndexAccessNode>(makeIdExpr(accessName),
                                                   makeIdExpr(indexName));
  } else if (iterableType->getKind() == zir::TypeKind::Class) {
    auto lenCall = std::make_unique<FunCall>();
    lenCall->callee_ =
        std::make_unique<MemberAccessNode>(makeIdExpr(accessName), "len");

    conditionAst = std::make_unique<BinExpr>(makeIdExpr(indexName), "<",
                                             std::move(lenCall));

    auto atCall = std::make_unique<FunCall>();
    atCall->callee_ =
        std::make_unique<MemberAccessNode>(makeIdExpr(accessName), "at");
    atCall->params_.push_back(
        std::make_unique<Argument>("", makeIdExpr(indexName), false, false));
    elementAst = std::move(atCall);
  } else {
    error(node.iterable_->span,
          "Type '" + renderTypeForUser(iterableType) +
              "' is not iterable in for-in. Expected array, slice, or class "
              "with 'len()' and 'at(Int)'.");
    popScope();
    return;
  }

  conditionAst->accept(*this);
  if (expressionStack_.empty()) {
    popScope();
    return;
  }
  auto condition = std::move(expressionStack_.top());
  expressionStack_.pop();
  if (condition->type->getKind() != zir::TypeKind::Bool) {
    error(node.span, "For-in condition must be Bool, got '" +
                         renderTypeForUser(condition->type) + "'");
  }

  elementAst->accept(*this);
  if (expressionStack_.empty()) {
    popScope();
    return;
  }
  auto elementValue = std::move(expressionStack_.top());
  expressionStack_.pop();

  auto increment = std::make_unique<BoundAssignment>(
      std::make_unique<BoundVariableExpression>(indexSymbol),
      std::make_unique<BoundBinaryExpression>(
          std::make_unique<BoundVariableExpression>(indexSymbol), "+",
          std::make_unique<BoundLiteral>("1", intType), intType));

  pushScope();
  auto itemSymbol = std::make_shared<VariableSymbol>(
      node.itemName_, elementValue->type, BindingKind::Immutable, false,
      node.itemName_, moduleName, Visibility::Private);
  if (!currentScope_->declare(node.itemName_, itemSymbol)) {
    error(node.span, "Variable '" + node.itemName_ + "' already declared.");
  }
  if (semanticInfo_) {
    semanticInfo_->recordSymbol(&node, itemSymbol);
    semanticInfo_->recordDeclaration(&node, itemSymbol);
    semanticInfo_->recordType(&node, itemSymbol->type);
  }

  std::shared_ptr<VariableSymbol> indexUserSymbol = nullptr;
  if (!node.indexName_.empty()) {
    indexUserSymbol = std::make_shared<VariableSymbol>(
        node.indexName_, intType, BindingKind::Immutable, false,
        node.indexName_, moduleName, Visibility::Private);
    if (!currentScope_->declare(node.indexName_, indexUserSymbol)) {
      error(node.span, "Variable '" + node.indexName_ + "' already declared.");
    }
  }

  ++loopDepth_;
  auto body = bindBody(node.body_.get(), false);
  --loopDepth_;

  body->statements.insert(body->statements.begin(),
                          std::make_unique<BoundVariableDeclaration>(
                              itemSymbol, std::move(elementValue)));
  if (indexUserSymbol) {
    body->statements.insert(
        body->statements.begin(),
        std::make_unique<BoundVariableDeclaration>(
            indexUserSymbol,
            std::make_unique<BoundVariableExpression>(indexSymbol)));
  }
  popScope();

  popScope();
  statementStack_.push(std::make_unique<BoundForStatement>(
      std::move(initBlock), std::move(condition), std::move(increment),
      std::move(body)));
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

} // namespace sema
