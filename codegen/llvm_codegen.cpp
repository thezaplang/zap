#include "llvm_codegen.hpp"
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#include <stdexcept>

namespace codegen {

LLVMCodeGen::LLVMCodeGen() : builder_(ctx_) {
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
}

void LLVMCodeGen::generate(sema::BoundRootNode &root) {
  module_ = std::make_unique<llvm::Module>("zap_module", ctx_);
  root.accept(*this);
}

void LLVMCodeGen::printIR() const {
  if (module_)
    module_->print(llvm::outs(), nullptr);
}

bool LLVMCodeGen::emitObjectFile(const std::string &path) {
  auto targetTriple = llvm::sys::getDefaultTargetTriple();
  module_->setTargetTriple(targetTriple);

  std::string error;
  const auto *target = llvm::TargetRegistry::lookupTarget(targetTriple, error);
  if (!target) {
    llvm::errs() << "Target lookup failed: " << error << "\n";
    return false;
  }

  llvm::TargetOptions opts;
  auto *tm = target->createTargetMachine(targetTriple, "generic", "", opts,
                                         llvm::Reloc::PIC_);
  module_->setDataLayout(tm->createDataLayout());

  std::error_code ec;
  llvm::raw_fd_ostream dest(path, ec, llvm::sys::fs::OF_None);
  if (ec) {
    llvm::errs() << "Cannot open output file: " << ec.message() << "\n";
    return false;
  }

  llvm::legacy::PassManager pm;
  if (tm->addPassesToEmitFile(pm, dest, nullptr,
                              llvm::CodeGenFileType::ObjectFile)) {
    llvm::errs() << "TargetMachine cannot emit object file\n";
    return false;
  }

  pm.run(*module_);
  dest.flush();
  delete tm;
  return true;
}

llvm::Type *LLVMCodeGen::toLLVMType(const zir::Type &ty) {
  switch (ty.getKind()) {
  case zir::TypeKind::Void:
    return llvm::Type::getVoidTy(ctx_);
  case zir::TypeKind::Bool:
    return llvm::Type::getInt1Ty(ctx_);
  case zir::TypeKind::Int:
    return llvm::Type::getInt64Ty(ctx_);
  case zir::TypeKind::Float:
    return llvm::Type::getDoubleTy(ctx_);
  case zir::TypeKind::Pointer: {
    const auto &pt = static_cast<const zir::PointerType &>(ty);
    return llvm::PointerType::getUnqual(toLLVMType(*pt.getBaseType()));
  }
  case zir::TypeKind::Enum:
    return llvm::Type::getInt64Ty(ctx_);
  case zir::TypeKind::Record: {
    const auto &rt = static_cast<const zir::RecordType &>(ty);
    auto it = structCache_.find(rt.getName());
    if (it != structCache_.end())
      return it->second;

    auto *structTy = llvm::StructType::create(ctx_, rt.getName());
    structCache_[rt.getName()] = structTy;
    std::vector<llvm::Type *> fieldTypes;
    for (const auto &f : rt.getFields())
      fieldTypes.push_back(toLLVMType(*f.type));
    structTy->setBody(fieldTypes);
    return structTy;
  }
  case zir::TypeKind::Array: {
    const auto &at = static_cast<const zir::ArrayType &>(ty);
    return llvm::ArrayType::get(toLLVMType(*at.getBaseType()), at.getSize());
  }
  }
  throw std::runtime_error("Unknown ZIR type");
}

llvm::FunctionType *
LLVMCodeGen::buildFunctionType(const sema::FunctionSymbol &sym) {
  std::vector<llvm::Type *> paramTypes;
  for (const auto &param : sym.parameters)
    paramTypes.push_back(toLLVMType(*param->type));

  llvm::Type *retTy = toLLVMType(*sym.returnType);
  return llvm::FunctionType::get(retTy, paramTypes, /*isVarArg=*/false);
}

llvm::AllocaInst *LLVMCodeGen::createEntryAlloca(llvm::Function *fn,
                                                 const std::string &name,
                                                 llvm::Type *ty) {
  llvm::IRBuilder<> entry(&fn->getEntryBlock(), fn->getEntryBlock().begin());
  return entry.CreateAlloca(ty, nullptr, name);
}

void LLVMCodeGen::visit(sema::BoundRootNode &node) {
  // Declare all functions first so forward calls resolve.
  for (const auto &fn : node.functions) {
    auto *ft = buildFunctionType(*fn->symbol);
    auto *f = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                                     fn->symbol->name, *module_);
    size_t idx = 0;
    for (auto &arg : f->args())
      arg.setName(fn->symbol->parameters[idx++]->name);

    functionMap_[fn->symbol->name] = f;
  }

  for (const auto &fn : node.functions)
    fn->accept(*this);
  for (const auto &rec : node.records)
    rec->accept(*this);
  for (const auto &en : node.enums)
    en->accept(*this);
}

void LLVMCodeGen::visit(sema::BoundFunctionDeclaration &node) {
  auto *fn = functionMap_.at(node.symbol->name);
  currentFn_ = fn;
  allocaMap_.clear();

  auto *entry = llvm::BasicBlock::Create(ctx_, "entry", fn);
  builder_.SetInsertPoint(entry);

  // Spill each argument to a stack slot so we can reassign params later.
  size_t idx = 0;
  for (auto &arg : fn->args()) {
    const auto &param = node.symbol->parameters[idx++];
    auto *alloca = createEntryAlloca(fn, param->name, arg.getType());
    builder_.CreateStore(&arg, alloca);
    allocaMap_[param->name] = alloca;
  }

  node.body->accept(*this);

  // Insert a void return if the block has no terminator.
  if (!builder_.GetInsertBlock()->getTerminator()) {
    if (fn->getReturnType()->isVoidTy())
      builder_.CreateRetVoid();
    // Non-void paths without an explicit return are a semantic error;
    // the Binder should have caught them already.
  }

  currentFn_ = nullptr;
}

// Stubs for nodes that will be implemented in subsequent steps.

void LLVMCodeGen::visit(sema::BoundBlock &node) {
  for (const auto &stmt : node.statements)
    stmt->accept(*this);
}

void LLVMCodeGen::visit(sema::BoundVariableDeclaration &node) {
  auto *ty = toLLVMType(*node.symbol->type);
  auto *alloca = createEntryAlloca(currentFn_, node.symbol->name, ty);
  allocaMap_[node.symbol->name] = alloca;

  if (node.initializer) {
    node.initializer->accept(*this);
    builder_.CreateStore(lastValue_, alloca);
  }
}

void LLVMCodeGen::visit(sema::BoundReturnStatement &node) {
  if (node.expression) {
    node.expression->accept(*this);
    builder_.CreateRet(lastValue_);
  } else {
    builder_.CreateRetVoid();
  }
}

void LLVMCodeGen::visit(sema::BoundAssignment &node) {
  node.expression->accept(*this);
  auto *alloca = allocaMap_.at(node.symbol->name);
  builder_.CreateStore(lastValue_, alloca);
}

void LLVMCodeGen::visit(sema::BoundLiteral &node) {
  auto *ty = toLLVMType(*node.type);
  if (ty->isIntegerTy(1)) {
    lastValue_ = llvm::ConstantInt::get(ty, node.value == "true" ? 1 : 0);
  } else if (ty->isIntegerTy()) {
    lastValue_ =
        llvm::ConstantInt::get(ty, std::stoll(node.value), /*isSigned=*/true);
  } else if (ty->isDoubleTy()) {
    lastValue_ = llvm::ConstantFP::get(ty, std::stod(node.value));
  } else {
    lastValue_ = llvm::Constant::getNullValue(ty);
  }
}

void LLVMCodeGen::visit(sema::BoundVariableExpression &node) {
  auto *alloca = allocaMap_.at(node.symbol->name);
  lastValue_ = builder_.CreateLoad(alloca->getAllocatedType(), alloca,
                                   node.symbol->name);
}

void LLVMCodeGen::visit(sema::BoundBinaryExpression &node) {
  node.left->accept(*this);
  auto *lhs = lastValue_;
  node.right->accept(*this);
  auto *rhs = lastValue_;

  bool isFloat = lhs->getType()->isDoubleTy();

  if (node.op == "+")
    lastValue_ =
        isFloat ? builder_.CreateFAdd(lhs, rhs) : builder_.CreateAdd(lhs, rhs);
  else if (node.op == "-")
    lastValue_ =
        isFloat ? builder_.CreateFSub(lhs, rhs) : builder_.CreateSub(lhs, rhs);
  else if (node.op == "*")
    lastValue_ =
        isFloat ? builder_.CreateFMul(lhs, rhs) : builder_.CreateMul(lhs, rhs);
  else if (node.op == "/")
    lastValue_ =
        isFloat ? builder_.CreateFDiv(lhs, rhs) : builder_.CreateSDiv(lhs, rhs);
  else if (node.op == "==")
    lastValue_ = isFloat ? builder_.CreateFCmpOEQ(lhs, rhs)
                         : builder_.CreateICmpEQ(lhs, rhs);
  else if (node.op == "!=")
    lastValue_ = isFloat ? builder_.CreateFCmpONE(lhs, rhs)
                         : builder_.CreateICmpNE(lhs, rhs);
  else if (node.op == "<")
    lastValue_ = isFloat ? builder_.CreateFCmpOLT(lhs, rhs)
                         : builder_.CreateICmpSLT(lhs, rhs);
  else if (node.op == "<=")
    lastValue_ = isFloat ? builder_.CreateFCmpOLE(lhs, rhs)
                         : builder_.CreateICmpSLE(lhs, rhs);
  else if (node.op == ">")
    lastValue_ = isFloat ? builder_.CreateFCmpOGT(lhs, rhs)
                         : builder_.CreateICmpSGT(lhs, rhs);
  else if (node.op == ">=")
    lastValue_ = isFloat ? builder_.CreateFCmpOGE(lhs, rhs)
                         : builder_.CreateICmpSGE(lhs, rhs);
}

void LLVMCodeGen::visit(sema::BoundUnaryExpression &node) {
  node.expr->accept(*this);
  if (node.op == "-") {
    lastValue_ = node.type->getKind() == zir::TypeKind::Float
                     ? builder_.CreateFNeg(lastValue_)
                     : builder_.CreateNeg(lastValue_);
  } else if (node.op == "!") {
    lastValue_ = builder_.CreateNot(lastValue_);
  }
}

void LLVMCodeGen::visit(sema::BoundFunctionCall &node) {
  auto *callee = functionMap_.at(node.symbol->name);
  std::vector<llvm::Value *> args;
  for (const auto &arg : node.arguments) {
    arg->accept(*this);
    args.push_back(lastValue_);
  }
  lastValue_ = builder_.CreateCall(callee, args);
}

void LLVMCodeGen::visit(sema::BoundArrayLiteral &) { lastValue_ = nullptr; }

void LLVMCodeGen::visit(sema::BoundRecordDeclaration &node) {
  toLLVMType(*node.type);
}

void LLVMCodeGen::visit(sema::BoundEnumDeclaration &) {}

} // namespace codegen
