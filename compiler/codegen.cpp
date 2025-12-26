#include "codegen.hpp"
#include <memory>
#include <llvm/IR/LLVMContext.h>
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/raw_ostream.h"
#include <fstream>
#include <iostream>
#include <cstdlib>
void zap::Compiler::compile(const std::unique_ptr<RootNode, std::default_delete<RootNode>> &root)
{
    for (const auto &child : root->children)
    {
        if (auto *funDecl = dynamic_cast<FunDecl *>(child.get()))
        {
            generateFunction(*funDecl);
        }
    }
}

void zap::Compiler::generateFunction(const FunDecl &funDecl)
{
    std::vector<llvm::Type *> paramTypes;
    for (const auto &param : funDecl.params_)
    {
        // For simplicity, assume all parameters are of type i32
        paramTypes.push_back(mapType(*param->type));
    }

    llvm::Type *returnType = mapType(*funDecl.returnType_);

    llvm::FunctionType *funcType =
        llvm::FunctionType::get(returnType, paramTypes, false);

    llvm::Function *function =
        llvm::Function::Create(funcType, llvm::Function::ExternalLinkage,
                               funDecl.name_, module_);

    unsigned idx = 0;
    for (auto &arg : function->args())
    {
        arg.setName(funDecl.params_[idx++]->name);
    }

    llvm::BasicBlock *entry =
        llvm::BasicBlock::Create(context_, "entry", function);
    builder_.SetInsertPoint(entry);

    if (funDecl.body_)
    {
        generateBody(*funDecl.body_);
    }
    else if (funDecl.isExtern_)
    {
        return;
    }
}

void zap::Compiler::generateBody(const BodyNode &body)
{
    for (const auto &stmt : body.statements)
    {
        if (auto *retNode = dynamic_cast<ReturnNode *>(stmt.get()))
        {
            generateReturn(*retNode);
        }
    }
}

void zap::Compiler::generateReturn(const ReturnNode &retNode)
{
    if (retNode.returnValue)
    {
        // TODO: check functon return type
        llvm::Value *retValue = generateExpression(*retNode.returnValue);
        builder_.CreateRet(retValue);
    }
    else
    {
        builder_.CreateRetVoid();
    }
}

llvm::Value *zap::Compiler::generateExpression(const ExpressionNode &expr)
{
    if (auto *constInt = dynamic_cast<const ConstInt *>(&expr))
    {
        llvm::Value *value = llvm::ConstantInt::get(
            llvm::Type::getInt32Ty(context_), constInt->value_);
        return value;
    }
    else if (auto *binExpr = dynamic_cast<const BinExpr *>(&expr))
    {
        llvm::Value *leftValue = generateExpression(*binExpr->left_);
        llvm::Value *rightValue = generateExpression(*binExpr->right_);
        llvm::Value *result = nullptr;
        if (binExpr->op_ == "+")
        {
            result = builder_.CreateAdd(leftValue, rightValue);
        }
        else if (binExpr->op_ == "-")
        {
            result = builder_.CreateSub(leftValue, rightValue);
        }
        else if (binExpr->op_ == "*")
        {
            result = builder_.CreateMul(leftValue, rightValue);
        }
        else if (binExpr->op_ == "/")
        {
            result = builder_.CreateSDiv(leftValue, rightValue);
        }

        return result;
    }
    else
    {
        std::cerr << "Unsupported expression type in code generation" << std::endl;
        return nullptr;
    }
}

llvm::Type *zap::Compiler::mapType(const TypeNode &typeNode)
{
    llvm::Type *baseType = nullptr;

    // Map base type
    if (typeNode.typeName == "i1")
    {
        baseType = llvm::Type::getInt1Ty(context_);
    }
    else if (typeNode.typeName == "i8")
    {
        baseType = llvm::Type::getInt8Ty(context_);
    }
    else if (typeNode.typeName == "i16")
    {
        baseType = llvm::Type::getInt16Ty(context_);
    }
    else if (typeNode.typeName == "i32")
    {
        baseType = llvm::Type::getInt32Ty(context_);
    }
    else if (typeNode.typeName == "i64")
    {
        baseType = llvm::Type::getInt64Ty(context_);
    }
    else if (typeNode.typeName == "f32")
    {
        baseType = llvm::Type::getFloatTy(context_);
    }
    else if (typeNode.typeName == "f64")
    {
        baseType = llvm::Type::getDoubleTy(context_);
    }
    else if (typeNode.typeName == "void")
    {
        baseType = llvm::Type::getVoidTy(context_);
    }
    else
    {

        baseType = llvm::Type::getInt32Ty(context_);
    }

    if (typeNode.isArray)
    {
        // TODO: Handle array size
        return llvm::ArrayType::get(baseType, 0);
    }

    if (typeNode.isPointer)
    {
        return baseType->getPointerTo();
    }

    // Handle reference type
    if (typeNode.isReference)
    {
        return baseType->getPointerTo();
    }

    return baseType;
}

void zap::Compiler::emitIRToFile(const std::string &filename)
{
    std::error_code EC;
    llvm::raw_fd_ostream OS(filename, EC);
    if (EC)
    {
        std::cerr << "Error writing to file: " << EC.message() << std::endl;
        return;
    }
    module_.print(OS, nullptr);
    OS.close();
    std::cout << "IR written to " << filename << std::endl;
}

void zap::Compiler::compileIR(const std::string &irFilename, const std::string &outputFilename)
{

    std::string objFile = outputFilename + ".o";
    std::string command = "llc -filetype=obj -o " + objFile + " " + irFilename;
    std::cout << "Running: " << command << std::endl;
    int result = system(command.c_str());

    if (result != 0)
    {
        std::cerr << "Error compiling IR with llc" << std::endl;
        return;
    }

    // Use gcc to link to final executable
    std::string linkCommand = "gcc -o " + outputFilename + " " + objFile;
    std::cout << "Running: " << linkCommand << std::endl;
    result = system(linkCommand.c_str());

    if (result != 0)
    {
        std::cerr << "Error linking executable with gcc" << std::endl;
        return;
    }

    std::cout << "Compilation successful! Output: " << outputFilename << std::endl;
}