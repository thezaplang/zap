#pragma once
#include "type.hpp"
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace zir {

enum class ValueKind {
    Register,
    Constant,
    AggregateConstant,
    ArrayConstant,
    GlobalAddress,
    Argument,
    FunctionReference,
    Global
};

enum class ValueOwnership {
    Borrowed,
    OwnedStrong,
    OwnedWeak,
    Owned = OwnedStrong,
};

inline bool isOwned(ValueOwnership ownership) {
    return ownership != ValueOwnership::Borrowed;
}

inline ValueOwnership ownedForType(const std::shared_ptr<Type>& type) {
    if (type && type->getKind() == TypeKind::Class
        && std::static_pointer_cast<ClassType>(type)->isWeak()) {
        return ValueOwnership::OwnedWeak;
    }
    return ValueOwnership::OwnedStrong;
}

class Value {
    ValueOwnership ownership_ = ValueOwnership::Borrowed;

public:
    virtual ~Value() = default;
    virtual ValueKind getKind() const = 0;
    virtual std::string getName() const = 0;
    virtual std::shared_ptr<Type> getType() const = 0;

    std::string getTypeName() const { return getType()->toString(); }

    ValueOwnership getOwnership() const { return ownership_; }

    void setOwnership(ValueOwnership ownership) { ownership_ = ownership; }
};

class GlobalAddress : public Value {
    std::string linkName;
    std::shared_ptr<Type> type;
    std::optional<size_t> arrayIndex;

public:
    GlobalAddress(
        std::string ln,
        std::shared_ptr<Type> t,
        std::optional<size_t> index = std::nullopt
    )
        : linkName(std::move(ln)),
          type(std::move(t)),
          arrayIndex(index) {}

    ValueKind getKind() const override { return ValueKind::GlobalAddress; }

    std::string getName() const override { return "@" + linkName; }

    std::shared_ptr<Type> getType() const override { return type; }

    const std::string& getLinkName() const { return linkName; }

    const std::optional<size_t>& getArrayIndex() const { return arrayIndex; }
};

class Register : public Value {
    std::string name;
    std::shared_ptr<Type> type;

public:
    Register(std::string n, std::shared_ptr<Type> t)
        : name(std::move(n)),
          type(std::move(t)) {}

    ValueKind getKind() const override { return ValueKind::Register; }

    std::string getName() const override { return "%" + name; }

    std::shared_ptr<Type> getType() const override { return type; }

    const std::string& getRawName() const { return name; }
};

class Constant : public Value {
    std::string value;
    std::shared_ptr<Type> type;

public:
    Constant(std::string v, std::shared_ptr<Type> t)
        : value(std::move(v)),
          type(std::move(t)) {}

    ValueKind getKind() const override { return ValueKind::Constant; }

    std::string getName() const override { return value; }

    std::shared_ptr<Type> getType() const override { return type; }

    const std::string& getLiteral() const { return value; }
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
        : type(std::move(t)),
          fields(std::move(f)) {}

    ValueKind getKind() const override { return ValueKind::AggregateConstant; }

    std::string getName() const override { return "<aggregate>"; }

    std::shared_ptr<Type> getType() const override { return type; }

    const std::vector<FieldValue>& getFields() const { return fields; }
};

class ArrayConstant : public Value {
    std::shared_ptr<Type> type;
    std::vector<std::shared_ptr<Value>> elements;

public:
    ArrayConstant(std::shared_ptr<Type> t, std::vector<std::shared_ptr<Value>> e)
        : type(std::move(t)),
          elements(std::move(e)) {}

    ValueKind getKind() const override { return ValueKind::ArrayConstant; }

    std::string getName() const override { return "<array>"; }

    std::shared_ptr<Type> getType() const override { return type; }

    const std::vector<std::shared_ptr<Value>>& getElements() const { return elements; }
};

class Argument : public Value {
    std::string name;
    std::shared_ptr<Type> type;
    bool isRef_;
    bool isVariadicPack_;
    std::shared_ptr<Type> variadicElementType_;
    ParameterOwnership parameterOwnership_;
    ParameterEscape parameterEscape_;

public:
    Argument(
        std::string n,
        std::shared_ptr<Type> t,
        bool isRef = false,
        bool isVariadicPack = false,
        std::shared_ptr<Type> variadicElementType = nullptr,
        ParameterOwnership parameterOwnership = ParameterOwnership::Borrow,
        ParameterEscape parameterEscape = ParameterEscape::Unspecified
    )
        : name(std::move(n)),
          type(std::move(t)),
          isRef_(isRef),
          isVariadicPack_(isVariadicPack),
          variadicElementType_(std::move(variadicElementType)),
          parameterOwnership_(parameterOwnership),
          parameterEscape_(parameterEscape) {}

    ValueKind getKind() const override { return ValueKind::Argument; }

    std::string getName() const override { return "%" + name; }

    std::shared_ptr<Type> getType() const override { return type; }

    const std::string& getRawName() const { return name; }

    bool isRef() const { return isRef_; }

    bool isVariadicPack() const { return isVariadicPack_; }

    const std::shared_ptr<Type>& getVariadicElementType() const { return variadicElementType_; }

    ParameterOwnership getParameterOwnership() const { return parameterOwnership_; }

    ParameterEscape getParameterEscape() const { return parameterEscape_; }
};

class FunctionReference : public Value {
    std::string linkName;
    std::shared_ptr<FunctionPointerType> type;

public:
    FunctionReference(std::string name, std::shared_ptr<FunctionPointerType> type)
        : linkName(std::move(name)),
          type(std::move(type)) {}

    ValueKind getKind() const override { return ValueKind::FunctionReference; }

    std::string getName() const override { return "@" + linkName; }

    std::shared_ptr<Type> getType() const override { return type; }

    const std::string& getLinkName() const { return linkName; }
};

class Global : public Value {
    std::string name;
    std::string linkName;
    std::shared_ptr<Type> type;
    std::shared_ptr<Value> initializer;
    bool isConst;

public:
    Global(
        std::string n,
        std::string ln,
        std::shared_ptr<Type> t,
        std::shared_ptr<Value> init = nullptr,
        bool isConstant = false
    )
        : name(std::move(n)),
          linkName(std::move(ln)),
          type(std::move(t)),
          initializer(std::move(init)),
          isConst(isConstant) {}

    ValueKind getKind() const override { return ValueKind::Global; }

    std::string getName() const override { return "@" + linkName; }

    std::shared_ptr<Type> getType() const override { return std::make_shared<PointerType>(type); }

    const std::string& getRawName() const { return name; }

    const std::string& getLinkName() const { return linkName; }

    const std::shared_ptr<Type>& getValueType() const { return type; }

    const std::shared_ptr<Value>& getInitializer() const { return initializer; }

    bool isConstant() const { return isConst; }
};

} // namespace zir
