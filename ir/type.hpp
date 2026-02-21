#pragma once
#include <memory>
#include <string>
#include <vector>

namespace zir {

enum class TypeKind { Void, Int, Float, Bool, Pointer, Record, Array };

class Type {
public:
  virtual ~Type() = default;
  virtual TypeKind getKind() const = 0;
  virtual std::string toString() const = 0;
  virtual bool isReferenceType() const { return false; }
};

class PrimitiveType : public Type {
  TypeKind kind;

public:
  PrimitiveType(TypeKind k) : kind(k) {}
  TypeKind getKind() const override { return kind; }
  std::string toString() const override {
    switch (kind) {
    case TypeKind::Int:
      return "i64";
    case TypeKind::Float:
      return "f64";
    case TypeKind::Bool:
      return "i1";
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
  std::string name;

public:
  RecordType(std::string n) : name(std::move(n)) {}
  TypeKind getKind() const override { return TypeKind::Record; }
  std::string toString() const override { return "%" + name; }
  bool isReferenceType() const override { return true; }
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
