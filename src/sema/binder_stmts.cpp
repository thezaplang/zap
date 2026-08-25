#include "../ast/class_decl.hpp"
#include "../ast/const/const_char.hpp"
#include "../ast/defer_node.hpp"
#include "../ast/record_decl.hpp"
#include "../ir/string_type.hpp"
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

namespace {

struct IntegerPatternValue {
  bool negative = false;
  uint64_t magnitude = 0;
};

std::optional<IntegerPatternValue>
evaluateIntegerPattern(const BoundExpression *expression) {
  if (const auto *cast = dynamic_cast<const BoundCast *>(expression)) {
    return evaluateIntegerPattern(cast->expression.get());
  }
  if (const auto *literal = dynamic_cast<const BoundLiteral *>(expression)) {
    try {
      const auto &text = literal->value;
      int base = 10;
      std::string digits = text;
      if (text.size() > 2 && text[0] == '0') {
        if (text[1] == 'x' || text[1] == 'X') {
          base = 16;
        } else if (text[1] == 'b' || text[1] == 'B') {
          base = 2;
          digits = text.substr(2);
        } else if (text[1] == 'o' || text[1] == 'O') {
          base = 8;
          digits = text.substr(2);
        }
      }
      size_t consumed = 0;
      const auto magnitude = std::stoull(digits, &consumed, base);
      if (consumed != digits.size()) {
        return std::nullopt;
      }
      return IntegerPatternValue{false, magnitude};
    } catch (...) {
      return std::nullopt;
    }
  }
  if (const auto *unary =
          dynamic_cast<const BoundUnaryExpression *>(expression)) {
    auto value = evaluateIntegerPattern(unary->expr.get());
    if (!value || (unary->op != "+" && unary->op != "-")) {
      return std::nullopt;
    }
    if (unary->op == "-" && value->magnitude != 0) {
      value->negative = !value->negative;
    }
    return value;
  }
  return std::nullopt;
}

std::optional<std::string>
normalizeIntegerPattern(IntegerPatternValue value,
                        const std::shared_ptr<zir::Type> &type, int width) {
  if (width <= 0 || width > 64) {
    return std::nullopt;
  }
  if (type->isUnsigned()) {
    if (value.negative) {
      return std::nullopt;
    }
    const uint64_t maximum = width == 64 ? std::numeric_limits<uint64_t>::max()
                                         : (uint64_t{1} << width) - 1;
    return value.magnitude <= maximum
               ? std::optional<std::string>(std::to_string(value.magnitude))
               : std::nullopt;
  }
  const uint64_t negativeMaximum = uint64_t{1} << (width - 1);
  const uint64_t positiveMaximum = negativeMaximum - 1;
  if (value.negative) {
    return value.magnitude <= negativeMaximum
               ? std::optional<std::string>("-" +
                                            std::to_string(value.magnitude))
               : std::nullopt;
  }
  return value.magnitude <= positiveMaximum
             ? std::optional<std::string>(std::to_string(value.magnitude))
             : std::nullopt;
}

bool isIrrefutableRecordPattern(const CasePattern &record) {
  return std::all_of(record.recordFields.begin(), record.recordFields.end(),
                     [](const CaseRecordFieldPattern &field) {
                       return !field.nested ||
                              (field.nested->kind == CasePatternKind::Record &&
                               isIrrefutableRecordPattern(*field.nested));
                     });
}

std::string canonicalLiteralPattern(const BoundExpression &value,
                                    int nativeIntegerBitWidth) {
  if (value.type->isInteger()) {
    auto integerValue = evaluateIntegerPattern(&value);
    auto numericInfo = zir::numericTypeInfo(value.type->getKind());
    const int width =
        numericInfo ? numericInfo->bitWidth(nativeIntegerBitWidth) : 0;
    auto normalized =
        integerValue ? normalizeIntegerPattern(*integerValue, value.type, width)
                     : std::nullopt;
    return normalized ? value.type->toString() + ":" + *normalized
                      : std::string{};
  }
  const BoundExpression *expression = &value;
  while (const auto *cast = dynamic_cast<const BoundCast *>(expression)) {
    expression = cast->expression.get();
  }
  if (const auto *literal = dynamic_cast<const BoundLiteral *>(expression)) {
    return value.type->toString() + ":" + literal->value;
  }
  return "";
}

std::string canonicalCasePattern(const BoundCasePattern &pattern,
                                 int nativeIntegerBitWidth) {
  if (pattern.kind == BoundCasePatternKind::Record) {
    std::vector<std::string> constraints;
    for (const auto &field : pattern.recordFields) {
      if (!field.nested) {
        continue;
      }
      auto nestedKey =
          canonicalCasePattern(*field.nested, nativeIntegerBitWidth);
      if (!nestedKey.empty()) {
        constraints.push_back(std::to_string(field.index) + ":" +
                              std::move(nestedKey));
      }
    }
    if (constraints.empty()) {
      return "";
    }
    std::sort(constraints.begin(), constraints.end());
    std::string key = "record{";
    for (const auto &constraint : constraints) {
      key += constraint + ";";
    }
    return key + "}";
  }
  if (pattern.kind == BoundCasePatternKind::Literal) {
    return "literal:" +
           canonicalLiteralPattern(*pattern.value, nativeIntegerBitWidth);
  }
  if (pattern.kind == BoundCasePatternKind::EnumVariant) {
    return "enum:" + std::to_string(pattern.variantTag);
  }
  if (pattern.kind == BoundCasePatternKind::TaggedUnionVariant) {
    std::string key = "union:" + std::to_string(pattern.variantTag);
    if (pattern.payloadValue) {
      key += ":literal:" + canonicalLiteralPattern(*pattern.payloadValue,
                                                   nativeIntegerBitWidth);
    } else if (pattern.payloadPattern) {
      key += ":" + canonicalCasePattern(*pattern.payloadPattern,
                                        nativeIntegerBitWidth);
    }
    return key;
  }
  return "";
}

} // namespace

void Binder::emitDefersUpTo(std::vector<std::unique_ptr<BoundStatement>> &target, bool stopAtLoop) {
  for (auto it = deferScopes_.rbegin(); it != deferScopes_.rend(); ++it) {
    for (auto defIt = it->defers.rbegin(); defIt != it->defers.rend(); ++defIt) {
      const auto *defer = *defIt;
      if (!defer || !defer->statement_) {
        continue;
      }
      if (auto *body = dynamic_cast<BodyNode *>(defer->statement_.get())) {
        target.push_back(bindBody(body, true));
      } else {
        defer->statement_->accept(*this);
        if (!statementStack_.empty()) {
          target.push_back(std::move(statementStack_.top()));
          statementStack_.pop();
        } else if (!expressionStack_.empty()) {
          auto boundExpr = std::move(expressionStack_.top());
          expressionStack_.pop();
          target.push_back(std::make_unique<BoundExpressionStatement>(std::move(boundExpr)));
        }
      }
    }
    if (stopAtLoop && it->isLoopBoundary) {
      break;
    }
  }
}

std::unique_ptr<BoundBlock> Binder::bindBody(BodyNode *body, bool createScope) {
  auto savedBlock = std::move(currentBlock_);
  if (createScope) {
    pushScope();
  }

  deferScopes_.push_back(DeferScope{});

  if (body) {
    body->accept(*this);
  }

  auto boundBody = std::make_unique<BoundBlock>();
  if (currentBlock_) {
    boundBody = std::move(currentBlock_);
  }

  auto currentScopeDefers = std::move(deferScopes_.back());
  deferScopes_.pop_back();

  if (!blockAlwaysReturns(boundBody.get())) {
    for (auto defIt = currentScopeDefers.defers.rbegin(); defIt != currentScopeDefers.defers.rend(); ++defIt) {
      const auto *deferNode = *defIt;
      if (!deferNode || !deferNode->statement_) {
        continue;
      }
      if (auto *bodyNode = dynamic_cast<BodyNode *>(deferNode->statement_.get())) {
        boundBody->statements.push_back(bindBody(bodyNode, true));
      } else {
        deferNode->statement_->accept(*this);
        if (!statementStack_.empty()) {
          boundBody->statements.push_back(std::move(statementStack_.top()));
          statementStack_.pop();
        } else if (!expressionStack_.empty()) {
          auto bound = std::move(expressionStack_.top());
          expressionStack_.pop();
          boundBody->statements.push_back(std::make_unique<BoundExpressionStatement>(std::move(bound)));
        }
      }
    }
  }

  if (createScope) {
    popScope();
  }

  currentBlock_ = std::move(savedBlock);
  return boundBody;
}

void Binder::visit(DeferNode &node) {
  if (deferScopes_.empty()) {
    error(node.span, "'defer' can only be used inside a function or block.");
    return;
  }
  deferScopes_.back().defers.push_back(&node);
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

  std::vector<std::unique_ptr<BoundStatement>> deferStmts;
  emitDefersUpTo(deferStmts, false);

  if (!deferStmts.empty()) {
    auto returnBlock = std::make_unique<BoundBlock>();
    std::shared_ptr<VariableSymbol> tempReturnSymbol = nullptr;

    if (expr && expr->type && expr->type->getKind() != zir::TypeKind::Void) {
      auto tempName = makeSyntheticLoopName("ret_val");
      tempReturnSymbol = std::make_shared<VariableSymbol>(
          tempName, expr->type, BindingKind::Immutable, false, tempName,
          modules_[currentModuleId_].info->moduleName, Visibility::Private);
      currentScope_->declare(tempName, tempReturnSymbol);
      returnBlock->statements.push_back(std::make_unique<BoundVariableDeclaration>(tempReturnSymbol, std::move(expr)));
    }

    for (auto &defStmt : deferStmts) {
      returnBlock->statements.push_back(std::move(defStmt));
    }

    std::unique_ptr<BoundExpression> finalReturnExpr = nullptr;
    if (tempReturnSymbol) {
      finalReturnExpr = std::make_unique<BoundVariableExpression>(tempReturnSymbol);
    } else if (expr) {
      finalReturnExpr = std::move(expr);
    }

    returnBlock->statements.push_back(std::make_unique<BoundReturnStatement>(std::move(finalReturnExpr), currentFunction_ && currentFunction_->returnsRef));
    statementStack_.push(std::move(returnBlock));
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

  std::vector<std::unique_ptr<BoundStatement>> deferStmts;
  emitDefersUpTo(deferStmts, false);

  if (!deferStmts.empty()) {
    auto fail = std::make_unique<BoundBlock>();
    auto name = makeSyntheticLoopName("fail_err");
    auto err = std::make_shared<VariableSymbol>(
        name, errExpr->type, BindingKind::Immutable, false, name,
        modules_[currentModuleId_].info->moduleName, Visibility::Private);
    currentScope_->declare(name, err);
    fail->statements.push_back(std::make_unique<BoundVariableDeclaration>(err, std::move(errExpr)));

    for (auto &defStmt : deferStmts) {
      fail->statements.push_back(std::move(defStmt));
    }

    fail->statements.push_back(std::make_unique<BoundFailStatement>(std::make_unique<BoundVariableExpression>(err), propagatedType, expectedErrorType));
    statementStack_.push(std::move(fail));
    return;
  }

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

void Binder::visit(CaseNode &node) { bindCaseStatement(node); }

std::unique_ptr<BoundBlock> Binder::bindCaseArmBody(
    const CaseArm &arm, const std::shared_ptr<VariableSymbol> &payloadBinding,
    const std::vector<std::shared_ptr<VariableSymbol>> &recordBindings) {
  if (!payloadBinding && recordBindings.empty()) {
    return bindBody(arm.body.get(), true);
  }

  pushScope();
  if (payloadBinding &&
      !currentScope_->declare(payloadBinding->name, payloadBinding)) {
    error(arm.span, "Duplicate payload binding '" + payloadBinding->name +
                        "' in case arm.");
  }
  for (const auto &binding : recordBindings) {
    if (!currentScope_->declare(binding->name, binding)) {
      error(arm.span,
            "Duplicate record binding '" + binding->name + "' in case arm.");
    }
  }
  auto body = bindBody(arm.body.get(), false);
  popScope();
  return body;
}

bool Binder::hasExhaustiveCaseCoverage(
    const std::shared_ptr<zir::Type> &scrutineeType,
    const std::unordered_set<int64_t> &coveredVariants,
    bool hasIrrefutableRecordPattern) const {
  if (hasIrrefutableRecordPattern) {
    return true;
  }
  if (scrutineeType->getKind() == zir::TypeKind::Enum) {
    const auto &variants =
        std::static_pointer_cast<zir::EnumType>(scrutineeType)->getVariants();
    return std::all_of(
        variants.begin(), variants.end(), [&](const auto &variant) {
          return coveredVariants.count(variant.discriminant) != 0;
        });
  }
  if (scrutineeType->getKind() == zir::TypeKind::TaggedUnion) {
    const auto &variants =
        std::static_pointer_cast<zir::TaggedUnionType>(scrutineeType)
            ->getVariants();
    return std::all_of(variants.begin(), variants.end(),
                       [&](const auto &variant) {
                         return coveredVariants.count(variant.tag) != 0;
                       });
  }
  return false;
}

Binder::RecordPatternResult
Binder::bindCaseRecordPattern(const CasePattern &record,
                              std::shared_ptr<zir::RecordType> type) {
  RecordPatternResult result;
  auto &recordBindings = result.bindings;
  auto &recordPatternValid = result.valid;
  std::vector<BoundCaseRecordField> fields;
  std::unordered_set<std::string> names;
  for (const auto &field : record.recordFields) {
    if (!names.insert(field.name).second) {
      error(field.span, "Duplicate record pattern field '" + field.name + "'.");
      recordPatternValid = false;
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
      error(field.span, "Record type '" + type->getName() + "' has no field '" +
                            field.name + "'.");
      recordPatternValid = false;
      continue;
    }
    if (field.nested) {
      if (field.nested->kind == CasePatternKind::Record) {
        if (fieldType->getKind() != zir::TypeKind::Record) {
          error(field.span, "Nested record pattern requires a "
                            "Record/struct field.");
          recordPatternValid = false;
          continue;
        }
        auto nestedSymbol = resolveQualifiedSymbol(
            field.nested->recordPath, field.nested->span, SymbolKind::Type);
        if (!nestedSymbol || !zir::sameType(nestedSymbol->type, fieldType)) {
          error(field.nested->span,
                "Nested record pattern does not match field '" + field.name +
                    "'.");
          recordPatternValid = false;
          continue;
        }
        auto nested = bindCaseRecordPattern(
            *field.nested,
            std::static_pointer_cast<zir::RecordType>(fieldType));
        recordPatternValid = recordPatternValid && nested.valid;
        recordBindings.insert(recordBindings.end(), nested.bindings.begin(),
                              nested.bindings.end());
        fields.push_back({index, std::move(nested.pattern), nullptr});
        continue;
      }

      if (field.nested->kind == CasePatternKind::Variant) {
        const auto &path = field.nested->variantPath;
        if ((fieldType->getKind() != zir::TypeKind::Enum &&
             fieldType->getKind() != zir::TypeKind::TaggedUnion) ||
            path.size() < 2) {
          error(field.nested->span,
                "Enum field pattern does not match field '" + field.name +
                    "'.");
          recordPatternValid = false;
          continue;
        }
        std::vector<std::string> typePath(path.begin(), path.end() - 1);
        auto enumSymbol = resolveQualifiedSymbol(typePath, field.nested->span,
                                                 SymbolKind::Type);
        if (!enumSymbol || !zir::sameType(enumSymbol->type, fieldType)) {
          error(field.nested->span,
                "Enum field pattern does not match field '" + field.name +
                    "'.");
          recordPatternValid = false;
          continue;
        }
        if (fieldType->getKind() == zir::TypeKind::Enum) {
          auto enumType = std::static_pointer_cast<zir::EnumType>(fieldType);
          const auto tag = enumType->getVariantDiscriminant(path.back());
          if (tag < 0 ||
              field.nested->payloadKind != CasePayloadPatternKind::None) {
            error(field.nested->span,
                  "Invalid enum field pattern for '" + field.name + "'.");
            recordPatternValid = false;
            continue;
          }
          fields.push_back({index,
                            std::make_unique<BoundCasePattern>(
                                BoundCasePatternKind::EnumVariant, tag),
                            nullptr});
          continue;
        }

        auto taggedUnion =
            std::static_pointer_cast<zir::TaggedUnionType>(fieldType);
        const auto *variant = taggedUnion->findVariant(path.back());
        if (!variant) {
          error(field.nested->span,
                "Invalid enum field pattern for '" + field.name + "'.");
          recordPatternValid = false;
          continue;
        }
        if (!variant->payloadType) {
          if (field.nested->payloadKind != CasePayloadPatternKind::None &&
              field.nested->payloadKind != CasePayloadPatternKind::Empty) {
            error(field.nested->span,
                  "Invalid enum field pattern for '" + field.name + "'.");
            recordPatternValid = false;
            continue;
          }
          fields.push_back(
              {index,
               std::make_unique<BoundCasePattern>(
                   BoundCasePatternKind::TaggedUnionVariant, variant->tag),
               nullptr});
          continue;
        }
        if (field.nested->payloadKind != CasePayloadPatternKind::Binding &&
            field.nested->payloadKind != CasePayloadPatternKind::Wildcard &&
            field.nested->payloadKind != CasePayloadPatternKind::Pattern &&
            field.nested->payloadKind != CasePayloadPatternKind::Literal) {
          error(field.nested->span,
                "Enum field payload pattern for '" + field.name +
                    "' must be a binding, '_', literal, or record "
                    "pattern.");
          recordPatternValid = false;
          continue;
        }
        std::unique_ptr<BoundExpression> payloadValue;
        if (field.nested->payloadKind == CasePayloadPatternKind::Literal) {
          auto expectedPayloadType = variant->payloadType;
          if (expectedPayloadType->getIntrinsicKind() ==
              zir::IntrinsicTypeKind::String) {
            expectedPayloadType = zir::makeStringViewType();
          }
          payloadValue = bindExpressionWithExpected(
              field.nested->payloadLiteral.get(), expectedPayloadType);
          if (!payloadValue) {
            recordPatternValid = false;
            continue;
          }
          if (variant->payloadType->isInteger()) {
            auto integerValue = evaluateIntegerPattern(payloadValue.get());
            if (!integerValue ||
                !normalizeIntegerPattern(*integerValue, variant->payloadType,
                                         typeBitWidth(variant->payloadType))) {
              error(field.nested->span,
                    "Enum field payload pattern is not representable "
                    "by payload type '" +
                        renderTypeForUser(variant->payloadType) + "'.");
              recordPatternValid = false;
              continue;
            }
            if (!zir::sameType(payloadValue->type, variant->payloadType)) {
              payloadValue = std::make_unique<BoundCast>(
                  std::move(payloadValue), variant->payloadType);
            }
          } else if (auto *literal =
                         dynamic_cast<BoundLiteral *>(payloadValue.get())) {
            auto conversion = conversions_.classifyImplicit(
                payloadValue->type, expectedPayloadType);
            if (!conversion) {
              error(field.nested->span,
                    "Enum field payload pattern type does not match "
                    "payload type '" +
                        renderTypeForUser(variant->payloadType) + "'.");
              recordPatternValid = false;
              continue;
            }
            if (isStringType(expectedPayloadType)) {
              payloadValue->type = expectedPayloadType;
            } else {
              payloadValue =
                  applyConversion(std::move(payloadValue), *conversion);
            }
          } else {
            error(field.nested->span,
                  "Enum field payload pattern must be a literal.");
            recordPatternValid = false;
            continue;
          }
        }
        std::unique_ptr<BoundCasePattern> payloadPattern;
        if (field.nested->payloadKind == CasePayloadPatternKind::Pattern) {
          const auto &payload = *field.nested->payloadPattern;
          if (payload.kind != CasePatternKind::Record ||
              variant->payloadType->getKind() != zir::TypeKind::Record) {
            error(field.nested->span,
                  "Enum field payload pattern for '" + field.name +
                      "' must match a Record/struct payload.");
            recordPatternValid = false;
            continue;
          }
          auto payloadSymbol = resolveQualifiedSymbol(
              payload.recordPath, payload.span, SymbolKind::Type);
          if (!payloadSymbol ||
              !zir::sameType(payloadSymbol->type, variant->payloadType)) {
            error(payload.span,
                  "Enum field record payload pattern does not match "
                  "field '" +
                      field.name + "'.");
            recordPatternValid = false;
            continue;
          }
          auto nested = bindCaseRecordPattern(
              payload,
              std::static_pointer_cast<zir::RecordType>(variant->payloadType));
          recordPatternValid = recordPatternValid && nested.valid;
          recordBindings.insert(recordBindings.end(), nested.bindings.begin(),
                                nested.bindings.end());
          payloadPattern = std::move(nested.pattern);
        }
        std::shared_ptr<VariableSymbol> payloadBinding;
        if (field.nested->payloadKind == CasePayloadPatternKind::Binding) {
          payloadBinding = std::make_shared<VariableSymbol>(
              field.nested->payloadBinding, variant->payloadType,
              BindingKind::Immutable, false, field.nested->payloadBinding,
              currentModuleId_);
          recordBindings.push_back(payloadBinding);
        }
        fields.push_back(
            {index,
             std::make_unique<BoundCasePattern>(
                 BoundCasePatternKind::TaggedUnionVariant, variant->tag,
                 variant->payloadType, std::move(payloadValue),
                 std::move(payloadPattern), std::move(payloadBinding)),
             nullptr});
        continue;
      }

      if (field.nested->kind != CasePatternKind::Literal) {
        error(field.nested->span,
              "Record fields currently support literal, enum, or "
              "record patterns.");
        recordPatternValid = false;
        continue;
      }
      auto value =
          bindExpressionWithExpected(field.nested->literal.get(), fieldType);
      if (!value) {
        recordPatternValid = false;
        continue;
      }
      if (fieldType->isInteger()) {
        auto integerValue = evaluateIntegerPattern(value.get());
        if (!integerValue ||
            !normalizeIntegerPattern(*integerValue, fieldType,
                                     typeBitWidth(fieldType))) {
          error(field.nested->span, "Record field pattern is not "
                                    "representable by field type '" +
                                        renderTypeForUser(fieldType) + "'.");
          recordPatternValid = false;
          continue;
        }
        if (!zir::sameType(value->type, fieldType)) {
          value = std::make_unique<BoundCast>(std::move(value), fieldType);
        }
      } else if (auto *literal = dynamic_cast<BoundLiteral *>(value.get())) {
        auto conversion = conversions_.classifyImplicit(value->type, fieldType);
        if (!conversion) {
          error(field.nested->span, "Record field pattern type '" +
                                        renderTypeForUser(value->type) +
                                        "' does not match field type '" +
                                        renderTypeForUser(fieldType) + "'.");
          recordPatternValid = false;
          continue;
        }
        value = applyConversion(std::move(value), *conversion);
      } else {
        error(field.nested->span, "Record field pattern must be a literal.");
        recordPatternValid = false;
        continue;
      }
      fields.push_back({index,
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
  result.pattern = std::make_unique<BoundCasePattern>(type, std::move(fields));
  return result;
}

void Binder::bindCaseStatement(CaseNode &node) {
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
          auto recordResult = bindCaseRecordPattern(
              pattern,
              std::static_pointer_cast<zir::RecordType>(scrutineeType));
          if (!recordResult.valid) {
            continue;
          }
          recordBindings.insert(recordBindings.end(),
                                recordResult.bindings.begin(),
                                recordResult.bindings.end());
          auto recordKey = canonicalCasePattern(
              *recordResult.pattern, targetInfo_.nativeIntegerBitWidth());
          recordKey = recordKey.empty() ? "record:*" : "record:" + recordKey;
          if (!seenPatterns.insert(recordKey).second) {
            error(pattern.span, "Duplicate case pattern.");
          }
          boundPatterns.push_back(std::move(*recordResult.pattern));
          hasRecordPattern =
              hasRecordPattern || isIrrefutableRecordPattern(pattern);
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
          if (coveredVariants.count(variant->tag) != 0) {
            error(pattern.span, "Case pattern is unreachable because an "
                                "earlier pattern covers this variant.");
            continue;
          }
          if (!variant->payloadType &&
              pattern.payloadKind != CasePayloadPatternKind::None &&
              pattern.payloadKind != CasePayloadPatternKind::Empty) {
            error(pattern.span,
                  "Enum variant '" + variantName + "' does not take a payload pattern.");
            continue;
          }
          if (variant->payloadType &&
              pattern.payloadKind != CasePayloadPatternKind::Binding &&
              pattern.payloadKind != CasePayloadPatternKind::Wildcard &&
              pattern.payloadKind != CasePayloadPatternKind::Literal &&
              pattern.payloadKind != CasePayloadPatternKind::Pattern) {
            error(pattern.span, "Enum variant '" + variantName +
                                    "' expects one payload pattern.");
            continue;
          }
          if (pattern.payloadKind == CasePayloadPatternKind::Binding ||
              pattern.payloadKind == CasePayloadPatternKind::Pattern) {
            if (arm.patterns.size() != 1) {
              error(pattern.span, "A case arm with payload bindings cannot "
                                  "have alternatives.");
              continue;
            }
          }
          if (pattern.payloadKind == CasePayloadPatternKind::Binding) {
            payloadBinding = std::make_shared<VariableSymbol>(
                pattern.payloadBinding, variant->payloadType,
                BindingKind::Immutable, false, pattern.payloadBinding,
                currentModuleId_);
          }
          std::unique_ptr<BoundExpression> payloadValue;
          std::unique_ptr<BoundCasePattern> payloadPattern;
          bool payloadPatternIrrefutable = false;
          std::string payloadKey;
          if (pattern.payloadKind == CasePayloadPatternKind::Literal) {
            auto expectedPayloadType = variant->payloadType;
            if (expectedPayloadType->getIntrinsicKind() ==
                zir::IntrinsicTypeKind::String) {
              expectedPayloadType = zir::makeStringViewType();
            }
            payloadValue = bindExpressionWithExpected(
                pattern.payloadLiteral.get(), expectedPayloadType);
            if (!payloadValue) {
              continue;
            }
            if (variant->payloadType->isInteger()) {
              auto integerValue = evaluateIntegerPattern(payloadValue.get());
              auto normalized = integerValue
                                    ? normalizeIntegerPattern(
                                          *integerValue, variant->payloadType,
                                          typeBitWidth(variant->payloadType))
                                    : std::nullopt;
              if (!normalized) {
                error(pattern.span,
                      "Enum payload pattern is not representable by payload "
                      "type '" +
                          renderTypeForUser(variant->payloadType) + "'.");
                continue;
              }
              payloadKey = *normalized;
              if (!zir::sameType(payloadValue->type, variant->payloadType)) {
                payloadValue = std::make_unique<BoundCast>(
                    std::move(payloadValue), variant->payloadType);
              }
            } else if (auto *literal =
                           dynamic_cast<BoundLiteral *>(payloadValue.get())) {
              auto conversion = conversions_.classifyImplicit(
                  payloadValue->type, expectedPayloadType);
              if (!conversion) {
                error(
                    pattern.span,
                    "Enum payload pattern type does not match payload type '" +
                        renderTypeForUser(variant->payloadType) + "'.");
                continue;
              }
              payloadKey = literal->value;
              if (isStringType(expectedPayloadType)) {
                payloadValue->type = expectedPayloadType;
              } else {
                payloadValue =
                    applyConversion(std::move(payloadValue), *conversion);
              }
            } else {
              error(pattern.span, "Enum payload pattern must be a literal.");
              continue;
            }
          }
          if (pattern.payloadKind == CasePayloadPatternKind::Pattern) {
            const auto &payload = *pattern.payloadPattern;
            if (payload.kind != CasePatternKind::Record ||
                variant->payloadType->getKind() != zir::TypeKind::Record) {
              error(pattern.span,
                    "Enum payload pattern must match a Record/struct payload.");
              continue;
            }
            auto typeSymbol = resolveQualifiedSymbol(
                payload.recordPath, payload.span, SymbolKind::Type);
            if (!typeSymbol ||
                !zir::sameType(typeSymbol->type, variant->payloadType)) {
              error(payload.span,
                    "Enum payload record pattern does not match payload type.");
              continue;
            }
            auto recordType =
                std::static_pointer_cast<zir::RecordType>(variant->payloadType);
            auto recordResult =
                bindCaseRecordPattern(payload, std::move(recordType));
            if (!recordResult.valid) {
              continue;
            }
            recordBindings.insert(recordBindings.end(),
                                  recordResult.bindings.begin(),
                                  recordResult.bindings.end());
            payloadPattern = std::move(recordResult.pattern);
            payloadPatternIrrefutable = isIrrefutableRecordPattern(payload);
            auto canonicalKey = canonicalCasePattern(
                *payloadPattern, targetInfo_.nativeIntegerBitWidth());
            payloadKey = canonicalKey.empty()
                             ? ":record:*"
                             : ":record:" + std::move(canonicalKey);
          }
          const auto key =
              "union:" + std::to_string(variant->tag) +
              (payloadValue ? ":literal:" + payloadKey : payloadKey);
          if (!seenPatterns.insert(key).second) {
            error(pattern.span, "Duplicate case pattern.");
          }
          if (!payloadValue &&
              (pattern.payloadKind != CasePayloadPatternKind::Pattern ||
               payloadPatternIrrefutable)) {
            coveredVariants.insert(variant->tag);
          }
          boundPatterns.emplace_back(BoundCasePatternKind::TaggedUnionVariant,
                                     variant->tag, variant->payloadType,
                                     std::move(payloadValue),
                                     std::move(payloadPattern));
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
          auto integerValue = evaluateIntegerPattern(value.get());
          if (!integerValue) {
            error(pattern.span, "Case integer pattern must be constant.");
            continue;
          }
          auto normalized = normalizeIntegerPattern(
              *integerValue, scrutineeType, typeBitWidth(scrutineeType));
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
          if (isStringType(scrutineeType)) {
            value->type = scrutineeType;
          } else {
            value = applyConversion(std::move(value), *conversion);
          }
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

    auto body = bindCaseArmBody(arm, payloadBinding, recordBindings);
    boundArms.emplace_back(arm.isElse, std::move(boundPatterns),
                           std::move(payloadBinding), std::move(recordBindings),
                           std::move(body));
  }
  const bool exhaustive = hasExhaustiveCaseCoverage(
      scrutineeType, coveredVariants, hasRecordPattern);

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
  deferScopes_.push_back(DeferScope{true, {}});
  auto body = bindBody(node.body_.get(), true);
  deferScopes_.pop_back();
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
  deferScopes_.push_back(DeferScope{true, {}});
  auto body = bindBody(node.body_.get(), true);
  deferScopes_.pop_back();
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

  if (auto rangeExpr = dynamic_cast<BoundRangeExpression *>(iterableValue.get())) {
    auto rangeType = rangeExpr->type;
    auto start = std::move(rangeExpr->start);
    auto end = std::move(rangeExpr->end);
    auto step = std::move(rangeExpr->step);

    std::optional<int64_t> constantStep;
    if (step) {
      constantStep = evaluateConstantInt(step.get());
    } else {
      constantStep = 1;
      step = std::make_unique<BoundLiteral>("1", rangeType);
    }

    auto initBlock = std::make_unique<BoundBlock>();

    auto counterName = makeSyntheticLoopName("val");
    auto valCounterSymbol = std::make_shared<VariableSymbol>(
        counterName, rangeType, BindingKind::Mutable, false, counterName,
        moduleName, Visibility::Private);
    currentScope_->declare(counterName, valCounterSymbol);
    initBlock->statements.push_back(std::make_unique<BoundVariableDeclaration>(
        valCounterSymbol, std::move(start)));

    auto endName = makeSyntheticLoopName("end");
    auto endCounterSymbol = std::make_shared<VariableSymbol>(endName, rangeType, BindingKind::Immutable, false, endName,moduleName, Visibility::Private);
    currentScope_->declare(endName, endCounterSymbol);
    initBlock->statements.push_back(std::make_unique<BoundVariableDeclaration>(endCounterSymbol, std::move(end)));

    auto stepCounterName = makeSyntheticLoopName("step");
    auto stepCounterSymbol = std::make_shared<VariableSymbol>(
        stepCounterName, rangeType, BindingKind::Immutable, false, stepCounterName,
        moduleName, Visibility::Private);
    currentScope_->declare(stepCounterName, stepCounterSymbol);
    initBlock->statements.push_back(std::make_unique<BoundVariableDeclaration>(stepCounterSymbol, std::move(step)));

    std::shared_ptr<VariableSymbol> idxCounterSymbol = nullptr;
    if (!node.indexName_.empty()) {
      auto idxCounterName = makeSyntheticLoopName("idx");
      idxCounterSymbol = std::make_shared<VariableSymbol>(
          idxCounterName, intType, BindingKind::Mutable, false, idxCounterName,
          moduleName, Visibility::Private);
      currentScope_->declare(idxCounterName, idxCounterSymbol);
      initBlock->statements.push_back(std::make_unique<BoundVariableDeclaration>(idxCounterSymbol, std::make_unique<BoundLiteral>("0", intType)));
    }

    auto boolType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Bool);
    std::unique_ptr<BoundExpression> condition;

    if (constantStep.has_value()) {
      std::string cmpOp = (*constantStep < 0) ? ">" : "<";
      condition = std::make_unique<BoundBinaryExpression>(std::make_unique<BoundVariableExpression>(valCounterSymbol), cmpOp,std::make_unique<BoundVariableExpression>(endCounterSymbol), boolType);
    } else {
      auto zeroLiteral = std::make_unique<BoundLiteral>("0", rangeType);
      auto stepIsPositive = std::make_unique<BoundBinaryExpression>(std::make_unique<BoundVariableExpression>(stepCounterSymbol), ">",std::move(zeroLiteral), boolType);
      auto posCondition = std::make_unique<BoundBinaryExpression>(std::make_unique<BoundVariableExpression>(valCounterSymbol), "<", std::make_unique<BoundVariableExpression>(endCounterSymbol), boolType);
      auto negCondition = std::make_unique<BoundBinaryExpression>(std::make_unique<BoundVariableExpression>(valCounterSymbol), ">",std::make_unique<BoundVariableExpression>(endCounterSymbol), boolType);

      condition = std::make_unique<BoundTernaryExpression>(std::move(stepIsPositive), std::move(posCondition),std::move(negCondition), boolType);
    }

    auto increment = std::make_unique<BoundAssignment>(
        std::make_unique<BoundVariableExpression>(valCounterSymbol),
        std::make_unique<BoundBinaryExpression>(
            std::make_unique<BoundVariableExpression>(valCounterSymbol), "+",
            std::make_unique<BoundVariableExpression>(stepCounterSymbol), rangeType));

    pushScope();
    auto itemSymbol = std::make_shared<VariableSymbol>(node.itemName_, rangeType, BindingKind::Immutable, false,node.itemName_, moduleName, Visibility::Private);
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
    deferScopes_.push_back(DeferScope{true, {}});
    auto body = bindBody(node.body_.get(), false);
    deferScopes_.pop_back();
    --loopDepth_;

    body->statements.insert(
        body->statements.begin(),
        std::make_unique<BoundVariableDeclaration>(
            itemSymbol,
            std::make_unique<BoundVariableExpression>(valCounterSymbol)));

    if (indexUserSymbol) {
      body->statements.insert(
          body->statements.begin(),
          std::make_unique<BoundVariableDeclaration>(
              indexUserSymbol,
              std::make_unique<BoundVariableExpression>(idxCounterSymbol)));

      body->statements.push_back(std::make_unique<BoundAssignment>(
          std::make_unique<BoundVariableExpression>(idxCounterSymbol),
          std::make_unique<BoundBinaryExpression>(
              std::make_unique<BoundVariableExpression>(idxCounterSymbol), "+",
              std::make_unique<BoundLiteral>("1", intType), intType)));
    }
    popScope();

    popScope();
    statementStack_.push(std::make_unique<BoundForStatement>(
        std::move(initBlock), std::move(condition), std::move(increment),
        std::move(body)));
    return;
  }

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
  deferScopes_.push_back(DeferScope{true, {}});
  auto body = bindBody(node.body_.get(), false);
  deferScopes_.pop_back();
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
  std::vector<std::unique_ptr<BoundStatement>> deferStmts;
  emitDefersUpTo(deferStmts, true);

  if (!deferStmts.empty()) {
    auto breakBlock = std::make_unique<BoundBlock>();
    for (auto &defStmt : deferStmts) {
      breakBlock->statements.push_back(std::move(defStmt));
    }
    breakBlock->statements.push_back(std::make_unique<BoundBreakStatement>());
    statementStack_.push(std::move(breakBlock));
    return;
  }
  statementStack_.push(std::make_unique<BoundBreakStatement>());
}

void Binder::visit(ContinueNode &node) {
  if (loopDepth_ <= 0) {
    error(node.span, "'continue' can only be used inside loops.");
    return;
  }
  std::vector<std::unique_ptr<BoundStatement>> deferStmts;
  emitDefersUpTo(deferStmts, true);

  if (!deferStmts.empty()) {
    auto continueBlock = std::make_unique<BoundBlock>();
    for (auto &defStmt : deferStmts) {
      continueBlock->statements.push_back(std::move(defStmt));
    }
    continueBlock->statements.push_back(std::make_unique<BoundContinueStatement>());
    statementStack_.push(std::move(continueBlock));
    return;
  }
  statementStack_.push(std::make_unique<BoundContinueStatement>());
}

} // namespace sema
