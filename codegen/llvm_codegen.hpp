#pragma once
#include "../sema/bound_nodes.hpp"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Target/TargetMachine.h>
#include <map>
#include <memory>
#include <string>

namespace codegen {

class LLVMCodeGen : public sema::BoundVisitor {
public:
  LLVMCodeGen();

  void generate(sema::BoundRootNode &root);

  void printIR() const;

  bool emitObjectFile(const std::string &path);

  void visit(sema::BoundRootNode &node) override;
  void visit(sema::BoundFunctionDeclaration &node) override;
  void visit(sema::BoundBlock &node) override;
  void visit(sema::BoundVariableDeclaration &node) override;
  void visit(sema::BoundReturnStatement &node) override;
  void visit(sema::BoundAssignment &node) override;
  void visit(sema::BoundLiteral &node) override;
  void visit(sema::BoundVariableExpression &node) override;
  void visit(sema::BoundBinaryExpression &node) override;
  void visit(sema::BoundUnaryExpression &node) override;
  void visit(sema::BoundFunctionCall &node) override;
  void visit(sema::BoundArrayLiteral &node) override;
  void visit(sema::BoundRecordDeclaration &node) override;
  void visit(sema::BoundEnumDeclaration &node) override;
  void visit(sema::BoundIfExpression &node) override;
  void visit(sema::BoundWhileStatement &node) override;

private:
  llvm::LLVMContext ctx_;
  llvm::IRBuilder<> builder_;
  std::unique_ptr<llvm::Module> module_;

  llvm::Function *currentFn_ = nullptr;
  llvm::Value *lastValue_ = nullptr;

  std::map<std::string, llvm::AllocaInst *> allocaMap_;
  std::map<std::string, llvm::Function *> functionMap_;
  std::map<std::string, llvm::StructType *> structCache_;

  llvm::Type *toLLVMType(const zir::Type &ty);
  llvm::FunctionType *buildFunctionType(const sema::FunctionSymbol &sym);

  // Creates an alloca in the entry block of `fn` (better for mem2reg).
  llvm::AllocaInst *createEntryAlloca(llvm::Function *fn,
                                      const std::string &name, llvm::Type *ty);
};

} // namespace codegen
