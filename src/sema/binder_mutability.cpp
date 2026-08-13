#include "binder.hpp"

namespace sema {

namespace {

enum class MutablePlaceFailure {
  None,
  NotLValue,
  CompileTimeConstant,
  ImmutableBinding,
  ImmutableRecordField,
};

struct MutablePlaceResult {
  MutablePlaceFailure failure = MutablePlaceFailure::None;
  std::shared_ptr<VariableSymbol> binding;
};

bool isImmutableRecordStorage(const std::shared_ptr<zir::Type> &type) {
  if (!type) {
    return false;
  }

  auto aggregateType = type;
  if (aggregateType->getKind() == zir::TypeKind::Pointer) {
    aggregateType = std::static_pointer_cast<zir::PointerType>(aggregateType)
                        ->getBaseType();
  }

  return aggregateType && aggregateType->getKind() == zir::TypeKind::Record &&
         std::static_pointer_cast<zir::RecordType>(aggregateType)
             ->hasImmutableFields();
}

MutablePlaceResult classifyMutablePlace(const BoundExpression &place) {
  if (const auto *variable =
          dynamic_cast<const BoundVariableExpression *>(&place)) {
    if (variable->symbol->isCompileTimeConstant()) {
      return {MutablePlaceFailure::CompileTimeConstant, variable->symbol};
    }
    if (variable->symbol->isImmutableBinding()) {
      return {MutablePlaceFailure::ImmutableBinding, variable->symbol};
    }
    return {};
  }

  if (const auto *member = dynamic_cast<const BoundMemberAccess *>(&place)) {
    if (isImmutableRecordStorage(member->left->type)) {
      return {MutablePlaceFailure::ImmutableRecordField, nullptr};
    }
    if (member->left->type &&
        (member->left->type->getKind() == zir::TypeKind::Class ||
         member->left->type->getKind() == zir::TypeKind::Pointer)) {
      return {};
    }
    return classifyMutablePlace(*member->left);
  }

  if (const auto *index = dynamic_cast<const BoundIndexAccess *>(&place)) {
    return classifyMutablePlace(*index->left);
  }

  if (const auto *unary = dynamic_cast<const BoundUnaryExpression *>(&place)) {
    return unary->op == "*" ? MutablePlaceResult{}
                            : classifyMutablePlace(*unary->expr);
  }

  if (const auto *cast = dynamic_cast<const BoundCast *>(&place)) {
    return classifyMutablePlace(*cast->expression);
  }

  if (const auto *call = dynamic_cast<const BoundFunctionCall *>(&place)) {
    return call->symbol && call->symbol->returnsRef
               ? MutablePlaceResult{}
               : MutablePlaceResult{MutablePlaceFailure::NotLValue, nullptr};
  }

  return {MutablePlaceFailure::NotLValue, nullptr};
}

} // namespace

bool Binder::requireMutablePlace(const BoundExpression &expression,
                                 SourceSpan span, MutablePlaceUse use) {
  const MutablePlaceResult result = classifyMutablePlace(expression);
  if (result.failure == MutablePlaceFailure::None) {
    return true;
  }

  if (result.failure == MutablePlaceFailure::ImmutableBinding) {
    error(span,
          "Cannot mutate immutable binding '" + result.binding->name + "'.");
    return false;
  }

  if (result.failure == MutablePlaceFailure::CompileTimeConstant) {
    const std::string &name = result.binding->name;
    switch (use) {
    case MutablePlaceUse::Assignment:
      error(span, "Cannot assign to constant '" + name + "'.");
      break;
    case MutablePlaceUse::MutableReference:
      error(span,
            "Cannot bind a mutable reference to constant '" + name + "'.");
      break;
    case MutablePlaceUse::Address:
      error(span, "Cannot take the address of constant '" + name + "'.");
      break;
    case MutablePlaceUse::AsmOutput:
      error(span,
            "Cannot assign to constant '" + name + "' through inline 'asm'.");
      break;
    }
    return false;
  }

  if (result.failure == MutablePlaceFailure::ImmutableRecordField) {
    switch (use) {
    case MutablePlaceUse::Assignment:
      error(span, "Cannot assign to a field of immutable record.");
      break;
    case MutablePlaceUse::MutableReference:
      error(span,
            "Cannot bind a mutable reference to a field of immutable record.");
      break;
    case MutablePlaceUse::Address:
      error(span, "Cannot take the address of a field of immutable record.");
      break;
    case MutablePlaceUse::AsmOutput:
      error(span, "Cannot use a field of immutable record as an inline 'asm' "
                  "output.");
      break;
    }
    return false;
  }

  switch (use) {
  case MutablePlaceUse::Assignment:
    error(span, "Target of assignment must be an l-value.");
    break;
  case MutablePlaceUse::MutableReference:
    error(span, "Mutable reference target must be an l-value.");
    break;
  case MutablePlaceUse::Address:
    error(span, "Cannot take the address of a non-lvalue expression.");
    break;
  case MutablePlaceUse::AsmOutput:
    error(span, "Inline 'asm' output operand must be an l-value.");
    break;
  }
  return false;
}

} // namespace sema
