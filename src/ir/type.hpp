#pragma once
#include <memory>
#include <string>
#include <vector>

namespace zir {

enum class TypeKind {
  Void,
  Int8,
  Int16,
  Int32,
  Int64,
  UInt8,
  UInt16,
  UInt32,
  UInt64,
  Int, // Default Int (32-bit)
  UInt, // Default UInt (32-bit)
  Float,
  Float32,
  Float64,
  Bool,
  Char,
  Pointer,
  Record,
  Array,
  Enum
};

class Type {
public:
  virtual ~Type() = default;
  virtual TypeKind getKind() const = 0;
  virtual std::string toString() const = 0;
  virtual bool isReferenceType() const { return false; }
  virtual bool isInteger() const {
    auto k = getKind();
    return k == TypeKind::Int8 || k == TypeKind::Int16 || k == TypeKind::Int32 ||
           k == TypeKind::Int64 || k == TypeKind::UInt8 ||
           k == TypeKind::UInt16 || k == TypeKind::UInt32 ||
           k == TypeKind::UInt64 || k == TypeKind::Int || k == TypeKind::UInt;
  }
  virtual bool isUnsigned() const {
    auto k = getKind();
    return k == TypeKind::UInt8 || k == TypeKind::UInt16 ||
           k == TypeKind::UInt32 || k == TypeKind::UInt64 ||
           k == TypeKind::UInt;
  }
  virtual bool isFloatingPoint() const {
    auto k = getKind();
    return k == TypeKind::Float || k == TypeKind::Float32 ||
           k == TypeKind::Float64;
  }
};

class PrimitiveType : public Type {
  TypeKind kind;

public:
  PrimitiveType(TypeKind k) : kind(k) {}
  TypeKind getKind() const override { return kind; }
  std::string toString() const override {
    switch (kind) {
    case TypeKind::Int8:
      return "i8";
    case TypeKind::Int16:
      return "i16";
    case TypeKind::Int32:
      return "i32";
    case TypeKind::Int64:
      return "i64";
    case TypeKind::UInt8:
      return "u8";
    case TypeKind::UInt16:
      return "u16";
    case TypeKind::UInt32:
      return "u32";
    case TypeKind::UInt64:
      return "u64";
    case TypeKind::Int:
      return "i32";
    case TypeKind::UInt:
      return "u32";
    case TypeKind::Float:
      return "f32";
    case TypeKind::Float32:
      return "f32";
    case TypeKind::Float64:
      return "f64";
    case TypeKind::Bool:
      return "i1";
    case TypeKind::Char:
      return "i8";
    case TypeKind::Void:
      return "void";
    default:
      return "unknown";
    }
  }
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
  struct Field {
    std::string name;
    std::shared_ptr<Type> type;
  };

  std::string name;
  std::string codegenName;
  std::vector<Field> fields;

public:
  RecordType(std::string n, std::string codegen = "")
      : name(std::move(n)),
        codegenName(codegen.empty() ? name : std::move(codegen)) {}
  TypeKind getKind() const override { return TypeKind::Record; }
  std::string toString() const override { return "%" + name; }
  bool isReferenceType() const override { return true; }

  void addField(std::string n, std::shared_ptr<Type> t) {
    fields.push_back({std::move(n), std::move(t)});
  }

  const std::vector<Field> &getFields() const { return fields; }
  const std::string &getName() const { return name; }
  const std::string &getCodegenName() const { return codegenName; }
};

class EnumType : public Type {
  std::string name;
  std::string codegenName;
  std::vector<std::string> variants;

public:
  EnumType(std::string n, std::vector<std::string> v, std::string codegen = "")
      : name(std::move(n)),
        codegenName(codegen.empty() ? name : std::move(codegen)),
        variants(std::move(v)) {}
  TypeKind getKind() const override { return TypeKind::Enum; }
  std::string toString() const override { return "enum " + name; }
  bool isReferenceType() const override { return false; }

  const std::vector<std::string> &getVariants() const { return variants; }
  const std::string &getName() const { return name; }
  const std::string &getCodegenName() const { return codegenName; }

  int getVariantIndex(const std::string &variantName) const {
    for (size_t i = 0; i < variants.size(); ++i) {
      if (variants[i] == variantName) {
        return static_cast<int>(i);
      }
    }
    return -1;
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

} // namespace zir
