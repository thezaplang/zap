#pragma once
#include "ast/nodes.hpp"
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
        llvm::LLVMContext context_;
        llvm::Module module_{"zap_module", context_};
        llvm::IRBuilder<> builder_{context_};
        void compile(const std::unique_ptr<RootNode> &root);
        void generateFunction(const FunDecl &funDecl);
        void generateBody(const BodyNode &body);
        void generateReturn(const ReturnNode &retNode);
        llvm::Value *generateExpression(const ExpressionNode &expr);
        llvm::Type *mapType(const TypeNode &typeNode);
        void emitIRToFile(const std::string &filename);
        void compileIR(const std::string &irFilename, const std::string &outputFilename);
    };

}