#pragma once

#include <string>
#include <vector>
#include <memory>
#include <variant>
#include <optional>
#include <map>

namespace IR
{

    struct IRValue;
    struct IRInstruction;
    struct IRBasicBlock;
    struct IRFunction;

    // IR Value types
    enum class ValueType
    {
        I1,  // Boolean (1 bit)
        I8,  // Byte
        I32, // Integer
        I64,
        F32, // Float
        F64,
        PtrI8,  // Pointer to i8 (string)
        Struct, // Custom struct type
        Void,
        Unknown
    };

    // Constant value
    struct IRConstant
    {
        ValueType type;
        std::variant<int, float, double, std::string> value;
        std::string structName; // For struct constants

        IRConstant(int val) : type(ValueType::I32), value(val) {}
        IRConstant(float val) : type(ValueType::F32), value(val) {}
        IRConstant(double val) : type(ValueType::F64), value(val) {}
        IRConstant(const std::string &val) : type(ValueType::PtrI8), value(val) {}
    };

    // IR Value (variable/register)
    struct IRValue
    {
        std::string name; // e.g., "%a", "%user"
        ValueType type;
        bool isConstant;
        IRConstant constant;
        std::string structTypeName; // For struct types

        // Constructor for variable (non-constant)
        IRValue(const std::string &name_, ValueType type_)
            : name(name_), type(type_), isConstant(false), constant(0) {}

        // Constructor for constant
        IRValue(const std::string &name_, ValueType type_, const IRConstant &const_)
            : name(name_), type(type_), isConstant(true), constant(const_) {}
    };

    // IR Instruction types
    enum class IROpcode
    {
        Add,
        Sub,
        Mul,
        Div,
        Mod,
        And,
        Or,
        Xor,
        Not,
        Shl,
        Shr,
        Eq, // ==
        Ne, // !=
        Lt, // <
        Le, // <=
        Gt, // >
        Ge, // >=
        Assign,
        Call,
        Return,
        GetField,
        SetField,
        StructCreate,
        Concat,
        Load,
        Store,
        Jump,
        CondJump,
        Label
    };

    // IR Instruction
    struct IRInstruction
    {
        IROpcode opcode;
        std::shared_ptr<IRValue> result;                // Result value (if any)
        std::vector<std::shared_ptr<IRValue>> operands; // Operands
        std::string funcName;                           // For function calls
        std::string fieldName;                          // For field access
        std::string labelName;                          // For jumps/labels
        std::string structName;                         // For struct creation

        IRInstruction(IROpcode op) : opcode(op), result(nullptr) {}

        std::string toString() const;
    };

    // IR Basic Block
    struct IRBasicBlock
    {
        std::string label;
        std::vector<std::shared_ptr<IRInstruction>> instructions;

        IRBasicBlock(const std::string &label_) : label(label_) {}

        void addInstruction(std::shared_ptr<IRInstruction> instr)
        {
            instructions.push_back(instr);
        }
    };

    // IR Function
    struct IRFunction
    {
        std::string name;
        std::vector<std::shared_ptr<IRValue>> parameters;
        ValueType returnType;
        std::vector<std::shared_ptr<IRBasicBlock>> basicBlocks;
        std::map<std::string, std::shared_ptr<IRValue>> localVariables;

        IRFunction(const std::string &name_, ValueType returnType_)
            : name(name_), returnType(returnType_) {}

        void addBasicBlock(std::shared_ptr<IRBasicBlock> block)
        {
            basicBlocks.push_back(block);
        }

        void addParameter(std::shared_ptr<IRValue> param)
        {
            parameters.push_back(param);
        }

        std::string toString() const;
    };

    // IR Module (entire program)
    struct IRModule
    {
        std::vector<std::shared_ptr<IRFunction>> functions;
        std::vector<std::pair<std::string, std::shared_ptr<IRValue>>> globals;
        std::map<std::string, std::vector<std::pair<std::string, ValueType>>> structs; // struct name -> fields

        void addFunction(std::shared_ptr<IRFunction> func)
        {
            functions.push_back(func);
        }

        void addGlobal(const std::string &name, std::shared_ptr<IRValue> val)
        {
            globals.push_back({name, val});
        }

        void defineStruct(const std::string &name, const std::vector<std::pair<std::string, ValueType>> &fields)
        {
            structs[name] = fields;
        }

        std::string toString() const;
    };

    // Utility functions
    std::string valueTypeToString(ValueType type);
    ValueType stringToValueType(const std::string &str);

} // namespace IR