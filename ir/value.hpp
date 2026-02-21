#pragma once
#include "type.hpp"
#include <string>
#include <memory>

namespace zir {

enum class ValueKind { Register, Constant, Argument };

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
    Register(std::string n, std::shared_ptr<Type> t) : name(std::move(n)), type(std::move(t)) {}
    ValueKind getKind() const override { return ValueKind::Register; }
    std::string getName() const override { return "%" + name; }
    std::shared_ptr<Type> getType() const override { return type; }
};

class Constant : public Value {
    std::string value;
    std::shared_ptr<Type> type;
public:
    Constant(std::string v, std::shared_ptr<Type> t) : value(std::move(v)), type(std::move(t)) {}
    ValueKind getKind() const override { return ValueKind::Constant; }
    std::string getName() const override { return value; }
    std::shared_ptr<Type> getType() const override { return type; }
};

class Argument : public Value {
    std::string name;
    std::shared_ptr<Type> type;
public:
    Argument(std::string n, std::shared_ptr<Type> t) : name(std::move(n)), type(std::move(t)) {}
    ValueKind getKind() const override { return ValueKind::Argument; }
    std::string getName() const override { return "%" + name; }
    std::shared_ptr<Type> getType() const override { return type; }
};

} // namespace zir
