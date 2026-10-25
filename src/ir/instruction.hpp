#pragma once
#include "value.hpp"
#include <memory>
#include <string>
#include <vector>

namespace zir {

enum class OpCode {
    Alloca,
    Load,
    Store,
    Add,
    Sub,
    Mul,
    SDiv,
    UDiv,
    SRem,
    URem,
    Shl,
    LShr,
    AShr,
    BitAnd,
    BitOr,
    BitXor,
    Cmp,
    Br,
    CondBr,
    Ret,
    Call,
    Copy,
    Move,
    Borrow,
    Destroy,
    Alloc,
    GetElementPtr,
    Phi,
    Cast,
    WeakLock,
    WeakAlive,
    ClassIs,
    InlineAsm
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
    BinaryInst(
        OpCode o,
        std::shared_ptr<Value> res,
        std::shared_ptr<Value> l,
        std::shared_ptr<Value> r
    )
        : op(o),
          result(std::move(res)),
          lhs(std::move(l)),
          rhs(std::move(r)) {}

    OpCode getOpCode() const override { return op; }

    const std::shared_ptr<Value>& getResult() const { return result; }

    const std::shared_ptr<Value>& getLhs() const { return lhs; }

    const std::shared_ptr<Value>& getRhs() const { return rhs; }

    std::string toString() const override {
        std::string opStr;
        switch (op) {
            case OpCode::Add:
                opStr = "add";
                break;
            case OpCode::Sub:
                opStr = "sub";
                break;
            case OpCode::Mul:
                opStr = "mul";
                break;
            case OpCode::SDiv:
                opStr = "sdiv";
                break;
            case OpCode::UDiv:
                opStr = "udiv";
                break;
            case OpCode::SRem:
                opStr = "srem";
                break;
            case OpCode::URem:
                opStr = "urem";
                break;
            case OpCode::Shl:
                opStr = "shl";
                break;
            case OpCode::LShr:
                opStr = "lshr";
                break;
            case OpCode::AShr:
                opStr = "ashr";
                break;
            case OpCode::BitAnd:
                opStr = "and";
                break;
            case OpCode::BitOr:
                opStr = "or";
                break;
            case OpCode::BitXor:
                opStr = "xor";
                break;
            default:
                opStr = "binary";
                break;
        }
        return result->getName() + " = " + opStr + " " + lhs->getTypeName() + " " + lhs->getName()
            + ", " + rhs->getName();
    }
};

enum class StoreMode {
    Assign,
    Initialize,
    RawAssign,
    RawInitialize,
};

class StoreInst : public Instruction {
    std::shared_ptr<Value> src, dest;
    StoreMode mode_;

public:
    StoreInst(std::shared_ptr<Value> s, std::shared_ptr<Value> d, StoreMode mode)
        : src(std::move(s)),
          dest(std::move(d)),
          mode_(mode) {}

    OpCode getOpCode() const override { return OpCode::Store; }

    const std::shared_ptr<Value>& getSource() const { return src; }

    const std::shared_ptr<Value>& getDestination() const { return dest; }

    StoreMode getMode() const { return mode_; }

    std::string toString() const override {
        const char* modeName = "invalid";
        switch (mode_) {
            case StoreMode::Assign:
                modeName = "assign";
                break;
            case StoreMode::Initialize:
                modeName = "initialize";
                break;
            case StoreMode::RawAssign:
                modeName = "raw_assign";
                break;
            case StoreMode::RawInitialize:
                modeName = "raw_initialize";
                break;
        }
        return "store." + std::string(modeName) + " " + src->getTypeName() + " " + src->getName()
            + ", " + dest->getTypeName() + " " + dest->getName();
    }
};

class LoadInst : public Instruction {
    std::shared_ptr<Value> result, src;

public:
    LoadInst(std::shared_ptr<Value> res, std::shared_ptr<Value> s)
        : result(std::move(res)),
          src(std::move(s)) {}

    OpCode getOpCode() const override { return OpCode::Load; }

    const std::shared_ptr<Value>& getResult() const { return result; }

    const std::shared_ptr<Value>& getSource() const { return src; }

    std::string toString() const override {
        return result->getName() + " = load " + result->getTypeName() + ", " + src->getTypeName()
            + " " + src->getName();
    }
};

class AllocaInst : public Instruction {
    std::shared_ptr<Value> result;
    std::shared_ptr<Type> type;

public:
    AllocaInst(std::shared_ptr<Value> res, std::shared_ptr<Type> t)
        : result(std::move(res)),
          type(std::move(t)) {}

    OpCode getOpCode() const override { return OpCode::Alloca; }

    const std::shared_ptr<Value>& getResult() const { return result; }

    const std::shared_ptr<Type>& getAllocatedType() const { return type; }

    std::string toString() const override {
        return result->getName() + " = alloca " + type->toString();
    }
};

class BranchInst : public Instruction {
    std::string target;

public:
    BranchInst(std::string t)
        : target(std::move(t)) {}

    OpCode getOpCode() const override { return OpCode::Br; }

    const std::string& getTarget() const { return target; }

    void setTarget(std::string value) { target = std::move(value); }

    std::string toString() const override { return "br label %" + target; }
};

class CondBranchInst : public Instruction {
    std::shared_ptr<Value> cond;
    std::string trueL, falseL;

public:
    CondBranchInst(std::shared_ptr<Value> c, std::string t, std::string f)
        : cond(std::move(c)),
          trueL(std::move(t)),
          falseL(std::move(f)) {}

    OpCode getOpCode() const override { return OpCode::CondBr; }

    const std::shared_ptr<Value>& getCondition() const { return cond; }

    const std::string& getTrueLabel() const { return trueL; }

    const std::string& getFalseLabel() const { return falseL; }

    void replaceTarget(const std::string& from, std::string to) {
        if (trueL == from) {
            trueL = to;
        }
        if (falseL == from) {
            falseL = std::move(to);
        }
    }

    std::string toString() const override {
        return "br i1 " + cond->getName() + ", label %" + trueL + ", label %" + falseL;
    }
};

class CallInst : public Instruction {
private:
    std::shared_ptr<Value> result;
    std::string funcName;
    std::shared_ptr<Value> calleeValue; // non-null for indirect calls
    std::vector<std::shared_ptr<Value>> args;
    std::vector<bool> argIsRef;
    std::shared_ptr<Value> variadicPack;

public:
    CallInst(
        std::shared_ptr<Value> res,
        std::string name,
        std::vector<std::shared_ptr<Value>> arguments,
        std::vector<bool> argumentIsRef = {},
        std::shared_ptr<Value> pack = nullptr
    )
        : result(std::move(res)),
          funcName(std::move(name)),
          args(std::move(arguments)),
          argIsRef(std::move(argumentIsRef)),
          variadicPack(std::move(pack)) {}

    // Indirect call constructor
    CallInst(
        std::shared_ptr<Value> res,
        std::shared_ptr<Value> callee,
        std::vector<std::shared_ptr<Value>> arguments
    )
        : result(std::move(res)),
          calleeValue(std::move(callee)),
          args(std::move(arguments)) {}

    OpCode getOpCode() const override { return OpCode::Call; }

    const std::shared_ptr<Value>& getResult() const { return result; }

    const std::string& getFunctionName() const { return funcName; }

    const std::shared_ptr<Value>& getCalleeValue() const { return calleeValue; }

    bool isIndirect() const { return calleeValue != nullptr; }

    const std::vector<std::shared_ptr<Value>>& getArguments() const { return args; }

    const std::vector<bool>& getArgumentIsRef() const { return argIsRef; }

    const std::shared_ptr<Value>& getVariadicPack() const { return variadicPack; }

    std::string toString() const override {
        std::string s = result ? result->getName() + " = call " : "call ";
        s += calleeValue ? calleeValue->getName() : "@" + funcName;
        s += "(";
        for (size_t i = 0; i < args.size(); ++i) {
            s += args[i]->getTypeName() + " " + args[i]->getName()
                + (i < args.size() - 1 ? ", " : "");
        }
        s += ")";
        if (variadicPack) {
            s += " spread " + variadicPack->getTypeName() + " " + variadicPack->getName();
        }
        return s;
    }
};

class ReturnInst : public Instruction {
    std::shared_ptr<Value> value;

public:
    explicit ReturnInst(std::shared_ptr<Value> v = nullptr)
        : value(std::move(v)) {}

    OpCode getOpCode() const override { return OpCode::Ret; }

    const std::shared_ptr<Value>& getValue() const { return value; }

    std::string toString() const override {
        if (value)
            return "ret " + value->getTypeName() + " " + value->getName();
        return "ret void";
    }
};

class CopyInst : public Instruction {
    std::shared_ptr<Value> result;
    std::shared_ptr<Value> source;

public:
    CopyInst(std::shared_ptr<Value> res, std::shared_ptr<Value> src)
        : result(std::move(res)),
          source(std::move(src)) {}

    OpCode getOpCode() const override { return OpCode::Copy; }

    const std::shared_ptr<Value>& getResult() const { return result; }

    const std::shared_ptr<Value>& getSource() const { return source; }

    std::string toString() const override {
        return result->getName() + " = copy " + source->getTypeName() + " " + source->getName();
    }
};

class MoveInst : public Instruction {
    std::shared_ptr<Value> result;
    std::shared_ptr<Value> source;

public:
    MoveInst(std::shared_ptr<Value> res, std::shared_ptr<Value> src)
        : result(std::move(res)),
          source(std::move(src)) {}

    OpCode getOpCode() const override { return OpCode::Move; }

    const std::shared_ptr<Value>& getResult() const { return result; }

    const std::shared_ptr<Value>& getSource() const { return source; }

    std::string toString() const override {
        return result->getName() + " = move " + source->getTypeName() + " " + source->getName();
    }
};

class BorrowInst : public Instruction {
    std::shared_ptr<Value> result;
    std::shared_ptr<Value> owner;

public:
    BorrowInst(std::shared_ptr<Value> res, std::shared_ptr<Value> source)
        : result(std::move(res)),
          owner(std::move(source)) {}

    OpCode getOpCode() const override { return OpCode::Borrow; }

    const std::shared_ptr<Value>& getResult() const { return result; }

    const std::shared_ptr<Value>& getOwner() const { return owner; }

    std::string toString() const override {
        return result->getName() + " = borrow " + result->getTypeName() + " " + owner->getTypeName()
            + " " + owner->getName();
    }
};

class DestroyInst : public Instruction {
    std::shared_ptr<Value> value;

public:
    explicit DestroyInst(std::shared_ptr<Value> v)
        : value(std::move(v)) {}

    OpCode getOpCode() const override { return OpCode::Destroy; }

    const std::shared_ptr<Value>& getValue() const { return value; }

    std::string toString() const override {
        return "destroy " + value->getTypeName() + " " + value->getName();
    }
};

class AllocInst : public Instruction {
    std::shared_ptr<Value> result;
    std::shared_ptr<Type> type;

public:
    AllocInst(std::shared_ptr<Value> res, std::shared_ptr<Type> t)
        : result(std::move(res)),
          type(std::move(t)) {}

    OpCode getOpCode() const override { return OpCode::Alloc; }

    const std::shared_ptr<Value>& getResult() const { return result; }

    const std::shared_ptr<Type>& getAllocatedType() const { return type; }

    std::string toString() const override {
        return result->getName() + " = alloc " + type->toString();
    }
};

class CmpInst : public Instruction {
    std::string predicate;
    std::shared_ptr<Value> result, lhs, rhs;

public:
    CmpInst(
        std::string pred,
        std::shared_ptr<Value> res,
        std::shared_ptr<Value> l,
        std::shared_ptr<Value> r
    )
        : predicate(std::move(pred)),
          result(std::move(res)),
          lhs(std::move(l)),
          rhs(std::move(r)) {}

    OpCode getOpCode() const override { return OpCode::Cmp; }

    const std::string& getPredicate() const { return predicate; }

    const std::shared_ptr<Value>& getResult() const { return result; }

    const std::shared_ptr<Value>& getLhs() const { return lhs; }

    const std::shared_ptr<Value>& getRhs() const { return rhs; }

    std::string toString() const override {
        return result->getName() + " = icmp " + predicate + " " + lhs->getTypeName() + " "
            + lhs->getName() + ", " + rhs->getName();
    }
};

class ClassIsInst : public Instruction {
    std::shared_ptr<Value> result, object;
    std::shared_ptr<ClassType> targetType;

public:
    ClassIsInst(
        std::shared_ptr<Value> res,
        std::shared_ptr<Value> value,
        std::shared_ptr<ClassType> target
    )
        : result(std::move(res)),
          object(std::move(value)),
          targetType(std::move(target)) {}

    OpCode getOpCode() const override { return OpCode::ClassIs; }

    const std::shared_ptr<Value>& getResult() const { return result; }

    const std::shared_ptr<Value>& getObject() const { return object; }

    const std::shared_ptr<ClassType>& getTargetType() const { return targetType; }

    std::string toString() const override {
        return result->getName() + " = classis " + object->getName() + ", " + targetType->getName();
    }
};

class GetElementPtrInst : public Instruction {
    std::shared_ptr<Value> result, ptr;
    int index;
    std::shared_ptr<Value> indexValue;

public:
    GetElementPtrInst(std::shared_ptr<Value> res, std::shared_ptr<Value> p, int idx)
        : result(std::move(res)),
          ptr(std::move(p)),
          index(idx) {}

    GetElementPtrInst(
        std::shared_ptr<Value> res,
        std::shared_ptr<Value> p,
        std::shared_ptr<Value> idx
    )
        : result(std::move(res)),
          ptr(std::move(p)),
          index(0),
          indexValue(std::move(idx)) {}

    OpCode getOpCode() const override { return OpCode::GetElementPtr; }

    const std::shared_ptr<Value>& getResult() const { return result; }

    const std::shared_ptr<Value>& getPointer() const { return ptr; }

    int getIndex() const { return index; }

    const std::shared_ptr<Value>& getIndexValue() const { return indexValue; }

    std::string toString() const override {
        return result->getName() + " = getelementptr " + ptr->getTypeName() + " " + ptr->getName()
            + ", "
            + (indexValue ? indexValue->getTypeName() + " " + indexValue->getName()
                          : "i32 " + std::to_string(index));
    }
};

class PhiInst : public Instruction {
    std::shared_ptr<Value> result;
    std::vector<std::pair<std::string, std::shared_ptr<Value>>> incoming;

public:
    PhiInst(
        std::shared_ptr<Value> res,
        std::vector<std::pair<std::string, std::shared_ptr<Value>>> inc
    )
        : result(std::move(res)),
          incoming(std::move(inc)) {}

    OpCode getOpCode() const override { return OpCode::Phi; }

    const std::shared_ptr<Value>& getResult() const { return result; }

    const std::vector<std::pair<std::string, std::shared_ptr<Value>>>& getIncoming() const {
        return incoming;
    }

    void replaceIncomingLabel(const std::string& from, std::string to) {
        for (auto& [label, value] : incoming) {
            (void)value;
            if (label == from) {
                label = std::move(to);
                return;
            }
        }
    }

    std::string toString() const override {
        std::string s = result->getName() + " = phi " + result->getTypeName() + " ";
        for (size_t i = 0; i < incoming.size(); ++i) {
            std::string valName = incoming[i].second ? incoming[i].second->getName() : "undef";
            s += "[ " + valName + ", %" + incoming[i].first + " ]"
                + (i < incoming.size() - 1 ? ", " : "");
        }
        return s;
    }
};

class CastInst : public Instruction {
    std::shared_ptr<Value> result, src;
    std::shared_ptr<Type> targetType;

public:
    CastInst(std::shared_ptr<Value> res, std::shared_ptr<Value> s, std::shared_ptr<Type> t)
        : result(std::move(res)),
          src(std::move(s)),
          targetType(std::move(t)) {}

    OpCode getOpCode() const override { return OpCode::Cast; }

    const std::shared_ptr<Value>& getResult() const { return result; }

    const std::shared_ptr<Value>& getSource() const { return src; }

    const std::shared_ptr<Type>& getTargetType() const { return targetType; }

    std::string toString() const override {
        return result->getName() + " = cast " + src->getTypeName() + " " + src->getName() + " to "
            + targetType->toString();
    }
};

class WeakLockInst : public Instruction {
    std::shared_ptr<Value> result;
    std::shared_ptr<Value> weakValue;

public:
    WeakLockInst(std::shared_ptr<Value> res, std::shared_ptr<Value> value)
        : result(std::move(res)),
          weakValue(std::move(value)) {}

    OpCode getOpCode() const override { return OpCode::WeakLock; }

    const std::shared_ptr<Value>& getResult() const { return result; }

    const std::shared_ptr<Value>& getWeakValue() const { return weakValue; }

    std::string toString() const override {
        return result->getName() + " = weak.lock " + weakValue->getTypeName() + " "
            + weakValue->getName();
    }
};

class WeakAliveInst : public Instruction {
    std::shared_ptr<Value> result;
    std::shared_ptr<Value> weakValue;

public:
    WeakAliveInst(std::shared_ptr<Value> res, std::shared_ptr<Value> value)
        : result(std::move(res)),
          weakValue(std::move(value)) {}

    OpCode getOpCode() const override { return OpCode::WeakAlive; }

    const std::shared_ptr<Value>& getResult() const { return result; }

    const std::shared_ptr<Value>& getWeakValue() const { return weakValue; }

    std::string toString() const override {
        return result->getName() + " = weak.alive " + weakValue->getTypeName() + " "
            + weakValue->getName();
    }
};

struct AsmOperand {
    std::string constraint;
    std::shared_ptr<Value> value;
    std::shared_ptr<Type> valueType;
};

class InlineAsmInst : public Instruction {
    std::string assembly;
    std::vector<AsmOperand> outputs;
    std::vector<AsmOperand> inputs;
    std::vector<std::string> clobbers;

public:
    InlineAsmInst(
        std::string asmStr,
        std::vector<AsmOperand> outs,
        std::vector<AsmOperand> ins,
        std::vector<std::string> clob
    )
        : assembly(std::move(asmStr)),
          outputs(std::move(outs)),
          inputs(std::move(ins)),
          clobbers(std::move(clob)) {}

    OpCode getOpCode() const override { return OpCode::InlineAsm; }

    const std::string& getAssembly() const { return assembly; }

    const std::vector<AsmOperand>& getOutputs() const { return outputs; }

    const std::vector<AsmOperand>& getInputs() const { return inputs; }

    const std::vector<std::string>& getClobbers() const { return clobbers; }

    std::string toString() const override {
        std::string s = "asm \"" + assembly + "\"";
        auto appendOperands = [&](const char* label, const std::vector<AsmOperand>& ops) {
            if (ops.empty())
                return;
            s += std::string(" ") + label + " ";
            for (size_t i = 0; i < ops.size(); ++i) {
                s += "\"" + ops[i].constraint + "\"("
                    + (ops[i].value ? ops[i].value->getName() : "?") + ")"
                    + (i + 1 < ops.size() ? ", " : "");
            }
        };
        appendOperands("out", outputs);
        appendOperands("in", inputs);
        if (!clobbers.empty()) {
            s += " clobbers ";
            for (size_t i = 0; i < clobbers.size(); ++i) {
                s += "\"" + clobbers[i] + "\"" + (i + 1 < clobbers.size() ? ", " : "");
            }
        }
        return s;
    }
};

} // namespace zir
