#include "conversion.hpp"

#include "../ir/failable_type.hpp"
#include "../ir/string_type.hpp"

#include <algorithm>

namespace sema {
namespace {

bool isSignedInteger(const std::shared_ptr<zir::Type> &type) {
  auto info = type ? zir::numericTypeInfo(type->getKind()) : std::nullopt;
  return info && info->category == zir::NumericCategory::SignedInteger;
}

int bitWidth(const std::shared_ptr<zir::Type> &type,
             uint16_t nativeBitWidth) {
  auto info = type ? zir::numericTypeInfo(type->getKind()) : std::nullopt;
  return info ? info->bitWidth(nativeBitWidth) : 0;
}

std::shared_ptr<zir::Type> primitive(zir::TypeKind kind) {
  return std::make_shared<zir::PrimitiveType>(kind);
}

std::shared_ptr<zir::Type>
joinNumericTypes(const std::shared_ptr<zir::Type> &left,
                 const std::shared_ptr<zir::Type> &right,
                 uint16_t nativeBitWidth) {
  if (left->isFloatingPoint() || right->isFloatingPoint()) {
    if (left->getKind() == zir::TypeKind::Float64 ||
        right->getKind() == zir::TypeKind::Float64) {
      return primitive(zir::TypeKind::Float64);
    }
    auto kind = left->getKind() == zir::TypeKind::Float ||
                        right->getKind() == zir::TypeKind::Float
                    ? zir::TypeKind::Float
                    : zir::TypeKind::Float32;
    return primitive(kind);
  }

  int leftWidth = bitWidth(left, nativeBitWidth);
  int rightWidth = bitWidth(right, nativeBitWidth);
  int width = std::max(leftWidth, rightWidth);
  bool leftUnsigned = left->isUnsigned();
  bool rightUnsigned = right->isUnsigned();

  bool useUnsigned = leftUnsigned && rightUnsigned;
  if (leftUnsigned != rightUnsigned) {
    int unsignedWidth = leftUnsigned ? leftWidth : rightWidth;
    int signedWidth = leftUnsigned ? rightWidth : leftWidth;
    useUnsigned = unsignedWidth >= signedWidth;
  }

  auto leftInfo = *zir::numericTypeInfo(left->getKind());
  auto rightInfo = *zir::numericTypeInfo(right->getKind());
  bool preferNative = width == nativeBitWidth &&
                      ((leftInfo.isNative && leftUnsigned == useUnsigned) ||
                       (rightInfo.isNative && rightUnsigned == useUnsigned));
  if (preferNative) {
    return primitive(useUnsigned ? zir::TypeKind::UInt : zir::TypeKind::Int);
  }

  if (useUnsigned) {
    if (width <= 8) {
      return primitive(zir::TypeKind::UInt8);
    }
    if (width <= 16) {
      return primitive(zir::TypeKind::UInt16);
    }
    if (width <= 32) {
      return primitive(zir::TypeKind::UInt32);
    }
    return primitive(zir::TypeKind::UInt64);
  }

  if (width <= 8) {
    return primitive(zir::TypeKind::Int8);
  }
  if (width <= 16) {
    return primitive(zir::TypeKind::Int16);
  }
  if (width <= 32) {
    return primitive(zir::TypeKind::Int32);
  }
  return primitive(zir::TypeKind::Int64);
}

bool hasClassAncestor(const std::shared_ptr<zir::ClassType> &source,
                      const std::string &targetCodegenName) {
  for (auto current = source; current; current = current->getBase()) {
    if (current->getCodegenName() == targetCodegenName) {
      return true;
    }
  }
  return false;
}

Conversion conversion(ConversionKind kind, ConversionRank rank,
                      const std::shared_ptr<zir::Type> &target) {
  return {kind, rank, target};
}

} // namespace

std::string_view Conversion::description() const {
  switch (kind) {
  case ConversionKind::Identity:
    return "exact match";
  case ConversionKind::StringToView:
    return "string-to-view conversion";
  case ConversionKind::StringToOwned:
    return "view-to-owned-string conversion";
  case ConversionKind::EnumToInteger:
    return "enum-to-integer conversion";
  case ConversionKind::FloatingWidening:
    return "floating widening";
  case ConversionKind::FloatingNarrowing:
    return "floating narrowing";
  case ConversionKind::SignedWidening:
    return "signed widening";
  case ConversionKind::SignedNarrowing:
    return "signed narrowing";
  case ConversionKind::UnsignedWidening:
    return "unsigned widening";
  case ConversionKind::UnsignedNarrowing:
    return "unsigned narrowing";
  case ConversionKind::SignednessChange:
    return "signedness-changing integer conversion";
  case ConversionKind::IntegerToFloat:
    return "integer-to-float conversion";
  case ConversionKind::FloatToInteger:
    return "float-to-integer conversion";
  case ConversionKind::NullToPointer:
  case ConversionKind::NullToClass:
    return "null-to-reference conversion";
  case ConversionKind::StringToCharPointer:
    return "string-to-character-pointer conversion";
  case ConversionKind::ClassUpcast:
    return "derived-to-base conversion";
  case ConversionKind::StrongToWeak:
    return "strong-to-weak conversion";
  case ConversionKind::ArrayToVariadicView:
    return "array-to-variadic-view conversion";
  case ConversionKind::Failable:
    return "failable conversion";
  case ConversionKind::CVariadicPromotion:
    return "C variadic promotion";
  case ConversionKind::ExplicitCharInteger:
    return "explicit character/integer conversion";
  case ConversionKind::ExplicitPointer:
    return "explicit pointer conversion";
  case ConversionKind::ExplicitStringPointer:
    return "explicit string-to-pointer conversion";
  case ConversionKind::ExplicitPointerString:
    return "explicit pointer-to-string conversion";
  case ConversionKind::ExplicitPointerInteger:
    return "explicit pointer/integer conversion";
  }
  return "conversion";
}

size_t ConversionClassifier::TypePairHash::operator()(
    const TypePair &pair) const noexcept {
  size_t result = zir::TypeIdHash{}(pair.source);
  result ^= zir::TypeIdHash{}(pair.target) + 0x9e3779b9U +
            (result << 6U) + (result >> 2U);
  return result;
}

bool isVariadicViewType(const std::shared_ptr<zir::Type> &type) {
  return type && type->getKind() == zir::TypeKind::Record &&
         static_cast<const zir::RecordType &>(*type).getRole() ==
             zir::RecordRole::VariadicView;
}

bool ConversionClassifier::isSubtype(
    const std::shared_ptr<zir::Type> &source,
    const std::shared_ptr<zir::Type> &target) const {
  if (!source || !target) {
    return false;
  }
  if (types_.same(source, target)) {
    return true;
  }
  if (source->getKind() != zir::TypeKind::Class ||
      target->getKind() != zir::TypeKind::Class) {
    return false;
  }

  const auto sourceClass = std::static_pointer_cast<zir::ClassType>(source);
  const auto targetClass = std::static_pointer_cast<zir::ClassType>(target);
  if (sourceClass->isWeak() != targetClass->isWeak()) {
    return false;
  }
  if (targetClass->isInterface()) {
    return sourceClass->implementsInterface(targetClass->getCodegenName());
  }
  return hasClassAncestor(sourceClass, targetClass->getCodegenName());
}

std::optional<Conversion> ConversionClassifier::classifyImplicit(
    const std::shared_ptr<zir::Type> &source,
    const std::shared_ptr<zir::Type> &target) const {
  if (!source || !target) {
    return std::nullopt;
  }
  const TypePair key{types_.intern(source), types_.intern(target)};
  if (auto cached = implicitCache_.find(key); cached != implicitCache_.end()) {
    auto result = cached->second;
    if (result) {
      result->targetType = target;
    }
    return result;
  }
  auto result = classifyImplicitUncached(source, target);
  implicitCache_.emplace(key, result);
  return result;
}

std::optional<Conversion> ConversionClassifier::classifyImplicitUncached(
    const std::shared_ptr<zir::Type> &source,
    const std::shared_ptr<zir::Type> &target) const {
  if (types_.same(source, target)) {
    return conversion(ConversionKind::Identity, ConversionRank::Exact, target);
  }

  if (source->getKind() == zir::TypeKind::NullPtr) {
    if (target->getKind() == zir::TypeKind::Pointer) {
      return conversion(ConversionKind::NullToPointer,
                        ConversionRank::NullPointer, target);
    }
    if (target->getKind() == zir::TypeKind::Class) {
      return conversion(ConversionKind::NullToClass,
                        ConversionRank::Structural, target);
    }
  }

  if (zir::isIntrinsicStringType(source) &&
      zir::isIntrinsicStringType(target)) {
    if (zir::isIntrinsicStringViewType(target)) {
      return conversion(ConversionKind::StringToView, ConversionRank::Exact,
                        target);
    }
    return conversion(ConversionKind::StringToOwned, ConversionRank::Promotion,
                      target);
  }

  if (zir::isIntrinsicStringType(source) &&
      target->getKind() == zir::TypeKind::Pointer) {
    const auto &pointer = static_cast<const zir::PointerType &>(*target);
    if (pointer.getBaseType()->getKind() == zir::TypeKind::Char) {
      return conversion(ConversionKind::StringToCharPointer,
                        ConversionRank::Structural, target);
    }
  }

  if (source->getKind() == zir::TypeKind::Class &&
      target->getKind() == zir::TypeKind::Class) {
    auto sourceClass = std::static_pointer_cast<zir::ClassType>(source);
    auto targetClass = std::static_pointer_cast<zir::ClassType>(target);
    if (isSubtype(source, target)) {
      return conversion(ConversionKind::ClassUpcast,
                        ConversionRank::Structural, target);
    }
    if (!sourceClass->isWeak() && targetClass->isWeak() &&
        hasClassAncestor(sourceClass, targetClass->getCodegenName())) {
      return conversion(ConversionKind::StrongToWeak,
                        ConversionRank::Structural, target);
    }
  }

  if (source->getKind() == zir::TypeKind::Array &&
      isVariadicViewType(target)) {
    const auto &array = static_cast<const zir::ArrayType &>(*source);
    const auto &view = static_cast<const zir::RecordType &>(*target);
    if (!view.getFields().empty() && view.getFields()[0].type &&
        view.getFields()[0].type->getKind() == zir::TypeKind::Pointer) {
      const auto &data =
          static_cast<const zir::PointerType &>(*view.getFields()[0].type);
      if (types_.same(array.getBaseType(), data.getBaseType())) {
        return conversion(ConversionKind::ArrayToVariadicView,
                          ConversionRank::Structural, target);
      }
    }
  }

  auto sourceFailable = zir::getFailableTypeLayout(source);
  auto targetFailable = zir::getFailableTypeLayout(target);
  if (sourceFailable && targetFailable &&
      classifyImplicit(sourceFailable->valueType, targetFailable->valueType) &&
      classifyImplicit(sourceFailable->errorType, targetFailable->errorType)) {
    return conversion(ConversionKind::Failable, ConversionRank::Structural,
                      target);
  }

  if (source->getKind() == zir::TypeKind::Enum && target->isInteger()) {
    const auto &enumType = static_cast<const zir::EnumType &>(*source);
    int sourceWidth = enumType.hasReprC
                          ? 32
                          : targetInfo_.nativeIntegerBitWidth();
    int targetWidth =
        bitWidth(target, targetInfo_.nativeIntegerBitWidth());
    auto targetInfo = zir::numericTypeInfo(target->getKind());
    auto rank = ConversionRank::Narrowing;
    if (target->isUnsigned()) {
      rank = ConversionRank::Lossy;
    } else if (targetInfo && targetInfo->isNative) {
      rank = ConversionRank::Promotion;
    } else if (targetWidth >= sourceWidth) {
      // A fixed-width integer can represent the enum, but the enum's natural
      // representation is the native signed integer.
      rank = ConversionRank::Numeric;
    }
    return conversion(ConversionKind::EnumToInteger, rank, target);
  }

  if (source->isFloatingPoint() && target->isFloatingPoint()) {
    bool widening = bitWidth(target, targetInfo_.nativeIntegerBitWidth()) >=
                    bitWidth(source, targetInfo_.nativeIntegerBitWidth());
    return conversion(widening ? ConversionKind::FloatingWidening
                               : ConversionKind::FloatingNarrowing,
                      widening ? ConversionRank::Promotion
                               : ConversionRank::Lossy,
                      target);
  }

  if (isSignedInteger(source) && isSignedInteger(target)) {
    bool widening = bitWidth(target, targetInfo_.nativeIntegerBitWidth()) >=
                    bitWidth(source, targetInfo_.nativeIntegerBitWidth());
    return conversion(widening ? ConversionKind::SignedWidening
                               : ConversionKind::SignedNarrowing,
                      widening ? ConversionRank::Promotion
                               : ConversionRank::Narrowing,
                      target);
  }

  if (source->isInteger() && source->isUnsigned() && target->isInteger() &&
      target->isUnsigned()) {
    bool widening = bitWidth(target, targetInfo_.nativeIntegerBitWidth()) >=
                    bitWidth(source, targetInfo_.nativeIntegerBitWidth());
    return conversion(widening ? ConversionKind::UnsignedWidening
                               : ConversionKind::UnsignedNarrowing,
                      widening ? ConversionRank::Promotion
                               : ConversionRank::Narrowing,
                      target);
  }

  if (source->isInteger() && target->isFloatingPoint()) {
    bool preservesWidth =
        bitWidth(target, targetInfo_.nativeIntegerBitWidth()) >=
        bitWidth(source, targetInfo_.nativeIntegerBitWidth());
    return conversion(ConversionKind::IntegerToFloat,
                      preservesWidth ? ConversionRank::Numeric
                                     : ConversionRank::Lossy,
                      target);
  }

  if (source->isFloatingPoint() && target->isInteger()) {
    return conversion(ConversionKind::FloatToInteger,
                      ConversionRank::FloatToInteger, target);
  }

  if (source->isInteger() && target->isInteger()) {
    return conversion(ConversionKind::SignednessChange, ConversionRank::Lossy,
                      target);
  }

  return std::nullopt;
}

std::optional<TypeJoin> ConversionClassifier::makeJoin(
    const std::shared_ptr<zir::Type> &left,
    const std::shared_ptr<zir::Type> &right,
    const std::shared_ptr<zir::Type> &target) const {
  auto leftConversion = classifyImplicit(left, target);
  auto rightConversion = classifyImplicit(right, target);
  if (!leftConversion || !rightConversion) {
    return std::nullopt;
  }
  return TypeJoin{target, *leftConversion, *rightConversion};
}

std::optional<TypeJoin> ConversionClassifier::joinTypes(
    const std::shared_ptr<zir::Type> &left,
    const std::shared_ptr<zir::Type> &right) const {
  if (!left || !right) {
    return std::nullopt;
  }
  if (types_.same(left, right)) {
    return makeJoin(left, right, left);
  }

  bool leftNumeric = left->isInteger() || left->isFloatingPoint();
  bool rightNumeric = right->isInteger() || right->isFloatingPoint();
  if (leftNumeric && rightNumeric) {
    return makeJoin(left, right,
                    joinNumericTypes(left, right,
                                     targetInfo_.nativeIntegerBitWidth()));
  }

  if (zir::isIntrinsicStringType(left) &&
      zir::isIntrinsicStringType(right)) {
    auto target = zir::makeStringViewType();
    if (zir::isIntrinsicStringViewType(left)) {
      target = std::static_pointer_cast<zir::RecordType>(left);
    } else if (zir::isIntrinsicStringViewType(right)) {
      target = std::static_pointer_cast<zir::RecordType>(right);
    }
    return makeJoin(left, right, target);
  }

  if (left->getKind() == zir::TypeKind::Class &&
      right->getKind() == zir::TypeKind::Class) {
    auto leftClass = std::static_pointer_cast<zir::ClassType>(left);
    auto rightClass = std::static_pointer_cast<zir::ClassType>(right);
    std::shared_ptr<zir::ClassType> commonClass;
    for (auto leftAncestor = leftClass; leftAncestor && !commonClass;
         leftAncestor = leftAncestor->getBase()) {
      for (auto rightAncestor = rightClass; rightAncestor;
           rightAncestor = rightAncestor->getBase()) {
        if (leftAncestor->getCodegenName() ==
            rightAncestor->getCodegenName()) {
          commonClass = leftAncestor;
          break;
        }
      }
    }
    if (commonClass) {
      bool weak = leftClass->isWeak() || rightClass->isWeak();
      std::shared_ptr<zir::ClassType> target = commonClass;
      if (weak && !target->isWeak()) {
        if (leftClass->isWeak() &&
            leftClass->getCodegenName() == target->getCodegenName()) {
          target = leftClass;
        } else if (rightClass->isWeak() &&
                   rightClass->getCodegenName() == target->getCodegenName()) {
          target = rightClass;
        } else {
          target = std::make_shared<zir::ClassType>(*commonClass);
          target->setWeak(true);
        }
      }
      return makeJoin(left, right, target);
    }
  }

  auto leftFailable = zir::getFailableTypeLayout(left);
  auto rightFailable = zir::getFailableTypeLayout(right);
  if (leftFailable && rightFailable) {
    auto valueJoin =
        joinTypes(leftFailable->valueType, rightFailable->valueType);
    auto errorJoin =
        joinTypes(leftFailable->errorType, rightFailable->errorType);
    if (valueJoin && errorJoin) {
      return makeJoin(left, right, zir::makeFailableRecordType(
                                       valueJoin->type, errorJoin->type));
    }
  }

  auto leftToRight = classifyImplicit(left, right);
  auto rightToLeft = classifyImplicit(right, left);
  if (leftToRight && !rightToLeft) {
    return makeJoin(left, right, right);
  }
  if (rightToLeft && !leftToRight) {
    return makeJoin(left, right, left);
  }
  if (leftToRight && rightToLeft) {
    if (leftToRight->cost() < rightToLeft->cost()) {
      return makeJoin(left, right, right);
    }
    if (rightToLeft->cost() < leftToRight->cost()) {
      return makeJoin(left, right, left);
    }
  }
  return std::nullopt;
}

std::optional<Conversion> ConversionClassifier::classifyExplicit(
    const std::shared_ptr<zir::Type> &source,
    const std::shared_ptr<zir::Type> &target) const {
  if (auto implicit = classifyImplicit(source, target)) {
    return implicit;
  }
  if (!source || !target) {
    return std::nullopt;
  }

  bool sourceChar = source->getKind() == zir::TypeKind::Char;
  bool targetChar = target->getKind() == zir::TypeKind::Char;
  if ((sourceChar && target->isInteger()) ||
      (source->isInteger() && targetChar)) {
    return conversion(ConversionKind::ExplicitCharInteger,
                      ConversionRank::Explicit, target);
  }
  if (source->getKind() == zir::TypeKind::Enum && target->isInteger()) {
    return conversion(ConversionKind::EnumToInteger, ConversionRank::Explicit,
                      target);
  }

  bool sourcePointer = source->getKind() == zir::TypeKind::Pointer;
  bool targetPointer = target->getKind() == zir::TypeKind::Pointer;
  if ((sourcePointer || source->getKind() == zir::TypeKind::NullPtr) &&
      targetPointer) {
    return conversion(ConversionKind::ExplicitPointer,
                      ConversionRank::Explicit, target);
  }
  if (zir::isIntrinsicStringType(source) && targetPointer) {
    return conversion(ConversionKind::ExplicitStringPointer,
                      ConversionRank::Explicit, target);
  }
  if (sourcePointer && zir::isIntrinsicStringType(target)) {
    const auto &pointer = static_cast<const zir::PointerType &>(*source);
    auto baseKind = pointer.getBaseType()->getKind();
    if (baseKind == zir::TypeKind::Void || baseKind == zir::TypeKind::Char) {
      return conversion(ConversionKind::ExplicitPointerString,
                        ConversionRank::Explicit, target);
    }
  }
  if ((sourcePointer && target->isInteger()) ||
      (source->isInteger() && targetPointer)) {
    return conversion(ConversionKind::ExplicitPointerInteger,
                      ConversionRank::Explicit, target);
  }
  return std::nullopt;
}

std::optional<Conversion> ConversionClassifier::classifyCVariadic(
    const std::shared_ptr<zir::Type> &source,
    const std::shared_ptr<zir::Type> &target) const {
  if (auto implicit = classifyImplicit(source, target)) {
    return implicit;
  }
  if (!source || !target) {
    return std::nullopt;
  }
  auto sourceKind = source->getKind();
  auto numericTarget = zir::numericTypeInfo(target->getKind());
  if (numericTarget &&
      numericTarget->category == zir::NumericCategory::SignedInteger &&
      numericTarget->bitWidth(targetInfo_.nativeIntegerBitWidth()) == 32 &&
      (sourceKind == zir::TypeKind::Bool ||
       sourceKind == zir::TypeKind::Char)) {
    return conversion(ConversionKind::CVariadicPromotion,
                      ConversionRank::Promotion, target);
  }
  return std::nullopt;
}

} // namespace sema
