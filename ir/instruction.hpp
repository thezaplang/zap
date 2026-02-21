#pragma once
#include "value.hpp"
#include <vector>
#include <memory>
#include <string>

namespace zir {

enum class OpCode {
    Alloca, Load, Store, Add, Sub, Mul, Div, Cmp,
    Br, CondBr, Ret, Call, Retain, Release, Alloc, GetElementPtr
};

class Instruction {
public:
    virtual ~Instruction() = default;
    virtual OpCode getOpCode() const = 0;
    virtual std::string toString() const = 0;
};

class BinaryInst : public Instruction {
    OpCode op;
    std::shared_ptr<Value> result, lhs, rhs;
public:
    BinaryInst(OpCode o, std::shared_ptr<Value> res, std::shared_ptr<Value> l, std::shared_ptr<Value> r)
        : op(o), result(std::move(res)), lhs(std::move(l)), rhs(std::move(r)) {}
    OpCode getOpCode() const override { return op; }
    std::string toString() const override {
        std::string opStr;
        switch (op) {
            case OpCode::Add: opStr = "add"; break;
            case OpCode::Sub: opStr = "sub"; break;
            case OpCode::Mul: opStr = "mul"; break;
            case OpCode::Div: opStr = "div"; break;
            default: opStr = "binary"; break;
        }
        return result->getName() + " = " + opStr + " " + lhs->getTypeName() + " " + lhs->getName() + ", " + rhs->getName();
    }
};

class StoreInst : public Instruction {
    std::shared_ptr<Value> src, dest;
public:
    StoreInst(std::shared_ptr<Value> s, std::shared_ptr<Value> d) : src(std::move(s)), dest(std::move(d)) {}
    OpCode getOpCode() const override { return OpCode::Store; }
    std::string toString() const override {
        return "store " + src->getTypeName() + " " + src->getName() + ", " + dest->getTypeName() + " " + dest->getName();
    }
};

class LoadInst : public Instruction {
    std::shared_ptr<Value> result, src;
public:
    LoadInst(std::shared_ptr<Value> res, std::shared_ptr<Value> s) : result(std::move(res)), src(std::move(s)) {}
    OpCode getOpCode() const override { return OpCode::Load; }
    std::string toString() const override {
        return result->getName() + " = load " + result->getTypeName() + ", " + src->getTypeName() + " " + src->getName();
    }
};

class AllocaInst : public Instruction {
    std::shared_ptr<Value> result;
    std::shared_ptr<Type> type;
public:
    AllocaInst(std::shared_ptr<Value> res, std::shared_ptr<Type> t) : result(std::move(res)), type(std::move(t)) {}
    OpCode getOpCode() const override { return OpCode::Alloca; }
    std::string toString() const override { return result->getName() + " = alloca " + type->toString(); }
};

class BranchInst : public Instruction {
    std::string target;
public:
    BranchInst(std::string t) : target(std::move(t)) {}
    OpCode getOpCode() const override { return OpCode::Br; }
    std::string toString() const override { return "br label %" + target; }
};

class CondBranchInst : public Instruction {
    std::shared_ptr<Value> cond;
    std::string trueL, falseL;
public:
    CondBranchInst(std::shared_ptr<Value> c, std::string t, std::string f)
        : cond(std::move(c)), trueL(std::move(t)), falseL(std::move(f)) {}
    OpCode getOpCode() const override { return OpCode::CondBr; }
    std::string toString() const override {
        return "br i1 " + cond->getName() + ", label %" + trueL + ", label %" + falseL;
    }
};

class CallInst : public Instruction {
    std::shared_ptr<Value> result;
    std::string funcName;
    std::vector<std::shared_ptr<Value>> args;
public:
    CallInst(std::shared_ptr<Value> res, std::string name, std::vector<std::shared_ptr<Value>> arguments)
        : result(std::move(res)), funcName(std::move(name)), args(std::move(arguments)) {}
    OpCode getOpCode() const override { return OpCode::Call; }
    std::string toString() const override {
        std::string s = (result ? result->getName() + " = " : "") + "call @" + funcName + "(";
        for (size_t i = 0; i < args.size(); ++i) {
            s += args[i]->getTypeName() + " " + args[i]->getName() + (i < args.size() - 1 ? ", " : "");
        }
        return s + ")";
    }
};

class ReturnInst : public Instruction {
    std::shared_ptr<Value> value;
public:
    ReturnInst(std::shared_ptr<Value> v = nullptr) : value(std::move(v)) {}
    OpCode getOpCode() const override { return OpCode::Ret; }
    std::string toString() const override {
        if (value) return "ret " + value->getTypeName() + " " + value->getName();
        return "ret void";
    }
};

class RetainInst : public Instruction {
    std::shared_ptr<Value> value;
public:
    RetainInst(std::shared_ptr<Value> v) : value(std::move(v)) {}
    OpCode getOpCode() const override { return OpCode::Retain; }
    std::string toString() const override { return "retain " + value->getTypeName() + " " + value->getName(); }
};

class ReleaseInst : public Instruction {
    std::shared_ptr<Value> value;
public:
    ReleaseInst(std::shared_ptr<Value> v) : value(std::move(v)) {}
    OpCode getOpCode() const override { return OpCode::Release; }
    std::string toString() const override { return "release " + value->getTypeName() + " " + value->getName(); }
};

class AllocInst : public Instruction {
    std::shared_ptr<Value> result;
    std::shared_ptr<Type> type;
public:
    AllocInst(std::shared_ptr<Value> res, std::shared_ptr<Type> t) : result(std::move(res)), type(std::move(t)) {}
    OpCode getOpCode() const override { return OpCode::Alloc; }
    std::string toString() const override { return result->getName() + " = alloc " + type->toString(); }
};

class CmpInst : public Instruction {
    std::string predicate;
    std::shared_ptr<Value> result, lhs, rhs;
public:
    CmpInst(std::string pred, std::shared_ptr<Value> res, std::shared_ptr<Value> l, std::shared_ptr<Value> r)
        : predicate(std::move(pred)), result(std::move(res)), lhs(std::move(l)), rhs(std::move(r)) {}
    OpCode getOpCode() const override { return OpCode::Cmp; }
    std::string toString() const override {
        return result->getName() + " = icmp " + predicate + " " + lhs->getTypeName() + " " + lhs->getName() + ", " + rhs->getName();
    }
};

} // namespace zir
