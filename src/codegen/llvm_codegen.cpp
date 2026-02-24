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

namespace codegen
{

  LLVMCodeGen::LLVMCodeGen() : builder_(ctx_)
  {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
  }

  void LLVMCodeGen::generate(sema::BoundRootNode &root)
  {
    module_ = std::make_unique<llvm::Module>("zap_module", ctx_);
    root.accept(*this);
  }

  void LLVMCodeGen::printIR() const
  {
    if (module_)
      module_->print(llvm::outs(), nullptr);
  }

  bool LLVMCodeGen::emitObjectFile(const std::string &path)
  {
    auto targetTriple = llvm::sys::getDefaultTargetTriple();
    module_->setTargetTriple(targetTriple);
    //on newer verison of llvm  module_->setTargetTriple(llvm::Triple(targetTriple));
    std::string error;
    const auto *target = llvm::TargetRegistry::lookupTarget(targetTriple, error);
    if (!target)
    {
      llvm::errs() << "Target lookup failed: " << error << "\n";
      return false;
    }

    llvm::TargetOptions opts;
    auto *tm = target->createTargetMachine(targetTriple, "generic", "", opts,
                                           llvm::Reloc::PIC_);
    module_->setDataLayout(tm->createDataLayout());

    std::error_code ec;
    llvm::raw_fd_ostream dest(path, ec, llvm::sys::fs::OF_None);
    if (ec)
    {
      llvm::errs() << "Cannot open output file: " << ec.message() << "\n";
      return false;
    }

    llvm::legacy::PassManager pm;
    if (tm->addPassesToEmitFile(pm, dest, nullptr,
                                llvm::CodeGenFileType::ObjectFile))
    {
      llvm::errs() << "TargetMachine cannot emit object file\n";
      return false;
    }

    // TODO: Improve handling of verifying the module.
    bool is_broken = llvm::verifyModule(*module_, &llvm::errs());

    if(!is_broken) pm.run(*module_);
    dest.flush();
    delete tm;
    return !is_broken;
  }

  llvm::Type *LLVMCodeGen::toLLVMType(const zir::Type &ty)
  {
    switch (ty.getKind())
    {
    case zir::TypeKind::Void:
      return llvm::Type::getVoidTy(ctx_);
    case zir::TypeKind::Bool:
      return llvm::Type::getInt1Ty(ctx_);
    case zir::TypeKind::Int:
      return llvm::Type::getInt64Ty(ctx_);
    case zir::TypeKind::Float:
      return llvm::Type::getDoubleTy(ctx_);
    case zir::TypeKind::Pointer:
    {
      const auto &pt = static_cast<const zir::PointerType &>(ty);
      return llvm::PointerType::getUnqual(toLLVMType(*pt.getBaseType()));
    }
    case zir::TypeKind::Enum:
      return llvm::Type::getInt64Ty(ctx_);
    case zir::TypeKind::Record:
    {
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
    case zir::TypeKind::Array:
    {
      const auto &at = static_cast<const zir::ArrayType &>(ty);
      return llvm::ArrayType::get(toLLVMType(*at.getBaseType()), at.getSize());
    }
    default:
      break;
    }
    throw std::runtime_error("Unknown ZIR type: " + ty.toString());
  }

  llvm::FunctionType *
  LLVMCodeGen::buildFunctionType(const sema::FunctionSymbol &sym)
  {
    std::vector<llvm::Type *> paramTypes;
    for (const auto &param : sym.parameters)
      paramTypes.push_back(toLLVMType(*param->type));

    llvm::Type *retTy = toLLVMType(*sym.returnType);
    return llvm::FunctionType::get(retTy, paramTypes, /*isVarArg=*/false);
  }

  llvm::AllocaInst *LLVMCodeGen::createEntryAlloca(llvm::Function *fn,
                                                   const std::string &name,
                                                   llvm::Type *ty)
  {
    llvm::IRBuilder<> entry(&fn->getEntryBlock(), fn->getEntryBlock().begin());
    return entry.CreateAlloca(ty, nullptr, name);
  }

  void LLVMCodeGen::visit(sema::BoundRootNode &node)
  {
    // Declare all external functions first
    for (const auto &extFn : node.externalFunctions)
    {
      auto *ft = buildFunctionType(*extFn->symbol);
      auto *f = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                                       extFn->symbol->name, *module_);
      size_t idx = 0;
      for (auto &arg : f->args())
        arg.setName(extFn->symbol->parameters[idx++]->name);

      functionMap_[extFn->symbol->name] = f;
    }

    // Declare all functions second so forward calls resolve
    for (const auto &fn : node.functions)
    {
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

  void LLVMCodeGen::visit(sema::BoundFunctionDeclaration &node)
  {
    auto *fn = functionMap_.at(node.symbol->name);
    currentFn_ = fn;
    allocaMap_.clear();

    auto *entry = llvm::BasicBlock::Create(ctx_, "entry", fn);
    builder_.SetInsertPoint(entry);

    // Spill each argument to a stack slot so we can reassign params later.
    size_t idx = 0;
    for (auto &arg : fn->args())
    {
      const auto &param = node.symbol->parameters[idx++];
      auto *alloca = createEntryAlloca(fn, param->name, arg.getType());
      builder_.CreateStore(&arg, alloca);
      allocaMap_[param->name] = alloca;
    }

    node.body->accept(*this);

    if (!builder_.GetInsertBlock()->getTerminator())
    {
      if (fn->getReturnType()->isVoidTy())
        builder_.CreateRetVoid();
      // Non-void paths without an explicit return are a semantic error;
      // the Binder should have caught them already.
    }

    currentFn_ = nullptr;
  }

  void LLVMCodeGen::visit(sema::BoundExternalFunctionDeclaration &node)
  {
    // External functions have already been declared in visit(BoundRootNode)
    // Nothing else to do here
    (void)node;
  }

  void LLVMCodeGen::visit(sema::BoundBlock &node)
  {
    for (const auto &stmt : node.statements)
    {
      stmt->accept(*this);
    }
    if (node.result)
    {
      node.result->accept(*this);
    }
  }

  void LLVMCodeGen::visit(sema::BoundVariableDeclaration &node)
  {
    auto *ty = toLLVMType(*node.symbol->type);
    auto *alloca = createEntryAlloca(currentFn_, node.symbol->name, ty);
    allocaMap_[node.symbol->name] = alloca;

    if (node.initializer)
    {
      node.initializer->accept(*this);
      builder_.CreateStore(lastValue_, alloca);
    }
  }

  void LLVMCodeGen::visit(sema::BoundReturnStatement &node)
  {
    if (node.expression)
    {
      node.expression->accept(*this);
      builder_.CreateRet(lastValue_);
    }
    else
    {
      builder_.CreateRetVoid();
    }
  }

  void LLVMCodeGen::visit(sema::BoundAssignment &node)
  {
    node.expression->accept(*this);
    auto *alloca = allocaMap_.at(node.symbol->name);
    builder_.CreateStore(lastValue_, alloca);
  }

  void LLVMCodeGen::visit(sema::BoundExpressionStatement &node)
  {
    node.expression->accept(*this);
  }

  void LLVMCodeGen::visit(sema::BoundLiteral &node)
  {
    auto *ty = toLLVMType(*node.type);
    if (ty->isIntegerTy(1))
    {
      lastValue_ = llvm::ConstantInt::get(ty, node.value == "true" ? 1 : 0);
    }
    else if (ty->isIntegerTy())
    {
      lastValue_ =
          llvm::ConstantInt::get(ty, std::stoll(node.value), /*isSigned=*/true);
    }
    else if (ty->isDoubleTy())
    {
      lastValue_ = llvm::ConstantFP::get(ty, std::stod(node.value));
    }
    else
    {
      lastValue_ = llvm::Constant::getNullValue(ty);
    }
  }

  void LLVMCodeGen::visit(sema::BoundVariableExpression &node)
  {
    auto *alloca = allocaMap_.at(node.symbol->name);
    lastValue_ = builder_.CreateLoad(alloca->getAllocatedType(), alloca,
                                     node.symbol->name);
  }

  void LLVMCodeGen::visit(sema::BoundBinaryExpression &node)
  {
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

  void LLVMCodeGen::visit(sema::BoundUnaryExpression &node)
  {
    node.expr->accept(*this);
    if (node.op == "-")
    {
      lastValue_ = node.type->getKind() == zir::TypeKind::Float
                       ? builder_.CreateFNeg(lastValue_)
                       : builder_.CreateNeg(lastValue_);
    }
    else if (node.op == "!")
    {
      lastValue_ = builder_.CreateNot(lastValue_);
    }
  }

  void LLVMCodeGen::visit(sema::BoundFunctionCall &node)
  {
    auto *callee = functionMap_.at(node.symbol->name);
    std::vector<llvm::Value *> args;
    for (const auto &arg : node.arguments)
    {
      arg->accept(*this);
      args.push_back(lastValue_);
    }
    lastValue_ = builder_.CreateCall(callee, args);
  }

  void LLVMCodeGen::visit(sema::BoundArrayLiteral &node)
  {
    auto *arrayTy = static_cast<llvm::ArrayType *>(toLLVMType(*node.type));
    auto *elemTy = arrayTy->getElementType();
    (void)elemTy;

    std::vector<llvm::Constant *> constants;
    bool allConstants = true;

    for (const auto &expr : node.elements)
    {
      expr->accept(*this);
      if (auto *c = llvm::dyn_cast<llvm::Constant>(lastValue_))
      {
        constants.push_back(c);
      }
      else
      {
        allConstants = false;
        break;
      }
    }

    if (allConstants)
    {
      lastValue_ = llvm::ConstantArray::get(arrayTy, constants);
    }
    else
    {
      // If not all elements are constants, allocate on stack and store
      auto *alloca = createEntryAlloca(currentFn_, "array_lit", arrayTy);
      for (size_t i = 0; i < node.elements.size(); ++i)
      {
        node.elements[i]->accept(*this);
        auto *ptr = builder_.CreateStructGEP(arrayTy, alloca, i);
        builder_.CreateStore(lastValue_, ptr);
      }
      lastValue_ = builder_.CreateLoad(arrayTy, alloca);
    }
  }

  void LLVMCodeGen::visit(sema::BoundRecordDeclaration &node)
  {
    toLLVMType(*node.type);
  }

  void LLVMCodeGen::visit(sema::BoundEnumDeclaration &node)
  {
    // Enums are typically handled at the type level in ZIR/sema.
    // We don't need to generate code for the declaration itself
    // unless we want to generate debug info or constant values.
    (void)node;
  }

  void LLVMCodeGen::visit(sema::BoundIfExpression &node)
  {
    if (!currentFn_)
      throw std::runtime_error("currentFn_ is null in visit(BoundIfExpression)");

    auto *thenBB = llvm::BasicBlock::Create(ctx_, "if.then", currentFn_);
    auto *elseBB = node.elseBody
                       ? llvm::BasicBlock::Create(ctx_, "if.else", currentFn_)
                       : nullptr;
    auto *mergeBB = llvm::BasicBlock::Create(ctx_, "if.merge", currentFn_);

    if (!node.condition)
      throw std::runtime_error("condition is null in BoundIfExpression");
    node.condition->accept(*this);
    auto *cond = lastValue_;
    if (!cond)
      throw std::runtime_error(
          "lastValue_ is null after condition in BoundIfExpression");

    if (elseBB)
    {
      builder_.CreateCondBr(cond, thenBB, elseBB);
    }
    else
    {
      builder_.CreateCondBr(cond, thenBB, mergeBB);
    }

    builder_.SetInsertPoint(thenBB);
    if (node.thenBody)
    {
      lastValue_ = nullptr;
      node.thenBody->accept(*this);
    }
    auto *thenVal = lastValue_;
    if (!builder_.GetInsertBlock()->getTerminator())
    {
      builder_.CreateBr(mergeBB);
    }
    thenBB = builder_.GetInsertBlock();

    auto *elseVal = (llvm::Value *)nullptr;
    if (elseBB)
    {
      builder_.SetInsertPoint(elseBB);
      if (node.elseBody)
      {
        lastValue_ = nullptr;
        node.elseBody->accept(*this);
      }
      elseVal = lastValue_;
      if (!builder_.GetInsertBlock()->getTerminator())
      {
        builder_.CreateBr(mergeBB);
      }
      elseBB = builder_.GetInsertBlock();
    }

    builder_.SetInsertPoint(mergeBB);
    if (node.type->getKind() != zir::TypeKind::Void)
    {
      auto *phiType = toLLVMType(*node.type);
      auto *phi = builder_.CreatePHI(phiType, 2, "if.res");

      if (thenBB->getTerminator())
      {
        phi->addIncoming(thenVal ? thenVal : llvm::UndefValue::get(phiType),
                         thenBB);
      }

      if (elseBB)
      {
        if (elseBB->getTerminator())
        {
          phi->addIncoming(elseVal ? elseVal : llvm::UndefValue::get(phiType),
                           elseBB);
        }
      }
      else
      {
        phi->addIncoming(llvm::UndefValue::get(phiType), thenBB);
      }
      lastValue_ = phi;
    }
  }

  void LLVMCodeGen::visit(sema::BoundWhileStatement &node)
  {
    if (!currentFn_)
      throw std::runtime_error(
          "currentFn_ is null in visit(BoundWhileStatement)");

    auto *condBB = llvm::BasicBlock::Create(ctx_, "while.cond", currentFn_);
    auto *bodyBB = llvm::BasicBlock::Create(ctx_, "while.body", currentFn_);
    auto *endBB = llvm::BasicBlock::Create(ctx_, "while.end", currentFn_);

    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(condBB);
    if (!node.condition)
      throw std::runtime_error("condition is null in BoundWhileStatement");
    node.condition->accept(*this);
    auto *cond = lastValue_;
    if (!cond)
      throw std::runtime_error(
          "lastValue_ is null after condition in BoundWhileStatement");
    builder_.CreateCondBr(cond, bodyBB, endBB);

    builder_.SetInsertPoint(bodyBB);
    loopBBStack_.push_back({condBB, endBB});
    if (node.body)
      node.body->accept(*this);
    loopBBStack_.pop_back();
    if (!builder_.GetInsertBlock()->getTerminator())
    {
      builder_.CreateBr(condBB);
    }

    builder_.SetInsertPoint(endBB);
  }

  void LLVMCodeGen::visit(sema::BoundBreakStatement &node)
  {
    if (loopBBStack_.empty())
      return; // binder should have diagnosed
    auto endBB = loopBBStack_.back().second;
    builder_.CreateBr(endBB);
    // Create a new continuation block so subsequent instructions have a place
    auto *contBB = llvm::BasicBlock::Create(ctx_, "after.break", currentFn_);
    builder_.SetInsertPoint(contBB);
  }

  void LLVMCodeGen::visit(sema::BoundContinueStatement &node)
  {
    if (loopBBStack_.empty())
      return;
    auto condBB = loopBBStack_.back().first;
    builder_.CreateBr(condBB);
    auto *contBB = llvm::BasicBlock::Create(ctx_, "after.continue", currentFn_);
    builder_.SetInsertPoint(contBB);
  }

} // namespace codegen
