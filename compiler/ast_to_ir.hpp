#pragma once

#include "../ir/ir.hpp"
#include "../ast/nodes.hpp"
#include <memory>
#include <unordered_map>
#include <string>
#include <vector>

namespace zap::compiler
{

    class ASTToIRConverter
    {
    public:
        ASTToIRConverter();

        std::shared_ptr<IR::IRModule> convert(RootNode *root);

    private:
        std::shared_ptr<IR::IRModule> module_;
        std::shared_ptr<IR::IRFunction> currentFunc_;
        std::shared_ptr<IR::IRBasicBlock> currentBlock_;
        std::unordered_map<std::string, std::shared_ptr<IR::IRValue>> symbolTable_;
        int tempVarCounter_;
        int labelCounter_;

        // Helper methods
        std::string generateTempName();
        std::string generateLabel();
        IR::ValueType astTypeToIRType(TypeNode *typeNode);

        // Conversion methods for different AST nodes
        void convertFunctionDecl(FunDecl *funDecl);
        std::shared_ptr<IR::IRValue> convertExpression(ExpressionNode *expr);
        void convertStatement(StatementNode *stmt);

        // Expression conversion
        std::shared_ptr<IR::IRValue> convertBinaryExpr(BinExpr *binExpr);
        std::shared_ptr<IR::IRValue> convertUnaryExpr(UnaryExpr *unaryExpr);
        std::shared_ptr<IR::IRValue> convertFunctionCall(FunCall *funCall);
        std::shared_ptr<IR::IRValue> convertConstInt(ConstInt *constInt);
        std::shared_ptr<IR::IRValue> convertConstFloat(ConstFloat *constFloat);
        std::shared_ptr<IR::IRValue> convertConstString(ConstString *constString);
        std::shared_ptr<IR::IRValue> convertConstId(ConstId *constId);

        // Statement conversion
        void convertVariableDecl(VarDecl *varDecl);
        void convertAssignment(AssignNode *assignNode);
        void convertReturnNode(ReturnNode *returnNode);
        void convertIfNode(IfNode *ifNode);
        void convertWhileNode(WhileNode *whileNode);
        void convertBody(BodyNode *body);

        // Utility methods
        IR::IROpcode getBinaryOpcode(const std::string &op);
        IR::IROpcode getUnaryOpcode(const std::string &op);
        void emitInstruction(std::shared_ptr<IR::IRInstruction> instr);
    };

} // namespace zap::compiler
