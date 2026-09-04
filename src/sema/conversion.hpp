#pragma once

#include "../ir/type_identity.hpp"
#include "target_info.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace sema {

enum class ConversionKind {
  Identity,
  StringToView,
  StringToOwned,
  EnumToInteger,
  FloatingWidening,
  FloatingNarrowing,
  SignedWidening,
  SignedNarrowing,
  UnsignedWidening,
  UnsignedNarrowing,
  SignednessChange,
  IntegerToFloat,
  FloatToInteger,
  NullToPointer,
  PointerToVoid,
  NullToClass,
  StringToCharPointer,
  ClassUpcast,
  StrongToWeak,
  ArrayToVariadicView,
  Failable,
  CVariadicPromotion,
  ExplicitCharInteger,
  ExplicitPointer,
  ExplicitStringPointer,
  ExplicitPointerString,
  ExplicitPointerInteger,
};

enum class ConversionRank : uint8_t {
  Exact = 0,
  Promotion = 1,
  Numeric = 2,
  NullPointer = 3,
  Narrowing = 4,
  Lossy = 5,
  FloatToInteger = 6,
  Structural = 7,
  Explicit = 8,
};

struct Conversion {
  ConversionKind kind;
  ConversionRank rank;
  std::shared_ptr<zir::Type> targetType;

  int cost() const { return static_cast<int>(rank); }
  bool requiresCast() const { return kind != ConversionKind::Identity; }
  std::string_view description() const;
};

struct TypeJoin {
  std::shared_ptr<zir::Type> type;
  Conversion leftConversion;
  Conversion rightConversion;
};

class ConversionClassifier {
public:
  explicit ConversionClassifier(zir::TypeInterner &types,
                                TargetInfo targetInfo = {})
      : types_(types), targetInfo_(targetInfo) {}

  std::optional<Conversion>
  classifyImplicit(const std::shared_ptr<zir::Type> &source,
                   const std::shared_ptr<zir::Type> &target) const;
  std::optional<Conversion>
  classifyExplicit(const std::shared_ptr<zir::Type> &source,
                   const std::shared_ptr<zir::Type> &target) const;
  std::optional<Conversion>
  classifyCVariadic(const std::shared_ptr<zir::Type> &source,
                    const std::shared_ptr<zir::Type> &target) const;
  bool isSubtype(const std::shared_ptr<zir::Type> &source,
                 const std::shared_ptr<zir::Type> &target) const;
  std::optional<TypeJoin>
  joinTypes(const std::shared_ptr<zir::Type> &left,
            const std::shared_ptr<zir::Type> &right) const;
  void clear() const { implicitCache_.clear(); }

private:
  struct TypePair {
    zir::TypeId source;
    zir::TypeId target;

    bool operator==(const TypePair &other) const {
      return source == other.source && target == other.target;
    }
  };

  struct TypePairHash {
    size_t operator()(const TypePair &pair) const noexcept;
  };

  std::optional<Conversion>
  classifyImplicitUncached(const std::shared_ptr<zir::Type> &source,
                           const std::shared_ptr<zir::Type> &target) const;
  std::optional<TypeJoin>
  makeJoin(const std::shared_ptr<zir::Type> &left,
           const std::shared_ptr<zir::Type> &right,
           const std::shared_ptr<zir::Type> &target) const;

  zir::TypeInterner &types_;
  TargetInfo targetInfo_;
  mutable std::unordered_map<TypePair, std::optional<Conversion>, TypePairHash>
      implicitCache_;
};

bool isVariadicViewType(const std::shared_ptr<zir::Type> &type);

} // namespace sema
