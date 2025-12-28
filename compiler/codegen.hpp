#pragma once
#include "ast/nodes.hpp"
#include "../sema/sema.hpp"
#include <memory>
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/raw_ostream.h"
namespace zap
{
    class Compiler
    {

    public:
        Compiler(std::shared_ptr<sema::SymbolTable> symTable) : symTable_(symTable) {}
        llvm::LLVMContext context_;
        llvm::Module module_{"zap_module", context_};
        llvm::IRBuilder<> builder_{context_};
        std::shared_ptr<sema::SymbolTable> symTable_;
        void compile(const std::unique_ptr<RootNode> &root);
        void generateFunction(const FunDecl &funDecl);
        void generateBody(const BodyNode &body);
        void generateReturn(const ReturnNode &retNode);
        void genereteLet(const VarDecl &varDel);
        //
        llvm::Value *generateExpression(const ExpressionNode &expr);
        llvm::Type *mapType(const TypeNode &typeNode);
        void emitIRToFile(const std::string &filename);
        void compileIR(const std::string &irFilename, const std::string &outputFilename);
    };
} // namespace zap