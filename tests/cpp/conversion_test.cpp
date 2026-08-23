#include "ir/failable_type.hpp"
#include "ir/string_type.hpp"
#include "sema/conversion.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using sema::ConversionClassifier;
using sema::ConversionKind;
using sema::ConversionRank;
using zir::ClassType;
using zir::PointerType;
using zir::PrimitiveType;
using zir::Type;
using zir::TypeInterner;
using zir::TypeKind;

std::shared_ptr<Type> primitive(TypeKind kind) {
  return std::make_shared<PrimitiveType>(kind);
}

bool expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

bool testNumericConversions() {
  TypeInterner types;
  ConversionClassifier conversions(types);

  auto identity = conversions.classifyImplicit(primitive(TypeKind::Int),
                                               primitive(TypeKind::Int));
  auto widening = conversions.classifyImplicit(primitive(TypeKind::Int16),
                                               primitive(TypeKind::Int64));
  auto narrowing = conversions.classifyImplicit(primitive(TypeKind::Int64),
                                                primitive(TypeKind::Int16));
  auto signedness = conversions.classifyImplicit(primitive(TypeKind::UInt),
                                                 primitive(TypeKind::Int));
  auto intToFloat = conversions.classifyImplicit(primitive(TypeKind::Int),
                                                 primitive(TypeKind::Float64));
  auto floatToInt = conversions.classifyImplicit(primitive(TypeKind::Float),
                                                 primitive(TypeKind::Int));

  return expect(identity && identity->kind == ConversionKind::Identity &&
                    identity->rank == ConversionRank::Exact,
                "identity conversion was not classified as exact") &&
         expect(widening && widening->kind == ConversionKind::SignedWidening &&
                    widening->rank == ConversionRank::Promotion,
                "signed widening was not classified as a promotion") &&
         expect(narrowing &&
                    narrowing->kind == ConversionKind::SignedNarrowing &&
                    narrowing->rank == ConversionRank::Narrowing,
                "signed narrowing has the wrong classification") &&
         expect(signedness &&
                    signedness->kind == ConversionKind::SignednessChange &&
                    signedness->rank == ConversionRank::Lossy,
                "signedness change has the wrong classification") &&
         expect(intToFloat &&
                    intToFloat->kind == ConversionKind::IntegerToFloat,
                "integer-to-float conversion was rejected") &&
         expect(floatToInt &&
                    floatToInt->kind == ConversionKind::FloatToInteger,
                "float-to-integer conversion was rejected");
}

bool testStringAndReferenceConversions() {
  TypeInterner types;
  ConversionClassifier conversions(types);

  auto toView = conversions.classifyImplicit(zir::makeStringType(),
                                             zir::makeStringViewType());
  auto toOwned = conversions.classifyImplicit(zir::makeStringViewType(),
                                              zir::makeStringType());
  auto charPointer = std::make_shared<PointerType>(primitive(TypeKind::Char));
  auto stringPointer =
      conversions.classifyImplicit(zir::makeStringViewType(), charPointer);
  auto nullPointer =
      conversions.classifyImplicit(primitive(TypeKind::NullPtr), charPointer);

  auto base = std::make_shared<ClassType>("Base", "module.Base");
  auto derived = std::make_shared<ClassType>("Derived", "module.Derived");
  derived->setBase(base);
  auto weakBase = std::make_shared<ClassType>(*base);
  weakBase->setWeak(true);
  auto upcast = conversions.classifyImplicit(derived, base);
  auto toWeak = conversions.classifyImplicit(derived, weakBase);
  auto toStrong = conversions.classifyImplicit(weakBase, base);

  return expect(toView && toView->kind == ConversionKind::StringToView &&
                    toView->rank == ConversionRank::Exact,
                "String-to-view conversion is not an exact match") &&
         expect(toOwned && toOwned->kind == ConversionKind::StringToOwned,
                "view-to-owned String conversion was rejected") &&
         expect(stringPointer &&
                    stringPointer->kind == ConversionKind::StringToCharPointer,
                "String-to-Char-pointer conversion was rejected") &&
         expect(nullPointer &&
                    nullPointer->kind == ConversionKind::NullToPointer,
                "null-to-pointer conversion was rejected") &&
         expect(upcast && upcast->kind == ConversionKind::ClassUpcast,
                "derived-to-base conversion was rejected") &&
         expect(toWeak && toWeak->kind == ConversionKind::StrongToWeak,
                "strong-to-weak conversion was rejected") &&
         expect(!toStrong, "weak-to-strong conversion was accepted");
}

bool testContextSpecificConversions() {
  TypeInterner types;
  ConversionClassifier conversions(types);
  auto charPointer = std::make_shared<PointerType>(primitive(TypeKind::Char));

  auto implicitPointerInteger =
      conversions.classifyImplicit(charPointer, primitive(TypeKind::Int64));
  auto explicitPointerInteger =
      conversions.classifyExplicit(charPointer, primitive(TypeKind::Int64));
  auto cCharPromotion = conversions.classifyCVariadic(
      primitive(TypeKind::Char), primitive(TypeKind::Int32));
  auto cBoolPromotion = conversions.classifyCVariadic(
      primitive(TypeKind::Bool), primitive(TypeKind::Int32));

  return expect(!implicitPointerInteger,
                "pointer-to-integer conversion became implicit") &&
         expect(explicitPointerInteger &&
                    explicitPointerInteger->kind ==
                        ConversionKind::ExplicitPointerInteger,
                "explicit pointer-to-integer conversion was rejected") &&
         expect(cCharPromotion &&
                    cCharPromotion->kind == ConversionKind::CVariadicPromotion,
                "C variadic Char promotion was rejected") &&
         expect(cBoolPromotion &&
                    cBoolPromotion->kind == ConversionKind::CVariadicPromotion,
                "C variadic Bool promotion was rejected");
}

bool testTargetDependentNativeIntegerConversions() {
  TypeInterner types;
  ConversionClassifier conversions32(types, sema::TargetInfo{32});
  ConversionClassifier conversions64(types, sema::TargetInfo{64});

  auto nativeInt = primitive(TypeKind::Int);
  auto fixedInt32 = primitive(TypeKind::Int32);
  auto fixedInt64 = primitive(TypeKind::Int64);
  auto join32 = conversions32.joinTypes(nativeInt, fixedInt64);
  auto join64 = conversions64.joinTypes(nativeInt, fixedInt64);
  auto sameWidth32 = conversions32.classifyImplicit(nativeInt, fixedInt32);
  auto floatAlias = conversions64.classifyImplicit(
      primitive(TypeKind::Float), primitive(TypeKind::Float32));
  auto enumType = std::make_shared<zir::EnumType>(
      "Mode", std::vector<std::string>{"First", "Second"});
  auto enumToNative = conversions64.classifyImplicit(enumType, nativeInt);
  auto enumToFixed = conversions64.classifyImplicit(enumType, fixedInt64);

  return expect(join32 && types.same(join32->type, fixedInt64),
                "32-bit native Int incorrectly dominates fixed Int64") &&
         expect(join64 && types.same(join64->type, nativeInt),
                "64-bit native Int was not preserved by an equal-width join") &&
         expect(sameWidth32 &&
                    sameWidth32->kind == ConversionKind::SignedWidening &&
                    sameWidth32->rank == ConversionRank::Promotion,
                "equal-width native/fixed integer conversion was rejected") &&
         expect(floatAlias && floatAlias->kind == ConversionKind::Identity,
                "Float-to-Float32 alias conversion is not identity") &&
         expect(enumToNative && enumToFixed &&
                    enumToNative->cost() < enumToFixed->cost(),
                "native Int is not preferred for a native enum");
}

bool testCachedConversionUsesRequestedTargetObject() {
  TypeInterner types;
  ConversionClassifier conversions(types);
  auto source = primitive(TypeKind::Int16);
  auto firstTarget = primitive(TypeKind::Int64);
  auto secondTarget = primitive(TypeKind::Int64);

  auto first = conversions.classifyImplicit(source, firstTarget);
  auto second = conversions.classifyImplicit(source, secondTarget);
  return expect(first && first->targetType == firstTarget,
                "uncached conversion lost its requested target object") &&
         expect(second && second->targetType == secondTarget,
                "cached conversion reused a stale target object");
}

bool testSubtypeRelation() {
  TypeInterner types;
  ConversionClassifier conversions(types);
  auto base = std::make_shared<ClassType>("Base", "module.Base");
  auto derived = std::make_shared<ClassType>("Derived", "module.Derived");
  derived->setBase(base);
  auto unrelated = std::make_shared<ClassType>("Other", "module.Other");
  auto weakBase = std::make_shared<ClassType>(*base);
  weakBase->setWeak(true);
  auto weakDerived = std::make_shared<ClassType>(*derived);
  weakDerived->setWeak(true);

  return expect(conversions.isSubtype(derived, base),
                "derived class is not a subtype of its base") &&
         expect(!conversions.isSubtype(base, derived),
                "base class became a subtype of its derived class") &&
         expect(conversions.isSubtype(weakDerived, weakBase),
                "weak class inheritance was not preserved") &&
         expect(!conversions.isSubtype(derived, weakBase),
                "strong-to-weak coercion was classified as subtyping") &&
         expect(!conversions.isSubtype(derived, unrelated),
                "unrelated classes satisfy the subtype relation") &&
         expect(!conversions.isSubtype(primitive(TypeKind::Int16),
                                       primitive(TypeKind::Int64)),
                "numeric widening was classified as subtyping");
}

bool testTypeJoins() {
  TypeInterner types;
  ConversionClassifier conversions(types);

  auto signedJoin = conversions.joinTypes(primitive(TypeKind::Int16),
                                          primitive(TypeKind::UInt8));
  auto reversedSignedJoin = conversions.joinTypes(primitive(TypeKind::UInt8),
                                                  primitive(TypeKind::Int16));
  auto floatJoin = conversions.joinTypes(primitive(TypeKind::Int),
                                         primitive(TypeKind::Float64));
  auto stringJoin =
      conversions.joinTypes(zir::makeStringType(), zir::makeStringViewType());
  auto reversedStringJoin =
      conversions.joinTypes(zir::makeStringViewType(), zir::makeStringType());
  auto charPointer = std::make_shared<PointerType>(primitive(TypeKind::Char));
  auto nullJoin =
      conversions.joinTypes(primitive(TypeKind::NullPtr), charPointer);

  auto base = std::make_shared<ClassType>("Base", "module.Base");
  auto leftClass = std::make_shared<ClassType>("Left", "module.Left");
  auto rightClass = std::make_shared<ClassType>("Right", "module.Right");
  leftClass->setBase(base);
  rightClass->setBase(base);
  auto classJoin = conversions.joinTypes(leftClass, rightClass);
  auto reversedClassJoin = conversions.joinTypes(rightClass, leftClass);
  auto weakLeftClass = std::make_shared<ClassType>(*leftClass);
  weakLeftClass->setWeak(true);
  auto weakClassJoin = conversions.joinTypes(weakLeftClass, rightClass);

  auto leftFailable = zir::makeFailableRecordType(primitive(TypeKind::Int16),
                                                  primitive(TypeKind::Int));
  auto rightFailable = zir::makeFailableRecordType(primitive(TypeKind::UInt8),
                                                   primitive(TypeKind::Int));
  auto failableJoin = conversions.joinTypes(leftFailable, rightFailable);
  auto failableLayout = failableJoin
                            ? zir::getFailableTypeLayout(failableJoin->type)
                            : std::nullopt;

  auto firstRecord = std::make_shared<zir::RecordType>("First", "module.First");
  auto secondRecord =
      std::make_shared<zir::RecordType>("Second", "module.Second");

  return expect(signedJoin &&
                    types.same(signedJoin->type, primitive(TypeKind::Int16)),
                "mixed integer join selected the wrong signed width") &&
         expect(reversedSignedJoin &&
                    types.same(signedJoin->type, reversedSignedJoin->type),
                "numeric join depends on operand order") &&
         expect(floatJoin &&
                    types.same(floatJoin->type, primitive(TypeKind::Float64)),
                "integer/float join selected the wrong float type") &&
         expect(stringJoin && reversedStringJoin &&
                    zir::isIntrinsicStringViewType(stringJoin->type) &&
                    zir::isIntrinsicStringViewType(reversedStringJoin->type),
                "String/StringView join is not stable and borrow-oriented") &&
         expect(nullJoin && types.same(nullJoin->type, charPointer),
                "null/pointer join did not select the pointer type") &&
         expect(classJoin && reversedClassJoin &&
                    types.same(classJoin->type, base) &&
                    types.same(classJoin->type, reversedClassJoin->type),
                "sibling classes did not join at their nearest base") &&
         expect(weakClassJoin &&
                    std::static_pointer_cast<ClassType>(weakClassJoin->type)
                        ->isWeak(),
                "class join lost the weak qualification") &&
         expect(failableLayout && types.same(failableLayout->valueType,
                                             primitive(TypeKind::Int16)),
                "failable join did not join its value types") &&
         expect(!conversions.joinTypes(firstRecord, secondRecord),
                "unrelated nominal records unexpectedly have a join");
}

} // namespace

int main() {
  bool ok = true;
  ok = testNumericConversions() && ok;
  ok = testStringAndReferenceConversions() && ok;
  ok = testContextSpecificConversions() && ok;
  ok = testTargetDependentNativeIntegerConversions() && ok;
  ok = testCachedConversionUsesRequestedTargetObject() && ok;
  ok = testSubtypeRelation() && ok;
  ok = testTypeJoins() && ok;
  return ok ? 0 : 1;
}
