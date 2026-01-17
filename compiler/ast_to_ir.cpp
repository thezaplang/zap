#include "ast_to_ir.hpp"
#include "../ast/const/const_int.hpp"
#include "../ast/const/const_float.hpp"
#include "../ast/const/const_string.hpp"
#include "../ast/const/const_id.hpp"
#include "../ast/bin_expr.hpp"
#include "../ast/unary_expr.hpp"
#include "../ast/fun_call.hpp"
#include "../ast/var_decl.hpp"
#include "../ast/assign_node.hpp"
#include "../ast/return_node.hpp"
#include "../ast/if_node.hpp"
#include "../ast/while_node.hpp"
#include "../ast/body_node.hpp"
#include <sstream>
#include <stdexcept>

namespace zap::compiler
{

    ASTToIRConverter::ASTToIRConverter()
        : tempVarCounter_(0), labelCounter_(0)
    {
        module_ = std::make_shared<IR::IRModule>();
    }

    std::shared_ptr<IR::IRModule> ASTToIRConverter::convert(RootNode *root)
    {
        if (!root)
        {
            throw std::runtime_error("Cannot convert null root node");
        }

        // Convert all top-level declarations
        for (auto &child : root->children)
        {
            // Try to cast to FunDecl
            if (auto funDecl = dynamic_cast<FunDecl *>(child.get()))
            {
                convertFunctionDecl(funDecl);
            }
        }

        return module_;
    }

    std::string ASTToIRConverter::generateTempName()
    {
        std::ostringstream ss;
        ss << "%tmp" << tempVarCounter_++;
        return ss.str();
    }

    std::string ASTToIRConverter::generateLabel()
    {
        std::ostringstream ss;
        ss << "label_" << labelCounter_++;
        return ss.str();
    }

    IR::ValueType ASTToIRConverter::astTypeToIRType(TypeNode *typeNode)
    {
        if (!typeNode)
            return IR::ValueType::Void;

        const std::string &typeName = typeNode->typeName;

        if (typeName == "int" || typeName == "i32")
        {
            return IR::ValueType::I32;
        }
        else if (typeName == "float" || typeName == "f32")
        {
            return IR::ValueType::F32;
        }
        else if (typeName == "double" || typeName == "f64")
        {
            return IR::ValueType::F64;
        }
        else if (typeName == "bool" || typeName == "i1")
        {
            return IR::ValueType::I1;
        }
        else if (typeName == "str" || typeName == "string")
        {
            return IR::ValueType::PtrI8;
        }
        else if (typeName == "void")
        {
            return IR::ValueType::Void;
        }
        else
        {

            return IR::ValueType::Struct;
        }
    }

    void ASTToIRConverter::convertFunctionDecl(FunDecl *funDecl)
    {
        if (!funDecl)
            return;

        // Create IR function
        IR::ValueType returnType = astTypeToIRType(funDecl->returnType_.get());
        currentFunc_ = std::make_shared<IR::IRFunction>(funDecl->name_, returnType);

        // Add parameters
        for (auto &param : funDecl->params_)
        {
            if (param)
            {
                IR::ValueType paramType = astTypeToIRType(param->type.get());
                auto irParam = std::make_shared<IR::IRValue>(
                    "%" + param->name,
                    paramType);
                currentFunc_->addParameter(irParam);
                symbolTable_[param->name] = irParam;
            }
        }

        currentBlock_ = std::make_shared<IR::IRBasicBlock>("entry");
        currentFunc_->addBasicBlock(currentBlock_);

        // Convert function body
        if (funDecl->body_)
        {
            convertBody(funDecl->body_.get());
        }
        else if (funDecl->lambdaExpr_)
        {
            // Lambda expression - treat as return
            auto result = convertExpression(funDecl->lambdaExpr_.get());
            auto returnInstr = std::make_shared<IR::IRInstruction>(IR::IROpcode::Return);
            if (result)
            {
                returnInstr->operands.push_back(result);
            }
            emitInstruction(returnInstr);
        }

        // Add implicit return if function returns void and has no explicit return
        if (returnType == IR::ValueType::Void &&
            (currentBlock_->instructions.empty() ||
             currentBlock_->instructions.back()->opcode != IR::IROpcode::Return))
        {
            auto returnInstr = std::make_shared<IR::IRInstruction>(IR::IROpcode::Return);
            emitInstruction(returnInstr);
        }

        module_->addFunction(currentFunc_);
        currentFunc_ = nullptr;
        currentBlock_ = nullptr;
        symbolTable_.clear();
    }

    std::shared_ptr<IR::IRValue> ASTToIRConverter::convertExpression(ExpressionNode *expr)
    {
        if (!expr)
            return nullptr;

        // Try to cast to specific expression types
        if (auto binExpr = dynamic_cast<BinExpr *>(expr))
        {
            return convertBinaryExpr(binExpr);
        }
        if (auto unaryExpr = dynamic_cast<UnaryExpr *>(expr))
        {
            return convertUnaryExpr(unaryExpr);
        }
        if (auto funCall = dynamic_cast<FunCall *>(expr))
        {
            return convertFunctionCall(funCall);
        }
        if (auto constInt = dynamic_cast<ConstInt *>(expr))
        {
            return convertConstInt(constInt);
        }
        if (auto constFloat = dynamic_cast<ConstFloat *>(expr))
        {
            return convertConstFloat(constFloat);
        }
        if (auto constString = dynamic_cast<ConstString *>(expr))
        {
            return convertConstString(constString);
        }
        if (auto constId = dynamic_cast<ConstId *>(expr))
        {
            return convertConstId(constId);
        }

        return nullptr;
    }

    void ASTToIRConverter::convertStatement(StatementNode *stmt)
    {
        if (!stmt)
            return;

        if (auto varDecl = dynamic_cast<VarDecl *>(stmt))
        {
            convertVariableDecl(varDecl);
        }
        else if (auto assignNode = dynamic_cast<AssignNode *>(stmt))
        {
            convertAssignment(assignNode);
        }
        else if (auto returnNode = dynamic_cast<ReturnNode *>(stmt))
        {
            convertReturnNode(returnNode);
        }
        else if (auto ifNode = dynamic_cast<IfNode *>(stmt))
        {
            convertIfNode(ifNode);
        }
        else if (auto whileNode = dynamic_cast<WhileNode *>(stmt))
        {
            convertWhileNode(whileNode);
        }
        else if (auto exprStmt = dynamic_cast<ExpressionNode *>(stmt))
        {
            convertExpression(exprStmt);
        }
    }

    std::shared_ptr<IR::IRValue> ASTToIRConverter::convertBinaryExpr(BinExpr *binExpr)
    {
        if (!binExpr || !binExpr->left_ || !binExpr->right_)
            return nullptr;

        auto left = convertExpression(binExpr->left_.get());
        auto right = convertExpression(binExpr->right_.get());

        if (!left || !right)
            return nullptr;

        IR::IROpcode opcode = getBinaryOpcode(binExpr->op_);
        auto instr = std::make_shared<IR::IRInstruction>(opcode);
        instr->operands.push_back(left);
        instr->operands.push_back(right);

        // Determine result type
        auto result = std::make_shared<IR::IRValue>(generateTempName(), left->type);
        instr->result = result;
        emitInstruction(instr);

        return result;
    }

    std::shared_ptr<IR::IRValue> ASTToIRConverter::convertUnaryExpr(UnaryExpr *unaryExpr)
    {
        if (!unaryExpr || !unaryExpr->expr_)
            return nullptr;

        auto operand = convertExpression(unaryExpr->expr_.get());
        if (!operand)
            return nullptr;

        IR::IROpcode opcode = getUnaryOpcode(unaryExpr->op_);
        auto instr = std::make_shared<IR::IRInstruction>(opcode);
        instr->operands.push_back(operand);

        auto result = std::make_shared<IR::IRValue>(generateTempName(), operand->type);
        instr->result = result;
        emitInstruction(instr);

        return result;
    }

    std::shared_ptr<IR::IRValue> ASTToIRConverter::convertFunctionCall(FunCall *funCall)
    {
        if (!funCall)
            return nullptr;

        auto callInstr = std::make_shared<IR::IRInstruction>(IR::IROpcode::Call);
        callInstr->funcName = funCall->funcName_;

        for (auto &param : funCall->params_)
        {
            auto arg = convertExpression(param.get());
            if (arg)
            {
                callInstr->operands.push_back(arg);
            }
        }

        auto result = std::make_shared<IR::IRValue>(generateTempName(), IR::ValueType::I32);
        callInstr->result = result;
        emitInstruction(callInstr);

        return result;
    }

    std::shared_ptr<IR::IRValue> ASTToIRConverter::convertConstInt(ConstInt *constInt)
    {
        if (!constInt)
            return nullptr;

        auto value = std::make_shared<IR::IRValue>(
            generateTempName(),
            IR::ValueType::I32,
            IR::IRConstant(constInt->value_));
        return value;
    }

    std::shared_ptr<IR::IRValue> ASTToIRConverter::convertConstFloat(ConstFloat *constFloat)
    {
        if (!constFloat)
            return nullptr;

        auto value = std::make_shared<IR::IRValue>(
            generateTempName(),
            IR::ValueType::F32,
            IR::IRConstant(constFloat->value_));
        return value;
    }

    std::shared_ptr<IR::IRValue> ASTToIRConverter::convertConstString(ConstString *constString)
    {
        if (!constString)
            return nullptr;

        auto value = std::make_shared<IR::IRValue>(
            generateTempName(),
            IR::ValueType::PtrI8,
            IR::IRConstant(constString->value_));
        return value;
    }

    std::shared_ptr<IR::IRValue> ASTToIRConverter::convertConstId(ConstId *constId)
    {
        if (!constId)
            return nullptr;

        auto it = symbolTable_.find(constId->value_);
        if (it != symbolTable_.end())
        {
            return it->second;
        }

        auto value = std::make_shared<IR::IRValue>(
            "%" + constId->value_,
            IR::ValueType::Unknown);
        return value;
    }

    void ASTToIRConverter::convertVariableDecl(VarDecl *varDecl)
    {
        if (!varDecl)
            return;

        IR::ValueType varType = astTypeToIRType(varDecl->type_.get());

        std::shared_ptr<IR::IRValue> initValue;
        if (varDecl->initializer_)
        {
            initValue = convertExpression(varDecl->initializer_.get());
        }

        auto varName = "%" + varDecl->name_;
        auto irValue = std::make_shared<IR::IRValue>(varName, varType);

        symbolTable_[varDecl->name_] = irValue;

        if (initValue)
        {
            auto assignInstr = std::make_shared<IR::IRInstruction>(IR::IROpcode::Assign);
            assignInstr->result = irValue;
            assignInstr->operands.push_back(initValue);
            emitInstruction(assignInstr);
        }
    }

    void ASTToIRConverter::convertAssignment(AssignNode *assignNode)
    {
        if (!assignNode)
            return;

        auto it = symbolTable_.find(assignNode->target_);
        if (it == symbolTable_.end())
        {
            throw std::runtime_error("Undefined variable: " + assignNode->target_);
        }

        auto varValue = it->second;

        // Convert RHS expression
        auto rhsValue = convertExpression(assignNode->expr_.get());
        if (!rhsValue)
            return;

        // Emit assignment
        auto assignInstr = std::make_shared<IR::IRInstruction>(IR::IROpcode::Assign);
        assignInstr->result = varValue;
        assignInstr->operands.push_back(rhsValue);
        emitInstruction(assignInstr);
    }

    void ASTToIRConverter::convertReturnNode(ReturnNode *returnNode)
    {
        if (!returnNode)
            return;

        auto returnInstr = std::make_shared<IR::IRInstruction>(IR::IROpcode::Return);

        if (returnNode->returnValue)
        {
            auto value = convertExpression(returnNode->returnValue.get());
            if (value)
            {
                returnInstr->operands.push_back(value);
            }
        }

        emitInstruction(returnInstr);
    }

    void ASTToIRConverter::convertIfNode(IfNode *ifNode)
    {
        if (!ifNode)
            return;

        auto condValue = convertExpression(ifNode->condition_.get());
        if (!condValue)
            return;

        // Generate labels
        std::string thenLabel = generateLabel();
        std::string elseLabel = ifNode->elseBody_ ? generateLabel() : "";
        std::string mergeLabel = generateLabel();

        // Emit conditional jump to then block
        auto condJump = std::make_shared<IR::IRInstruction>(IR::IROpcode::CondJump);
        condJump->operands.push_back(condValue);
        condJump->labelName = thenLabel;
        emitInstruction(condJump);

        if (!ifNode->elseBody_)
        {
            auto skipJump = std::make_shared<IR::IRInstruction>(IR::IROpcode::Jump);
            skipJump->labelName = mergeLabel;
            emitInstruction(skipJump);
        }
        else
        {

            auto elseJump = std::make_shared<IR::IRInstruction>(IR::IROpcode::Jump);
            elseJump->labelName = elseLabel;
            emitInstruction(elseJump);

            // Create else block
            currentBlock_ = std::make_shared<IR::IRBasicBlock>(elseLabel);
            currentFunc_->addBasicBlock(currentBlock_);
            convertBody(ifNode->elseBody_.get());

            // Jump to merge after else
            auto endElseJump = std::make_shared<IR::IRInstruction>(IR::IROpcode::Jump);
            endElseJump->labelName = mergeLabel;
            emitInstruction(endElseJump);
        }

        // Create then block
        currentBlock_ = std::make_shared<IR::IRBasicBlock>(thenLabel);
        currentFunc_->addBasicBlock(currentBlock_);

        if (ifNode->thenBody_)
        {
            convertBody(ifNode->thenBody_.get());
        }

        auto thenJump = std::make_shared<IR::IRInstruction>(IR::IROpcode::Jump);
        thenJump->labelName = mergeLabel;
        emitInstruction(thenJump);

        currentBlock_ = std::make_shared<IR::IRBasicBlock>(mergeLabel);
        currentFunc_->addBasicBlock(currentBlock_);
    }

    void ASTToIRConverter::convertWhileNode(WhileNode *whileNode)
    {
        if (!whileNode)
            return;

        // Generate labels
        std::string loopLabel = generateLabel();
        std::string bodyLabel = generateLabel();
        std::string endLabel = generateLabel();

        // Jump to loop condition
        auto loopStart = std::make_shared<IR::IRInstruction>(IR::IROpcode::Jump);
        loopStart->labelName = loopLabel;
        emitInstruction(loopStart);

        // Create loop condition block
        currentBlock_ = std::make_shared<IR::IRBasicBlock>(loopLabel);
        currentFunc_->addBasicBlock(currentBlock_);

        auto condValue = convertExpression(whileNode->condition_.get());

        // Conditional jump to body
        auto condJump = std::make_shared<IR::IRInstruction>(IR::IROpcode::CondJump);
        if (condValue)
        {
            condJump->operands.push_back(condValue);
        }
        condJump->labelName = bodyLabel;
        emitInstruction(condJump);

        auto endJump = std::make_shared<IR::IRInstruction>(IR::IROpcode::Jump);
        endJump->labelName = endLabel;
        emitInstruction(endJump);

        currentBlock_ = std::make_shared<IR::IRBasicBlock>(bodyLabel);
        currentFunc_->addBasicBlock(currentBlock_);

        if (whileNode->body_)
        {
            convertBody(whileNode->body_.get());
        }

        auto loopJump = std::make_shared<IR::IRInstruction>(IR::IROpcode::Jump);
        loopJump->labelName = loopLabel;
        emitInstruction(loopJump);

        currentBlock_ = std::make_shared<IR::IRBasicBlock>(endLabel);
        currentFunc_->addBasicBlock(currentBlock_);
    }

    void ASTToIRConverter::convertBody(BodyNode *body)
    {
        if (!body)
            return;

        for (auto &stmt : body->statements)
        {
            if (auto statementNode = dynamic_cast<StatementNode *>(stmt.get()))
            {
                convertStatement(statementNode);
            }
        }
    }
    IR::IROpcode ASTToIRConverter::getBinaryOpcode(const std::string &op)
    {
        if (op == "+")
            return IR::IROpcode::Add;
        if (op == "-")
            return IR::IROpcode::Sub;
        if (op == "*")
            return IR::IROpcode::Mul;
        if (op == "/")
            return IR::IROpcode::Div;
        if (op == "%")
            return IR::IROpcode::Mod;
        if (op == "&")
            return IR::IROpcode::And;
        if (op == "|")
            return IR::IROpcode::Or;
        if (op == "^")
            return IR::IROpcode::Xor;
        if (op == "<<")
            return IR::IROpcode::Shl;
        if (op == ">>")
            return IR::IROpcode::Shr;
        if (op == "==")
            return IR::IROpcode::Eq;
        if (op == "!=")
            return IR::IROpcode::Ne;
        if (op == "<")
            return IR::IROpcode::Lt;
        if (op == "<=")
            return IR::IROpcode::Le;
        if (op == ">")
            return IR::IROpcode::Gt;
        if (op == ">=")
            return IR::IROpcode::Ge;

        throw std::runtime_error("Unknown binary operator: " + op);
    }

    IR::IROpcode ASTToIRConverter::getUnaryOpcode(const std::string &op)
    {
        if (op == "!")
            return IR::IROpcode::Not;
        if (op == "-")
            return IR::IROpcode::Sub;

        throw std::runtime_error("Unknown unary operator: " + op);
    }

    void ASTToIRConverter::emitInstruction(std::shared_ptr<IR::IRInstruction> instr)
    {
        if (!currentBlock_)
        {
            throw std::runtime_error("No current basic block to emit instruction");
        }
        currentBlock_->addInstruction(instr);
    }

} // namespace zap::compiler
