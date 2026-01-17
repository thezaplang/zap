#include "ir.hpp"
#include <sstream>

namespace IR
{

    // Convert ValueType to string
    std::string valueTypeToString(ValueType type)
    {
        switch (type)
        {
        case ValueType::I1:
            return "i1";
        case ValueType::I8:
            return "i8";
        case ValueType::I32:
            return "int";
        case ValueType::I64:
            return "i64";
        case ValueType::F32:
            return "f32";
        case ValueType::F64:
            return "float";
        case ValueType::PtrI8:
            return "ptr i8";
        case ValueType::Void:
            return "void";
        case ValueType::Struct:
            return "struct";
        case ValueType::Unknown:
            return "unknown";
        default:
            return "unknown";
        }
    }

    // Convert string to ValueType
    ValueType stringToValueType(const std::string &str)
    {
        if (str == "i1" || str == "bool")
            return ValueType::I1;
        if (str == "i8" || str == "byte")
            return ValueType::I8;
        if (str == "i32" || str == "int")
            return ValueType::I32;
        if (str == "i64")
            return ValueType::I64;
        if (str == "f32" || str == "float")
            return ValueType::F32;
        if (str == "f64" || str == "double")
            return ValueType::F64;
        if (str == "ptr i8" || str == "str")
            return ValueType::PtrI8;
        if (str == "void")
            return ValueType::Void;

        return ValueType::Unknown;
    }

    std::string IRInstruction::toString() const
    {
        std::ostringstream ss;

        if (result)
        {
            ss << result->name << ":" << valueTypeToString(result->type) << " = ";
        }

        switch (opcode)
        {
        case IROpcode::Add:
            ss << "add";
            break;
        case IROpcode::Sub:
            ss << "sub";
            break;
        case IROpcode::Mul:
            ss << "mul";
            break;
        case IROpcode::Div:
            ss << "div";
            break;
        case IROpcode::Mod:
            ss << "mod";
            break;
        case IROpcode::And:
            ss << "and";
            break;
        case IROpcode::Or:
            ss << "or";
            break;
        case IROpcode::Xor:
            ss << "xor";
            break;
        case IROpcode::Not:
            ss << "not";
            break;
        case IROpcode::Shl:
            ss << "shl";
            break;
        case IROpcode::Shr:
            ss << "shr";
            break;
        case IROpcode::Eq:
            ss << "eq";
            break;
        case IROpcode::Ne:
            ss << "ne";
            break;
        case IROpcode::Lt:
            ss << "lt";
            break;
        case IROpcode::Le:
            ss << "le";
            break;
        case IROpcode::Gt:
            ss << "gt";
            break;
        case IROpcode::Ge:
            ss << "ge";
            break;
        case IROpcode::Assign:
            ss << "assign";
            break;
        case IROpcode::Call:
            ss << "call " << funcName;
            break;
        case IROpcode::Return:
            ss << "return";
            break;
        case IROpcode::GetField:
            ss << "getfield " << fieldName;
            break;
        case IROpcode::SetField:
            ss << "setfield " << fieldName;
            break;
        case IROpcode::StructCreate:
            ss << "struct " << structName;
            break;
        case IROpcode::Concat:
            ss << "concat";
            break;
        case IROpcode::Load:
            ss << "load";
            break;
        case IROpcode::Store:
            ss << "store";
            break;
        case IROpcode::Jump:
            ss << "jump " << labelName;
            break;
        case IROpcode::CondJump:
            ss << "cond_jump " << labelName;
            break;
        case IROpcode::Label:
            ss << labelName << ":";
            break;
        default:
            ss << "unknown";
        }

        for (size_t i = 0; i < operands.size(); ++i)
        {
            if (operands[i]->isConstant)
            {

                ss << " ";
                if (std::holds_alternative<int>(operands[i]->constant.value))
                {
                    ss << std::get<int>(operands[i]->constant.value);
                }
                else if (std::holds_alternative<float>(operands[i]->constant.value))
                {
                    ss << std::get<float>(operands[i]->constant.value);
                }
                else if (std::holds_alternative<double>(operands[i]->constant.value))
                {
                    ss << std::get<double>(operands[i]->constant.value);
                }
                else if (std::holds_alternative<std::string>(operands[i]->constant.value))
                {
                    ss << "\"" << std::get<std::string>(operands[i]->constant.value) << "\"";
                }
            }
            else
            {
                ss << " " << operands[i]->name;
            }
        }

        return ss.str();
    }

    std::string IRFunction::toString() const
    {
        std::ostringstream ss;

        ss << "func " << name << "(";
        for (size_t i = 0; i < parameters.size(); ++i)
        {
            ss << parameters[i]->name << ":" << valueTypeToString(parameters[i]->type);
            if (i < parameters.size() - 1)
                ss << ", ";
        }
        ss << ") -> " << valueTypeToString(returnType) << ":\n";

        for (const auto &block : basicBlocks)
        {
            ss << "  " << block->label << ":\n";
            for (const auto &instr : block->instructions)
            {
                ss << "    " << instr->toString() << "\n";
            }
        }

        return ss.str();
    }

    std::string IRModule::toString() const
    {
        std::ostringstream ss;

        // Output structs
        for (const auto &[structName, fields] : structs)
        {
            ss << "struct " << structName << " {\n";
            for (const auto &[fieldName, fieldType] : fields)
            {
                ss << "  " << fieldName << ": " << valueTypeToString(fieldType) << "\n";
            }
            ss << "}\n\n";
        }

        // Output globals
        for (const auto &[name, value] : globals)
        {
            ss << "global " << name << ": " << valueTypeToString(value->type);
            if (value->isConstant)
            {
                ss << " = ";
                // Output constant value
                if (std::holds_alternative<int>(value->constant.value))
                {
                    ss << std::get<int>(value->constant.value);
                }
                else if (std::holds_alternative<float>(value->constant.value))
                {
                    ss << std::get<float>(value->constant.value);
                }
                else if (std::holds_alternative<double>(value->constant.value))
                {
                    ss << std::get<double>(value->constant.value);
                }
                else if (std::holds_alternative<std::string>(value->constant.value))
                {
                    ss << "\"" << std::get<std::string>(value->constant.value) << "\"";
                }
            }
            ss << "\n";
        }

        if (!globals.empty())
            ss << "\n";

        // Output functions
        for (const auto &func : functions)
        {
            ss << func->toString() << "\n";
        }

        return ss.str();
    }

} // namespace IR
