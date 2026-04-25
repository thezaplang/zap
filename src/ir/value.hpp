#pragma once
#include "type.hpp"
#include <memory>
#include <string>
#include <vector>

namespace zir {

enum class ValueKind { Register, Constant, AggregateConstant, Argument, Global };

class Value {
public:
  virtual ~Value() = default;
  virtual ValueKind getKind() const = 0;
  virtual std::string getName() const = 0;
  virtual std::shared_ptr<Type> getType() const = 0;
  std::string getTypeName() const { return getType()->toString(); }
};

class Register : public Value {
  std::string name;
  std::shared_ptr<Type> type;

public:
  Register(std::string n, std::shared_ptr<Type> t)
      : name(std::move(n)), type(std::move(t)) {}
  ValueKind getKind() const override { return ValueKind::Register; }
  std::string getName() const override { return "%" + name; }
  std::shared_ptr<Type> getType() const override { return type; }
  const std::string &getRawName() const { return name; }
};

class Constant : public Value {
  std::string value;
  std::shared_ptr<Type> type;

public:
  Constant(std::string v, std::shared_ptr<Type> t)
      : value(std::move(v)), type(std::move(t)) {}
  ValueKind getKind() const override { return ValueKind::Constant; }
  std::string getName() const override { return value; }
  std::shared_ptr<Type> getType() const override { return type; }
  const std::string &getLiteral() const { return value; }
};

class AggregateConstant : public Value {
public:
  struct FieldValue {
    std::string name;
    std::shared_ptr<Value> value;
  };

private:
  std::shared_ptr<Type> type;
  std::vector<FieldValue> fields;

public:
  AggregateConstant(std::shared_ptr<Type> t, std::vector<FieldValue> f)
      : type(std::move(t)), fields(std::move(f)) {}

  ValueKind getKind() const override { return ValueKind::AggregateConstant; }
  std::string getName() const override { return "<aggregate>"; }
  std::shared_ptr<Type> getType() const override { return type; }

  const std::vector<FieldValue> &getFields() const { return fields; }
};

class Argument : public Value {
  std::string name;
  std::shared_ptr<Type> type;
  bool isRef_;
  bool isVariadicPack_;
  std::shared_ptr<Type> variadicElementType_;

public:
  Argument(std::string n, std::shared_ptr<Type> t, bool isRef = false,
           bool isVariadicPack = false,
           std::shared_ptr<Type> variadicElementType = nullptr)
      : name(std::move(n)), type(std::move(t)), isRef_(isRef),
        isVariadicPack_(isVariadicPack),
        variadicElementType_(std::move(variadicElementType)) {}
  ValueKind getKind() const override { return ValueKind::Argument; }
  std::string getName() const override { return "%" + name; }
  std::shared_ptr<Type> getType() const override { return type; }
  const std::string &getRawName() const { return name; }
  bool isRef() const { return isRef_; }
  bool isVariadicPack() const { return isVariadicPack_; }
  const std::shared_ptr<Type> &getVariadicElementType() const {
    return variadicElementType_;
  }
};

class Global : public Value {
  std::string name;
  std::string linkName;
  std::shared_ptr<Type> type;
  std::shared_ptr<Value> initializer;
  bool isConst;

public:
  Global(std::string n, std::string ln, std::shared_ptr<Type> t,
         std::shared_ptr<Value> init = nullptr, bool isConstant = false)
      : name(std::move(n)), linkName(std::move(ln)), type(std::move(t)),
        initializer(std::move(init)), isConst(isConstant) {}
  ValueKind getKind() const override { return ValueKind::Global; }
  std::string getName() const override { return "@" + linkName; }
  std::shared_ptr<Type> getType() const override {
    return std::make_shared<PointerType>(type);
  }
  const std::string &getRawName() const { return name; }
  const std::string &getLinkName() const { return linkName; }
  const std::shared_ptr<Type> &getValueType() const { return type; }
  const std::shared_ptr<Value> &getInitializer() const { return initializer; }
  bool isConstant() const { return isConst; }
};

} // namespace zir
