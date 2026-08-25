#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace zir {

enum class TypeKind {
  // These values are part of the z1 type-mangling schema. Append new kinds.
  Void = 0,
  Int8 = 1,
  Int16 = 2,
  Int32 = 3,
  Int64 = 4,
  UInt8 = 5,
  UInt16 = 6,
  UInt32 = 7,
  UInt64 = 8,
  Int = 9,   // Native signed integer (pointer width)
  UInt = 10, // Native unsigned integer (pointer width)
  Float = 11,
  Float32 = 12,
  Float64 = 13,
  Bool = 14,
  Char = 15,
  Pointer = 16,
  NullPtr = 17,
  Record = 18,
  Class = 19,
  Array = 20,
  Enum = 21,
  TaggedUnion = 22,
  FunctionPointer = 23
};

enum class NumericCategory { SignedInteger, UnsignedInteger, FloatingPoint };

struct NumericTypeInfo {
  NumericCategory category;
  uint16_t fixedBitWidth;
  bool isNative;

  uint16_t bitWidth(uint16_t nativeBitWidth) const {
    return isNative ? nativeBitWidth : fixedBitWidth;
  }
};

std::optional<NumericTypeInfo> numericTypeInfo(TypeKind kind);
TypeKind canonicalPrimitiveKind(TypeKind kind);
std::string_view primitiveIrName(TypeKind kind);

enum class IntrinsicTypeKind {
  None = 0,
  String = 1,
  StringView = 2,
};

// These records have a physical record ABI, but distinct semantic roles.
// The tag prevents semantic code from inferring that role from generated names.
enum class RecordRole {
  User,
  Failable,
  VariadicView,
  GenericParameter,
};

enum class RecordMutability {
  Mutable,
  Immutable,
};

class Type {
  IntrinsicTypeKind intrinsicKind;

protected:
  explicit Type(IntrinsicTypeKind intrinsic = IntrinsicTypeKind::None)
      : intrinsicKind(intrinsic) {}

public:
  virtual ~Type() = default;
  virtual TypeKind getKind() const = 0;
  virtual std::string toString() const = 0;
  virtual bool isReferenceType() const { return false; }
  virtual bool isPointerLike() const {
    auto k = getKind();
    return k == TypeKind::Pointer || k == TypeKind::NullPtr;
  }
  virtual bool isInteger() const {
    auto info = numericTypeInfo(getKind());
    return info && info->category != NumericCategory::FloatingPoint;
  }
  virtual bool isUnsigned() const {
    auto info = numericTypeInfo(getKind());
    return info && info->category == NumericCategory::UnsignedInteger;
  }
  virtual bool isFloatingPoint() const {
    auto info = numericTypeInfo(getKind());
    return info && info->category == NumericCategory::FloatingPoint;
  }
  IntrinsicTypeKind getIntrinsicKind() const { return intrinsicKind; }
};

class PrimitiveType : public Type {
  TypeKind kind;

public:
  PrimitiveType(TypeKind k) : kind(k) {}
  TypeKind getKind() const override { return kind; }
  std::string toString() const override;
};

class PointerType : public Type {
  std::shared_ptr<Type> base;

public:
  PointerType(std::shared_ptr<Type> b) : base(std::move(b)) {}
  TypeKind getKind() const override { return TypeKind::Pointer; }
  std::string toString() const override { return base->toString() + "*"; }
  bool isReferenceType() const override { return true; }
  std::shared_ptr<Type> getBaseType() const { return base; }
};

class RecordType : public Type {
public:
  struct Field {
    std::string name;
    std::shared_ptr<Type> type;
    int visibility = 0;
  };

protected:
  std::string name;
  std::string codegenName;
  std::vector<Field> fields;
  std::string genericBaseName;
  std::string genericCodegenBaseName;
  std::vector<std::shared_ptr<Type>> genericArguments;
  RecordRole role = RecordRole::User;
  RecordMutability mutability = RecordMutability::Mutable;

public:
  bool hasReprC = false;
  bool isPacked = false;

  RecordType(std::string n, std::string codegen = "",
             IntrinsicTypeKind intrinsic = IntrinsicTypeKind::None,
             RecordRole recordRole = RecordRole::User,
             RecordMutability recordMutability = RecordMutability::Mutable)
      : Type(intrinsic), name(std::move(n)),
        codegenName(codegen.empty() ? name : std::move(codegen)),
        role(recordRole), mutability(recordMutability) {}
  TypeKind getKind() const override { return TypeKind::Record; }
  std::string toString() const override { return "%" + name; }
  bool isReferenceType() const override { return true; }

  void addField(std::string n, std::shared_ptr<Type> t) {
    fields.push_back({std::move(n), std::move(t), 0});
  }

  void addField(std::string n, std::shared_ptr<Type> t, int visibility) {
    fields.push_back({std::move(n), std::move(t), visibility});
  }

  const std::vector<Field> &getFields() const { return fields; }
  void clearFields() { fields.clear(); }
  const std::string &getName() const { return name; }
  const std::string &getCodegenName() const { return codegenName; }
  const std::string &getGenericBaseName() const { return genericBaseName; }
  const std::string &getGenericCodegenBaseName() const {
    return genericCodegenBaseName;
  }
  const std::vector<std::shared_ptr<Type>> &getGenericArguments() const {
    return genericArguments;
  }
  RecordRole getRole() const { return role; }
  RecordMutability getMutability() const { return mutability; }
  bool hasImmutableFields() const {
    return mutability == RecordMutability::Immutable;
  }
  bool isGenericInstance() const { return !genericBaseName.empty(); }

  void setMutability(RecordMutability value) { mutability = value; }

  void setGenericInstance(std::string baseName, std::string codegenBaseName,
                          std::vector<std::shared_ptr<Type>> args) {
    genericBaseName = std::move(baseName);
    genericCodegenBaseName = std::move(codegenBaseName);
    genericArguments = std::move(args);
  }
};

inline std::shared_ptr<RecordType> makeGenericParameterType(std::string name) {
  return std::make_shared<RecordType>(name, name, IntrinsicTypeKind::None,
                                      RecordRole::GenericParameter);
}

class ClassType : public RecordType {
public:
  struct InterfaceConformance {
    std::string interfaceCodegenName;
    std::vector<int> methodVtableSlots;
  };

  struct InterfaceMethod {
    std::string name;
    std::string linkName;
  };

private:
  std::shared_ptr<ClassType> base;
  bool weakRef = false;
  bool isInterface_ = false;
  std::vector<InterfaceConformance> interfaces_;
  std::vector<InterfaceMethod> interfaceMethods_;

public:
  ClassType(std::string n, std::string codegen = "")
      : RecordType(std::move(n), std::move(codegen)) {}

  TypeKind getKind() const override { return TypeKind::Class; }
  std::string toString() const override {
    return std::string(weakRef ? "weak class " : "class ") + name;
  }
  bool isReferenceType() const override { return true; }

  void setBase(std::shared_ptr<ClassType> b) { base = std::move(b); }
  std::shared_ptr<ClassType> getBase() const { return base; }
  void setWeak(bool weak) { weakRef = weak; }
  bool isWeak() const { return weakRef; }

  void setIsInterface(bool value) { isInterface_ = value; }
  bool isInterface() const { return isInterface_; }

  void addInterfaceConformance(InterfaceConformance conformance) {
    interfaces_.push_back(std::move(conformance));
  }
  const std::vector<InterfaceConformance> &getInterfaceConformances() const {
    return interfaces_;
  }
  bool implementsInterface(const std::string &interfaceCodegenName) const {
    for (const auto &c : interfaces_) {
      if (c.interfaceCodegenName == interfaceCodegenName) {
        return true;
      }
    }
    return false;
  }

  void setInterfaceMethods(std::vector<InterfaceMethod> methods) {
    interfaceMethods_ = std::move(methods);
  }
  const std::vector<InterfaceMethod> &getInterfaceMethods() const {
    return interfaceMethods_;
  }
};

class EnumType : public Type {
public:
  struct Variant {
    std::string name;
    int64_t discriminant = 0;
  };

private:
  std::string name;
  std::string codegenName;
  std::vector<Variant> variants;

public:
  bool hasReprC = false;

  EnumType(std::string n, std::vector<Variant> v, std::string codegen = "")
      : name(std::move(n)),
        codegenName(codegen.empty() ? name : std::move(codegen)),
        variants(std::move(v)) {}

  EnumType(std::string n, std::vector<std::string> v, std::string codegen = "")
      : name(std::move(n)),
        codegenName(codegen.empty() ? name : std::move(codegen)) {
    variants.reserve(v.size());
    for (size_t i = 0; i < v.size(); ++i) {
      variants.push_back(Variant{std::move(v[i]), static_cast<int64_t>(i)});
    }
  }

  TypeKind getKind() const override { return TypeKind::Enum; }
  std::string toString() const override { return "enum " + name; }
  bool isReferenceType() const override { return false; }

  const std::vector<Variant> &getVariants() const { return variants; }
  std::vector<std::string> getVariantNames() const {
    std::vector<std::string> names;
    names.reserve(variants.size());
    for (const auto &variant : variants) {
      names.push_back(variant.name);
    }
    return names;
  }

  const std::string &getName() const { return name; }
  const std::string &getCodegenName() const { return codegenName; }

  int64_t getVariantDiscriminant(const std::string &variantName) const {
    for (const auto &variant : variants) {
      if (variant.name == variantName) {
        return variant.discriminant;
      }
    }
    return -1;
  }

  int getVariantIndex(const std::string &variantName) const {
    for (size_t i = 0; i < variants.size(); ++i) {
      if (variants[i].name == variantName) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }
};

class TaggedUnionType : public Type {
public:
  struct Variant {
    std::string name;
    std::shared_ptr<Type> payloadType;
    int64_t tag = 0;
  };

private:
  std::string name;
  std::string codegenName;
  std::vector<Variant> variants;

public:
  TaggedUnionType(std::string n, std::vector<Variant> v,
                  std::string codegen = "")
      : name(std::move(n)),
        codegenName(codegen.empty() ? name : std::move(codegen)),
        variants(std::move(v)) {}

  TypeKind getKind() const override { return TypeKind::TaggedUnion; }
  std::string toString() const override { return "enum " + name; }
  bool isReferenceType() const override { return true; }

  const std::string &getName() const { return name; }
  const std::string &getCodegenName() const { return codegenName; }
  const std::vector<Variant> &getVariants() const { return variants; }
  void setVariants(std::vector<Variant> v) { variants = std::move(v); }
  void clearVariants() { variants.clear(); }

  const Variant *findVariant(const std::string &variantName) const {
    for (const auto &variant : variants) {
      if (variant.name == variantName)
        return &variant;
    }
    return nullptr;
  }
};

class ArrayType : public Type {
  std::shared_ptr<Type> base;
  size_t size;

public:
  ArrayType(std::shared_ptr<Type> b, size_t s) : base(std::move(b)), size(s) {}
  TypeKind getKind() const override { return TypeKind::Array; }
  std::string toString() const override {
    return "[" + std::to_string(size) + "]" + base->toString();
  }
  std::shared_ptr<Type> getBaseType() const { return base; }
  size_t getSize() const { return size; }
};

enum class ParameterOwnership {
  Borrow,
  Transfer,
  Sink,
};

enum class ParameterEscape {
  Unspecified,
  NoEscape,
};

class ResultBorrowContract {
  std::optional<size_t> sourceParameter_;

  explicit ResultBorrowContract(std::optional<size_t> sourceParameter)
      : sourceParameter_(sourceParameter) {}

public:
  ResultBorrowContract() = default;

  static ResultBorrowContract fromParameter(size_t parameterIndex) {
    return ResultBorrowContract(parameterIndex);
  }

  bool hasSource() const { return sourceParameter_.has_value(); }
  const std::optional<size_t> &sourceParameter() const {
    return sourceParameter_;
  }

  bool operator==(const ResultBorrowContract &other) const {
    return sourceParameter_ == other.sourceParameter_;
  }
  bool operator!=(const ResultBorrowContract &other) const {
    return !(*this == other);
  }
};

inline bool transfersOwnership(ParameterOwnership ownership) {
  return ownership == ParameterOwnership::Transfer ||
         ownership == ParameterOwnership::Sink;
}

inline bool containsManagedValues(const std::shared_ptr<Type> &type);

class FunctionPointerType : public Type {
  std::vector<std::shared_ptr<Type>> params;
  std::vector<ParameterOwnership> parameterOwnership;
  std::vector<ParameterEscape> parameterEscapes;
  std::shared_ptr<Type> returnType;
  ResultBorrowContract resultBorrow_;
  bool returnsRef_ = false;

public:
  FunctionPointerType(std::vector<std::shared_ptr<Type>> p,
                      std::shared_ptr<Type> r,
                      std::vector<ParameterOwnership> ownership = {},
                      std::vector<ParameterEscape> escape = {},
                      ResultBorrowContract resultBorrow = {},
                      bool returnsRef = false)
      : params(std::move(p)), parameterOwnership(std::move(ownership)),
        parameterEscapes(std::move(escape)), returnType(std::move(r)),
        resultBorrow_(resultBorrow), returnsRef_(returnsRef) {
    if (parameterOwnership.empty()) {
      parameterOwnership.assign(params.size(), ParameterOwnership::Borrow);
    } else if (parameterOwnership.size() != params.size()) {
      throw std::invalid_argument(
          "function pointer parameter ownership count mismatch");
    }
    if (parameterEscapes.empty()) {
      parameterEscapes.assign(params.size(), ParameterEscape::Unspecified);
    } else if (parameterEscapes.size() != params.size()) {
      throw std::invalid_argument(
          "function pointer parameter escape count mismatch");
    }
    if (resultBorrow_.hasSource() &&
        *resultBorrow_.sourceParameter() >= params.size()) {
      throw std::invalid_argument(
          "function pointer result borrow source is out of range");
    }
  }
  TypeKind getKind() const override { return TypeKind::FunctionPointer; }
  std::string toString() const override {
    std::string s = "*fun(";
    for (size_t i = 0; i < params.size(); ++i) {
      if (i)
        s += ", ";
      if (containsManagedValues(params[i])) {
        switch (parameterOwnership[i]) {
        case ParameterOwnership::Borrow:
          s += "borrow ";
          break;
        case ParameterOwnership::Transfer:
          s += "transfer ";
          break;
        case ParameterOwnership::Sink:
          s += "sink ";
          break;
        }
      }
      if (parameterEscapes[i] == ParameterEscape::NoEscape) {
        s += "noescape ";
      }
      s += params[i]->toString();
    }
    s += ") " + returnType->toString();
    if (returnsRef_) {
      s += "*";
    }
    if (resultBorrow_.hasSource()) {
      s += " borrows(" +
           std::to_string(*resultBorrow_.sourceParameter()) + ")";
    }
    return s;
  }
  const std::vector<std::shared_ptr<Type>> &getParams() const { return params; }
  const std::vector<ParameterOwnership> &getParameterOwnership() const {
    return parameterOwnership;
  }
  const std::vector<ParameterEscape> &getParameterEscapes() const {
    return parameterEscapes;
  }
  const std::shared_ptr<Type> &getReturnType() const { return returnType; }
  const ResultBorrowContract &getResultBorrow() const { return resultBorrow_; }
  bool returnsRef() const { return returnsRef_; }
};

inline bool containsManagedValues(const std::shared_ptr<Type> &type) {
  if (!type) {
    return false;
  }
  if (type->getKind() == TypeKind::Class ||
      type->getIntrinsicKind() == IntrinsicTypeKind::String) {
    return true;
  }
  if (type->getKind() == TypeKind::Record) {
    const auto &record = static_cast<const RecordType &>(*type);
    for (const auto &field : record.getFields()) {
      if (containsManagedValues(field.type)) {
        return true;
      }
    }
    return false;
  }
  if (type->getKind() == TypeKind::Array) {
    const auto &array = static_cast<const ArrayType &>(*type);
    return containsManagedValues(array.getBaseType());
  }
  if (type->getKind() == TypeKind::TaggedUnion) {
    const auto &taggedUnion = static_cast<const TaggedUnionType &>(*type);
    for (const auto &variant : taggedUnion.getVariants()) {
      if (containsManagedValues(variant.payloadType)) {
        return true;
      }
    }
  }
  return false;
}

} // namespace zir
