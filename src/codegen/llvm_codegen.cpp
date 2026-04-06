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
  namespace
  {
    bool isStringType(const std::shared_ptr<zir::Type> &type)
    {
      return type && type->getKind() == zir::TypeKind::Record &&
             static_cast<zir::RecordType *>(type.get())->getName() == "String";
    }
  } // namespace
  LLVMCodeGen::LLVMCodeGen() : builder_(ctx_), nextStringId_(0), evaluateAsAddr_(false)
  {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
  }

  llvm::Constant *LLVMCodeGen::getOrCreateGlobalString(const std::string &str,
                                                       std::string &globalName)
  {
    globalName = ".str." + std::to_string(nextStringId_++);

    auto *arrayTy = llvm::ArrayType::get(llvm::Type::getInt8Ty(ctx_),
                                         static_cast<unsigned>(str.size() + 1));
    auto *constArray = llvm::ConstantDataArray::getString(ctx_, str, true);

    auto *gv = new llvm::GlobalVariable(*module_, arrayTy, /*isConstant=*/true,
                                        llvm::GlobalValue::PrivateLinkage,
                                        constArray, globalName);

    auto *zero32 = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
    llvm::Constant *indices[] = {zero32, zero32};
    auto *gep = llvm::ConstantExpr::getInBoundsGetElementPtr(arrayTy, gv,
                                                             indices);
    auto *ptrTy = llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(ctx_));
    auto *ptr = llvm::ConstantExpr::getBitCast(gep, ptrTy);
    return ptr;
  }

  void LLVMCodeGen::generate(sema::BoundRootNode &root)
  {
    module_ = std::make_unique<llvm::Module>("zap_module", ctx_);
    root.accept(*this);
  }

  void LLVMCodeGen::printIR(llvm::raw_ostream &os) const
  {
    if (module_)
      module_->print(os, nullptr);
  }

  bool LLVMCodeGen::emitObjectFile(const std::string &path)
  {
    auto targetTripleStr = llvm::sys::getDefaultTargetTriple();
    llvm::Triple triple(targetTripleStr);
    module_->setTargetTriple(triple);
    std::string error;
    const auto *target = llvm::TargetRegistry::lookupTarget(targetTripleStr, error);
    if (!target)
    {
      llvm::errs() << "Target lookup failed: " << error << "\n";
      return false;
    }

    llvm::TargetOptions opts;
    auto *tm = target->createTargetMachine(triple, "generic", "", opts,
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

    if (!is_broken)
      pm.run(*module_);
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
    case zir::TypeKind::Char:
      return llvm::Type::getInt8Ty(ctx_);
    case zir::TypeKind::Int8:
    case zir::TypeKind::UInt8:
      return llvm::Type::getInt8Ty(ctx_);
    case zir::TypeKind::Int16:
    case zir::TypeKind::UInt16:
      return llvm::Type::getInt16Ty(ctx_);
    case zir::TypeKind::Int32:
    case zir::TypeKind::UInt32:
      return llvm::Type::getInt32Ty(ctx_);
    case zir::TypeKind::Int:
    case zir::TypeKind::UInt:
    case zir::TypeKind::Int64:
    case zir::TypeKind::UInt64:
      return llvm::Type::getInt64Ty(ctx_);
    case zir::TypeKind::Float:
    case zir::TypeKind::Float32:
      return llvm::Type::getFloatTy(ctx_);
    case zir::TypeKind::Float64:
      return llvm::Type::getDoubleTy(ctx_);
    case zir::TypeKind::Pointer:
    {
      const auto &pt = static_cast<const zir::PointerType &>(ty);
      auto *baseTy = toLLVMType(*pt.getBaseType());
      if (baseTy->isVoidTy())
      {
        baseTy = llvm::Type::getInt8Ty(ctx_);
      }
      return llvm::PointerType::getUnqual(baseTy);
    }
    case zir::TypeKind::NullPtr:
      return llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(ctx_));
    case zir::TypeKind::Enum:
      return llvm::Type::getInt64Ty(ctx_);
    case zir::TypeKind::Record:
    {
      const auto &rt = static_cast<const zir::RecordType &>(ty);
      auto it = structCache_.find(rt.getCodegenName());
      if (it != structCache_.end())
        return it->second;

      if (rt.getName() == "String")
      {
        auto *structTy = llvm::StructType::create(ctx_, rt.getCodegenName());
        structCache_[rt.getCodegenName()] = structTy;
        std::vector<llvm::Type *> fieldTypes;
        fieldTypes.push_back(llvm::PointerType::getUnqual(
            llvm::Type::getInt8Ty(ctx_)));
        fieldTypes.push_back(llvm::Type::getInt64Ty(ctx_));
        structTy->setBody(fieldTypes);
        return structTy;
      }

      auto *structTy = llvm::StructType::create(ctx_, rt.getCodegenName());
      structCache_[rt.getCodegenName()] = structTy;
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
  LLVMCodeGen::buildFunctionType(const sema::FunctionSymbol &sym,
                                 bool injectMainProcessArgs)
  {
    std::vector<llvm::Type *> paramTypes;
    if (injectMainProcessArgs)
    {
      paramTypes.push_back(llvm::Type::getInt32Ty(ctx_));
      paramTypes.push_back(
          llvm::PointerType::getUnqual(llvm::PointerType::getUnqual(
              llvm::Type::getInt8Ty(ctx_))));
    }
    for (const auto &param : sym.parameters)
    {
      if (param->is_variadic_pack)
      {
        paramTypes.push_back(llvm::Type::getInt32Ty(ctx_));
        paramTypes.push_back(
            llvm::PointerType::getUnqual(toLLVMType(*param->variadic_element_type)));
        continue;
      }
      auto *ty = toLLVMType(*param->type);
      if (param->is_ref)
        ty = llvm::PointerType::getUnqual(ty);
      paramTypes.push_back(ty);
    }

    llvm::Type *retTy = toLLVMType(*sym.returnType);
    return llvm::FunctionType::get(retTy, paramTypes, sym.isCVariadic);
  }

  llvm::AllocaInst *LLVMCodeGen::createEntryAlloca(llvm::Function *fn,
                                                   const std::string &name,
                                                   llvm::Type *ty)
  {
    llvm::IRBuilder<> entry(&fn->getEntryBlock(), fn->getEntryBlock().begin());
    return entry.CreateAlloca(ty, nullptr, name);
  }

  std::string LLVMCodeGen::variadicCountSlotName(const std::string &name) const
  {
    return name + "#count";
  }

  void LLVMCodeGen::visit(sema::BoundRootNode &node)
  {
    for (const auto &extFn : node.externalFunctions)
    {
      auto *ft = buildFunctionType(*extFn->symbol);
      auto *f = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                                       extFn->symbol->linkName, *module_);
      auto argIt = f->arg_begin();
      for (const auto &param : extFn->symbol->parameters)
      {
        if (param->is_variadic_pack)
        {
          argIt->setName(param->name + ".count");
          ++argIt;
          argIt->setName(param->name + ".data");
          ++argIt;
          continue;
        }
        argIt->setName(param->name);
        ++argIt;
      }

      functionMap_[extFn->symbol->linkName] = f;
    }

    for (const auto &fn : node.functions)
    {
      bool isEntryMain = fn->symbol->linkName == "main";
      auto *ft = buildFunctionType(*fn->symbol, isEntryMain);
      auto *f = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                                       fn->symbol->linkName, *module_);
      auto argIt = f->arg_begin();
      if (isEntryMain)
      {
        argIt->setName("argc");
        ++argIt;
        argIt->setName("argv");
        ++argIt;
      }
      for (const auto &param : fn->symbol->parameters)
      {
        if (param->is_variadic_pack)
        {
          argIt->setName(param->name + ".count");
          ++argIt;
          argIt->setName(param->name + ".data");
          ++argIt;
          continue;
        }
        argIt->setName(param->name);
        ++argIt;
      }

      functionMap_[fn->symbol->linkName] = f;
    }

    for (const auto &global : node.globals)
      global->accept(*this);
    for (const auto &fn : node.functions)
      fn->accept(*this);
    for (const auto &rec : node.records)
      rec->accept(*this);
    for (const auto &en : node.enums)
      en->accept(*this);
  }

  void LLVMCodeGen::visit(sema::BoundFunctionDeclaration &node)
  {
    auto *fn = functionMap_.at(node.symbol->linkName);
    currentFn_ = fn;
    localValues_.clear();

    auto *entry = llvm::BasicBlock::Create(ctx_, "entry", fn);
    builder_.SetInsertPoint(entry);

    auto argIt = fn->arg_begin();
    if (node.symbol->linkName == "main")
    {
      auto *i32Ty = llvm::Type::getInt32Ty(ctx_);
      auto *i8PtrTy = llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(ctx_));
      auto *argvTy = llvm::PointerType::getUnqual(i8PtrTy);

      argIt->setName("argc");
      llvm::Value *argcValue = &*argIt++;
      argIt->setName("argv");
      llvm::Value *argvValue = &*argIt++;

      if (functionMap_.count("__zap_process_set_args") == 0)
      {
        auto *ft = llvm::FunctionType::get(
            llvm::Type::getVoidTy(ctx_), {i32Ty, argvTy}, false);
        auto *setArgsFn = llvm::Function::Create(
            ft, llvm::Function::ExternalLinkage, "__zap_process_set_args", *module_);
        functionMap_["__zap_process_set_args"] = setArgsFn;
      }

      builder_.CreateCall(functionMap_.at("__zap_process_set_args"),
                          {builder_.CreateIntCast(argcValue, i32Ty, true), argvValue});
    }

    // Spill each argument to a stack slot so we can reassign params later.
    size_t idx = 0;
    for (; argIt != fn->arg_end(); ++argIt)
    {
      const auto &param = node.symbol->parameters[idx++];
      if (param->is_variadic_pack)
      {
        auto &countArg = *argIt;
        auto *countAlloca = createEntryAlloca(fn, variadicCountSlotName(param->name),
                                              countArg.getType());
        builder_.CreateStore(&countArg, countAlloca);
        localValues_[variadicCountSlotName(param->name)] = countAlloca;

        ++argIt;
        auto &dataArg = *argIt;
        auto *dataAlloca = createEntryAlloca(fn, param->name, dataArg.getType());
        builder_.CreateStore(&dataArg, dataAlloca);
        localValues_[param->name] = dataAlloca;
        continue;
      }

      auto &arg = *argIt;
      auto *alloca = createEntryAlloca(fn, param->name, arg.getType());
      builder_.CreateStore(&arg, alloca);
      localValues_[param->name] = alloca;
    }

    node.body->accept(*this);

    if (!builder_.GetInsertBlock()->getTerminator())
    {
      if (node.body->result)
      {
        builder_.CreateRet(lastValue_);
      }
      else if (fn->getReturnType()->isVoidTy())
      {
        builder_.CreateRetVoid();
      }
    }

    currentFn_ = nullptr;
  }

  void LLVMCodeGen::visit(sema::BoundExternalFunctionDeclaration &node)
  {
    (void)node;
  }

  void LLVMCodeGen::visit(sema::BoundModuleReference &node)
  {
    (void)node;
    throw std::runtime_error("module reference reached codegen");
  }

  void LLVMCodeGen::visit(sema::BoundBlock &node)
  {
    for (const auto &stmt : node.statements)
    {
      if (builder_.GetInsertBlock()->getTerminator())
      {
        break;
      }
      stmt->accept(*this);
    }
    if (node.result && !builder_.GetInsertBlock()->getTerminator())
    {
      node.result->accept(*this);
    }
  }

  void LLVMCodeGen::visit(sema::BoundVariableDeclaration &node)
  {
    auto *ty = toLLVMType(*node.symbol->type);

    if (currentFn_)
    {
      auto *alloca = createEntryAlloca(currentFn_, node.symbol->name, ty);
      localValues_[node.symbol->name] = alloca;

      if (node.initializer)
      {
        node.initializer->accept(*this);
        builder_.CreateStore(lastValue_, alloca);
      }
    }
    else
    {
      llvm::Constant *initializer = nullptr;
      if (node.initializer)
      {
        node.initializer->accept(*this);
        initializer = llvm::dyn_cast<llvm::Constant>(lastValue_);
      }

      if (!initializer)
      {
        // If no initializer or initializer is not a constant, use null initializer
        initializer = llvm::Constant::getNullValue(ty);
      }

      auto *gv = new llvm::GlobalVariable(*module_, ty, node.symbol->is_const,
                                          llvm::GlobalVariable::ExternalLinkage,
                                          initializer, node.symbol->linkName);
      globalValues_[node.symbol->linkName] = gv;
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

  void LLVMCodeGen::visit(sema::BoundCast &node)
  {
    node.expression->accept(*this);
    auto *src = lastValue_;
    auto *srcTy = src->getType();
    auto *destTy = toLLVMType(*node.type);

    if (isStringType(node.expression->type) && destTy->isPointerTy())
    {
      auto *ptr = builder_.CreateExtractValue(src, {0}, "str.ptr");
      if (ptr->getType() == destTy)
      {
        lastValue_ = ptr;
      }
      else
      {
        lastValue_ = builder_.CreateBitCast(ptr, destTy);
      }
      return;
    }

    if (srcTy == destTy)
    {
      lastValue_ = src;
      return;
    }

    if (srcTy->isPointerTy() && destTy->isPointerTy())
    {
      lastValue_ = builder_.CreateBitCast(src, destTy);
    }
    else if (srcTy->isPointerTy() && destTy->isIntegerTy())
    {
      lastValue_ = builder_.CreatePtrToInt(src, destTy);
    }
    else if (srcTy->isIntegerTy() && destTy->isPointerTy())
    {
      lastValue_ = builder_.CreateIntToPtr(src, destTy);
    }
    else if (srcTy->isIntegerTy() && destTy->isIntegerTy())
    {
      unsigned srcBits = srcTy->getIntegerBitWidth();
      unsigned destBits = destTy->getIntegerBitWidth();

      if (destBits > srcBits)
      {
        if (node.expression->type->isUnsigned())
        {
          lastValue_ = builder_.CreateZExt(src, destTy);
        }
        else
        {
          lastValue_ = builder_.CreateSExt(src, destTy);
        }
      }
      else if (destBits < srcBits)
      {
        lastValue_ = builder_.CreateTrunc(src, destTy);
      }
      else
      {
        // Same bit width but different signedness in our ZIR, but LLVM doesn't care
        lastValue_ = src;
      }
    }
    else if (srcTy->isIntegerTy() && destTy->isFloatingPointTy())
    {
      if (node.expression->type->isUnsigned())
      {
        lastValue_ = builder_.CreateUIToFP(src, destTy);
      }
      else
      {
        lastValue_ = builder_.CreateSIToFP(src, destTy);
      }
    }
    else if (srcTy->isFloatingPointTy() && destTy->isIntegerTy())
    {
      if (node.type->isUnsigned())
      {
        lastValue_ = builder_.CreateFPToUI(src, destTy);
      }
      else
      {
        lastValue_ = builder_.CreateFPToSI(src, destTy);
      }
    }
    else if (srcTy->isFloatingPointTy() && destTy->isFloatingPointTy())
    {
      if (srcTy->getPrimitiveSizeInBits() < destTy->getPrimitiveSizeInBits())
      {
        lastValue_ = builder_.CreateFPExt(src, destTy);
      }
      else if (srcTy->getPrimitiveSizeInBits() > destTy->getPrimitiveSizeInBits())
      {
        lastValue_ = builder_.CreateFPTrunc(src, destTy);
      }
      else
      {
        lastValue_ = src;
      }
    }
    else
    {
      // Fallback or other pointer casts
      lastValue_ = builder_.CreateBitCast(src, destTy);
    }
  }

  void LLVMCodeGen::visit(sema::BoundAssignment &node)
  {
    node.expression->accept(*this);
    llvm::Value *val = lastValue_;

    bool old = evaluateAsAddr_;
    evaluateAsAddr_ = true;
    node.target->accept(*this);
    llvm::Value *alloca = lastValue_;
    evaluateAsAddr_ = old;

    builder_.CreateStore(val, alloca);
  }

  void LLVMCodeGen::visit(sema::BoundExpressionStatement &node)
  {
    node.expression->accept(*this);
  }

  void LLVMCodeGen::visit(sema::BoundLiteral &node)
  {
    if (node.type->getKind() == zir::TypeKind::Record)
    {
      const auto &rt = static_cast<const zir::RecordType &>(*node.type);
      if (rt.getName() == "String")
      {
        std::string gname;
        auto *ptrConst = getOrCreateGlobalString(node.value, gname);
        auto *lenConst =
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_),
                                   static_cast<uint64_t>(node.value.size()));

        auto *structTy = static_cast<llvm::StructType *>(toLLVMType(*node.type));
        std::vector<llvm::Constant *> elems;
        elems.push_back(ptrConst);
        elems.push_back(lenConst);
        lastValue_ = llvm::ConstantStruct::get(structTy, elems);
        return;
      }
    }

    auto *ty = toLLVMType(*node.type);
    if (ty->isIntegerTy(1))
    {
      lastValue_ = llvm::ConstantInt::get(ty, node.value == "true" ? 1 : 0);
    }
    else if (ty->isIntegerTy(8))
    {
      int64_t code = 0;
      if (!node.value.empty())
      {
        if (node.value.size() >= 2 && node.value[0] == '\\')
        {
          switch (node.value[1])
          {
          case 'n':
            code = '\n';
            break;
          case 't':
            code = '\t';
            break;
          case 'r':
            code = '\r';
            break;
          case '\\':
            code = '\\';
            break;
          case '\'':
            code = '\'';
            break;
          case '0':
            code = '\0';
            break;
          default:
            code = static_cast<unsigned char>(node.value[1]);
            break;
          }
        }
        else
        {
          code = static_cast<unsigned char>(node.value[0]);
        }
      }
      lastValue_ = llvm::ConstantInt::get(ty, code, /*isSigned=*/false);
    }
    else if (ty->isIntegerTy())
    {
      if (node.type->isUnsigned())
      {
        lastValue_ =
            llvm::ConstantInt::get(ty, std::stoull(node.value), /*isSigned=*/false);
      }
      else
      {
        lastValue_ =
            llvm::ConstantInt::get(ty, std::stoll(node.value), /*isSigned=*/true);
      }
    }
    else if (ty->isFloatTy())
    {
      lastValue_ = llvm::ConstantFP::get(ty, std::stof(node.value));
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
    llvm::Value *addr = nullptr;
    if (localValues_.count(node.symbol->name))
    {
      addr = localValues_.at(node.symbol->name);
    }
    else
    {
      addr = globalValues_.at(node.symbol->linkName);
    }

    if (node.symbol->is_ref)
    {
       // addr is a pointer to the pointer passed as argument
       auto *ptrTy = llvm::PointerType::getUnqual(toLLVMType(*node.symbol->type));
       addr = builder_.CreateLoad(ptrTy, addr, node.symbol->name + ".ptr");
    }

    if (evaluateAsAddr_)
    {
      lastValue_ = addr;
    }
    else
    {
      auto *ty = toLLVMType(*node.symbol->type);
      lastValue_ = builder_.CreateLoad(ty, addr,
                                       node.symbol->name);
    }
  }

  void LLVMCodeGen::visit(sema::BoundBinaryExpression &node)
  {
    if (node.op == "&&")
    {
      auto *rhsBB = llvm::BasicBlock::Create(ctx_, "and.rhs", currentFn_);
      auto *mergeBB = llvm::BasicBlock::Create(ctx_, "and.merge", currentFn_);

      node.left->accept(*this);
      auto *lhs = lastValue_;
      auto *leftBB = builder_.GetInsertBlock();
      builder_.CreateCondBr(lhs, rhsBB, mergeBB);

      builder_.SetInsertPoint(rhsBB);
      node.right->accept(*this);
      auto *rhs = lastValue_;
      auto *actualRhsBB = builder_.GetInsertBlock();
      builder_.CreateBr(mergeBB);

      builder_.SetInsertPoint(mergeBB);
      auto *phi = builder_.CreatePHI(llvm::Type::getInt1Ty(ctx_), 2, "and.res");
      phi->addIncoming(llvm::ConstantInt::get(llvm::Type::getInt1Ty(ctx_), 0), leftBB);
      phi->addIncoming(rhs, actualRhsBB);
      lastValue_ = phi;
      return;
    }

    if (node.op == "||")
    {
      auto *rhsBB = llvm::BasicBlock::Create(ctx_, "or.rhs", currentFn_);
      auto *mergeBB = llvm::BasicBlock::Create(ctx_, "or.merge", currentFn_);

      node.left->accept(*this);
      auto *lhs = lastValue_;
      auto *leftBB = builder_.GetInsertBlock();
      builder_.CreateCondBr(lhs, mergeBB, rhsBB);

      builder_.SetInsertPoint(rhsBB);
      node.right->accept(*this);
      auto *rhs = lastValue_;
      auto *actualRhsBB = builder_.GetInsertBlock();
      builder_.CreateBr(mergeBB);

      builder_.SetInsertPoint(mergeBB);
      auto *phi = builder_.CreatePHI(llvm::Type::getInt1Ty(ctx_), 2, "or.res");
      phi->addIncoming(llvm::ConstantInt::get(llvm::Type::getInt1Ty(ctx_), 1), leftBB);
      phi->addIncoming(rhs, actualRhsBB);
      lastValue_ = phi;
      return;
    }

    node.left->accept(*this);
    auto *lhs = lastValue_;
    node.right->accept(*this);
    auto *rhs = lastValue_;

    bool isPointer = lhs->getType()->isPointerTy() || rhs->getType()->isPointerTy();
    bool isFP = lhs->getType()->isFloatingPointTy();
    bool isUnsigned = node.left->type->isUnsigned();

    if (isPointer && (node.op == "+" || node.op == "-"))
    {
      llvm::Value *pointerValue = lhs->getType()->isPointerTy() ? lhs : rhs;
      llvm::Value *offsetValue = lhs->getType()->isPointerTy() ? rhs : lhs;
      auto ptrType = std::static_pointer_cast<zir::PointerType>(
          lhs->getType()->isPointerTy() ? node.left->type : node.right->type);
      auto *elemTy = toLLVMType(*ptrType->getBaseType());

      if (lhs->getType()->isPointerTy() && rhs->getType()->isPointerTy())
      {
        auto *i64Ty = llvm::Type::getInt64Ty(ctx_);
        auto *lhsInt = builder_.CreatePtrToInt(lhs, i64Ty);
        auto *rhsInt = builder_.CreatePtrToInt(rhs, i64Ty);
        auto *bytes = builder_.CreateSub(lhsInt, rhsInt);
        llvm::Value *elemSize64 = llvm::ConstantExpr::getSizeOf(elemTy);
        if (elemSize64->getType() != i64Ty)
        {
          elemSize64 = builder_.CreateIntCast(elemSize64, i64Ty, /*isSigned=*/false);
        }
        lastValue_ = builder_.CreateSDiv(bytes, elemSize64);
        return;
      }

      auto *indexTy = llvm::Type::getInt64Ty(ctx_);
      auto *index = builder_.CreateIntCast(offsetValue, indexTy, /*isSigned=*/true);
      if (node.op == "-")
      {
        index = builder_.CreateNeg(index);
      }
      lastValue_ = builder_.CreateInBoundsGEP(elemTy, pointerValue, index);
      return;
    }

    if (node.op == "+")
      lastValue_ =
          isFP ? builder_.CreateFAdd(lhs, rhs) : builder_.CreateAdd(lhs, rhs);
    else if (node.op == "-")
      lastValue_ =
          isFP ? builder_.CreateFSub(lhs, rhs) : builder_.CreateSub(lhs, rhs);
    else if (node.op == "*")
      lastValue_ =
          isFP ? builder_.CreateFMul(lhs, rhs) : builder_.CreateMul(lhs, rhs);
    else if (node.op == "/")
      lastValue_ =
          isFP ? builder_.CreateFDiv(lhs, rhs)
                  : (isUnsigned ? builder_.CreateUDiv(lhs, rhs)
                                : builder_.CreateSDiv(lhs, rhs));
    else if (node.op == "%")
      lastValue_ =
          isFP ? builder_.CreateFRem(lhs, rhs)
                  : (isUnsigned ? builder_.CreateURem(lhs, rhs)
                                : builder_.CreateSRem(lhs, rhs));
    else if (node.op == "==")
      lastValue_ = isFP ? builder_.CreateFCmpOEQ(lhs, rhs)
                           : builder_.CreateICmpEQ(lhs, rhs);
    else if (node.op == "!=")
      lastValue_ = isFP ? builder_.CreateFCmpONE(lhs, rhs)
                           : builder_.CreateICmpNE(lhs, rhs);
    else if (node.op == "<")
      lastValue_ = isFP ? builder_.CreateFCmpOLT(lhs, rhs)
                           : (isUnsigned ? builder_.CreateICmpULT(lhs, rhs)
                                         : builder_.CreateICmpSLT(lhs, rhs));
    else if (node.op == "<=")
      lastValue_ = isFP ? builder_.CreateFCmpOLE(lhs, rhs)
                           : (isUnsigned ? builder_.CreateICmpULE(lhs, rhs)
                                         : builder_.CreateICmpSLE(lhs, rhs));
    else if (node.op == ">")
      lastValue_ = isFP ? builder_.CreateFCmpOGT(lhs, rhs)
                           : (isUnsigned ? builder_.CreateICmpUGT(lhs, rhs)
                                         : builder_.CreateICmpSGT(lhs, rhs));
    else if (node.op == ">=")
      lastValue_ = isFP ? builder_.CreateFCmpOGE(lhs, rhs)
                           : (isUnsigned ? builder_.CreateICmpUGE(lhs, rhs)
                                         : builder_.CreateICmpSGE(lhs, rhs));
    else if (node.op == "~")
    {
      auto *i8Ty = llvm::Type::getInt8Ty(ctx_);
      auto *i64Ty = llvm::Type::getInt64Ty(ctx_);

      llvm::Value *lhs_ptr = nullptr;
      llvm::Value *lhs_len = nullptr;
      llvm::Value *rhs_ptr = nullptr;
      llvm::Value *rhs_len = nullptr;

      if (node.left->type->getKind() == zir::TypeKind::Record)
      {
        lhs_ptr = builder_.CreateExtractValue(lhs, {0});
        lhs_len = builder_.CreateExtractValue(lhs, {1});
      }
      else if (node.left->type->getKind() == zir::TypeKind::Char)
      {
        auto *buf = createEntryAlloca(currentFn_, "char_buf_l", i8Ty);
        builder_.CreateStore(lhs, buf);
        lhs_ptr = buf;
        lhs_len = llvm::ConstantInt::get(i64Ty, 1);
      }

      if (node.right->type->getKind() == zir::TypeKind::Record)
      {
        rhs_ptr = builder_.CreateExtractValue(rhs, {0});
        rhs_len = builder_.CreateExtractValue(rhs, {1});
      }
      else if (node.right->type->getKind() == zir::TypeKind::Char)
      {
        auto *buf = createEntryAlloca(currentFn_, "char_buf_r", i8Ty);
        builder_.CreateStore(rhs, buf);
        rhs_ptr = buf;
        rhs_len = llvm::ConstantInt::get(i64Ty, 1);
      }

      if (functionMap_.count("string_concat_ptrlen") == 0)
      {
        std::vector<llvm::Type *> params = {
            llvm::PointerType::getUnqual(i8Ty), i64Ty,
            llvm::PointerType::getUnqual(i8Ty), i64Ty};
        auto *ft = llvm::FunctionType::get(llvm::PointerType::getUnqual(i8Ty), params, false);
        auto *fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "string_concat_ptrlen", *module_);
        functionMap_["string_concat_ptrlen"] = fn;
      }

      auto *concatFn = functionMap_.at("string_concat_ptrlen");
      auto *call = builder_.CreateCall(concatFn, {lhs_ptr, lhs_len, rhs_ptr, rhs_len});

      auto *sumLen = builder_.CreateAdd(lhs_len, rhs_len);

      auto *structTy = static_cast<llvm::StructType *>(toLLVMType(*node.type));
      llvm::Value *res = llvm::UndefValue::get(structTy);
      res = builder_.CreateInsertValue(res, call, {0});
      res = builder_.CreateInsertValue(res, sumLen, {1});
      lastValue_ = res;
    }
  }

  void LLVMCodeGen::visit(sema::BoundTernaryExpression &node)
  {
    auto *thenBB = llvm::BasicBlock::Create(ctx_, "ternary.then", currentFn_);
    auto *elseBB = llvm::BasicBlock::Create(ctx_, "ternary.else", currentFn_);
    auto *mergeBB = llvm::BasicBlock::Create(ctx_, "ternary.merge", currentFn_);

    node.condition->accept(*this);
    auto *cond = lastValue_;
    builder_.CreateCondBr(cond, thenBB, elseBB);

    builder_.SetInsertPoint(thenBB);
    node.thenExpr->accept(*this);
    auto *thenVal = lastValue_;
    auto *actualThenBB = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(elseBB);
    node.elseExpr->accept(*this);
    auto *elseVal = lastValue_;
    auto *actualElseBB = builder_.GetInsertBlock();
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(mergeBB);
    auto *phiType = toLLVMType(*node.type);
    auto *phi = builder_.CreatePHI(phiType, 2, "ternary.res");
    phi->addIncoming(thenVal, actualThenBB);
    phi->addIncoming(elseVal, actualElseBB);
    lastValue_ = phi;
  }

  void LLVMCodeGen::visit(sema::BoundUnaryExpression &node)
  {
    if (node.op == "&")
    {
      bool old = evaluateAsAddr_;
      evaluateAsAddr_ = true;
      node.expr->accept(*this);
      evaluateAsAddr_ = old;
      return;
    }
    else if (node.op == "*")
    {
      bool old = evaluateAsAddr_;
      evaluateAsAddr_ = false;
      node.expr->accept(*this);
      evaluateAsAddr_ = old;
      auto *ptr = lastValue_;
      if (evaluateAsAddr_)
      {
        lastValue_ = ptr;
      }
      else
      {
        lastValue_ = builder_.CreateLoad(toLLVMType(*node.type), ptr, "deref");
      }
      return;
    }

    node.expr->accept(*this);
    if (node.op == "-")
    {
      lastValue_ = node.type->isFloatingPoint()
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
    auto *callee = functionMap_.at(node.symbol->linkName);
    std::vector<llvm::Value *> args;
    size_t fixedParamCount = node.symbol->fixedParameterCount();
    for (size_t i = 0; i < node.arguments.size(); ++i)
    {
      bool isRef = false;
      if (i < fixedParamCount && i < node.argumentIsRef.size())
        isRef = node.argumentIsRef[i];
      
      bool old = evaluateAsAddr_;
      if (isRef)
        evaluateAsAddr_ = true;
      
      node.arguments[i]->accept(*this);
      args.push_back(lastValue_);
      
      evaluateAsAddr_ = old;
    }

    if (node.symbol->hasVariadicParameter())
    {
      auto variadicParam = node.symbol->variadicParameter();
      auto *elemTy = toLLVMType(*variadicParam->variadic_element_type);
      auto *elemPtrTy = llvm::PointerType::getUnqual(elemTy);
      size_t explicitVariadicCount = node.arguments.size() - fixedParamCount;

      llvm::Value *forwardedCount = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
      llvm::Value *forwardedData = llvm::ConstantPointerNull::get(elemPtrTy);
      if (node.variadicPack)
      {
        auto *packVar = dynamic_cast<sema::BoundVariableExpression *>(node.variadicPack.get());
        node.variadicPack->accept(*this);
        forwardedData = lastValue_;
        auto *countAlloca = localValues_.at(variadicCountSlotName(packVar->symbol->name));
        forwardedCount = builder_.CreateLoad(llvm::Type::getInt32Ty(ctx_), countAlloca,
                                             packVar->symbol->name + ".count");
      }

      llvm::Value *explicitCount =
          llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_),
                                 static_cast<uint64_t>(explicitVariadicCount));

      if (explicitVariadicCount == 0 && !node.variadicPack)
      {
        args.push_back(explicitCount);
        args.push_back(llvm::ConstantPointerNull::get(elemPtrTy));
      }
      else if (explicitVariadicCount == 0)
      {
        args.push_back(forwardedCount);
        args.push_back(forwardedData);
      }
      else
      {
        llvm::Value *totalCount = explicitCount;
        if (node.variadicPack)
        {
          totalCount = builder_.CreateAdd(explicitCount, forwardedCount, "varargs.total");
        }

        auto *buffer = builder_.CreateAlloca(elemTy, totalCount, "varargs.buf");
        for (size_t i = 0; i < explicitVariadicCount; ++i)
        {
          auto *dst = builder_.CreateInBoundsGEP(
              elemTy, buffer,
              llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_),
                                     static_cast<uint64_t>(i)));
          builder_.CreateStore(args[fixedParamCount + i], dst);
        }

        if (node.variadicPack)
        {
          auto *copyCondBB = llvm::BasicBlock::Create(ctx_, "varargs.copy.cond", currentFn_);
          auto *copyBodyBB = llvm::BasicBlock::Create(ctx_, "varargs.copy.body", currentFn_);
          auto *copyDoneBB = llvm::BasicBlock::Create(ctx_, "varargs.copy.done", currentFn_);
          auto *copyPreheaderBB = builder_.GetInsertBlock();

          builder_.CreateBr(copyCondBB);
          builder_.SetInsertPoint(copyCondBB);
          auto *indexPhi = builder_.CreatePHI(llvm::Type::getInt32Ty(ctx_), 2, "varargs.i");
          indexPhi->addIncoming(
              llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0),
              copyPreheaderBB);
          auto *hasMore = builder_.CreateICmpSLT(indexPhi, forwardedCount);
          builder_.CreateCondBr(hasMore, copyBodyBB, copyDoneBB);

          builder_.SetInsertPoint(copyBodyBB);
          auto *src = builder_.CreateInBoundsGEP(elemTy, forwardedData, indexPhi);
          auto *dstIndex = builder_.CreateAdd(explicitCount, indexPhi);
          auto *dst = builder_.CreateInBoundsGEP(elemTy, buffer, dstIndex);
          auto *loaded = builder_.CreateLoad(elemTy, src);
          builder_.CreateStore(loaded, dst);
          auto *nextIndex = builder_.CreateAdd(
              indexPhi, llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 1));
          builder_.CreateBr(copyCondBB);
          indexPhi->addIncoming(nextIndex, copyBodyBB);

          builder_.SetInsertPoint(copyDoneBB);
        }

        args.resize(fixedParamCount);
        args.push_back(totalCount);
        args.push_back(buffer);
      }
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
        auto *ptr = builder_.CreateConstGEP2_32(arrayTy, alloca, 0, (unsigned)i);
        builder_.CreateStore(lastValue_, ptr);
      }
      lastValue_ = builder_.CreateLoad(arrayTy, alloca);
    }
  }

  void LLVMCodeGen::visit(sema::BoundIndexAccess &node)
  {
    llvm::Value *leftAddr = nullptr;
    llvm::Value *leftValue = nullptr;
    bool old = evaluateAsAddr_;

    if (isStringType(node.left->type))
    {
      evaluateAsAddr_ = false;
      node.left->accept(*this);
      leftValue = lastValue_;
    }
    else
    {
      evaluateAsAddr_ = true;
      node.left->accept(*this);
      leftAddr = lastValue_;
    }

    evaluateAsAddr_ = false;
    node.index->accept(*this);
    llvm::Value *indexVal = lastValue_;
    evaluateAsAddr_ = old;

    llvm::Value *elemAddr = nullptr;

    if (node.left->type->getKind() == zir::TypeKind::Array)
    {
      auto *leftTy = toLLVMType(*node.left->type);
      auto *i32Ty = llvm::Type::getInt32Ty(ctx_);
      std::vector<llvm::Value *> indices = {
          llvm::ConstantInt::get(i32Ty, 0),
          builder_.CreateIntCast(indexVal, i32Ty, /*isSigned=*/false)};
      elemAddr = builder_.CreateInBoundsGEP(leftTy, leftAddr, indices);
    }
    else if (isStringType(node.left->type))
    {
      if (evaluateAsAddr_)
      {
        throw std::runtime_error("String index access is not assignable.");
      }

      auto *ptr = builder_.CreateExtractValue(leftValue, {0}, "string.ptr");
      auto *i8Ty = llvm::Type::getInt8Ty(ctx_);
      elemAddr = builder_.CreateInBoundsGEP(i8Ty, ptr, indexVal, "string.index");
    }
    else
    {
      throw std::runtime_error("Type '" + node.left->type->toString() + "' does not support indexing.");
    }

    if (evaluateAsAddr_)
    {
      lastValue_ = elemAddr;
    }
    else
    {
      auto *ty = toLLVMType(*node.type);
      lastValue_ = builder_.CreateLoad(ty, elemAddr, "index_access");
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

  void LLVMCodeGen::visit(sema::BoundMemberAccess &node)
  {
    llvm::Value *leftAddr = nullptr;
    
    // Check if left is a dereference (*ptr).field
    auto *unary = dynamic_cast<sema::BoundUnaryExpression*>(node.left.get());
    if (unary && unary->op == "*")
    {
      // Evaluate the pointer directly, skip the dereference
      bool old = evaluateAsAddr_;
      evaluateAsAddr_ = false;
      unary->expr->accept(*this);
      leftAddr = lastValue_;
      evaluateAsAddr_ = old;
    }
    else
    {
      // Normal case: evaluate as address
      bool old = evaluateAsAddr_;
      evaluateAsAddr_ = true;
      node.left->accept(*this);
      leftAddr = lastValue_;
      evaluateAsAddr_ = old;
    }

    auto recordType = std::static_pointer_cast<zir::RecordType>(node.left->type);
    int fieldIndex = -1;
    const auto &fields = recordType->getFields();
    for (size_t i = 0; i < fields.size(); ++i)
    {
      if (fields[i].name == node.member)
      {
        fieldIndex = static_cast<int>(i);
        break;
      }
    }

    if (fieldIndex == -1)
      throw std::runtime_error("Field '" + node.member + "' not found in type '" + node.left->type->toString() + "'");

    llvm::StructType *structTy = static_cast<llvm::StructType *>(toLLVMType(*recordType));
    llvm::Value *fieldAddr = builder_.CreateStructGEP(structTy, leftAddr, fieldIndex, node.member);

    if (evaluateAsAddr_)
    {
      lastValue_ = fieldAddr;
    }
    else
    {
      lastValue_ = builder_.CreateLoad(toLLVMType(*node.type), fieldAddr, node.member);
    }
  }

  void LLVMCodeGen::visit(sema::BoundStructLiteral &node)
  {
    auto recordType = std::static_pointer_cast<zir::RecordType>(node.type);
    llvm::StructType *structTy = static_cast<llvm::StructType *>(toLLVMType(*recordType));
    
    // Create an alloca for the struct
    llvm::Value *structAddr = createEntryAlloca(currentFn_, "struct_literal", structTy);
    
    for (const auto &fieldInit : node.fields)
    {
      // Find field index
      int fieldIndex = -1;
      const auto &fields = recordType->getFields();
      for (size_t i = 0; i < fields.size(); ++i)
      {
        if (fields[i].name == fieldInit.first)
        {
          fieldIndex = static_cast<int>(i);
          break;
        }
      }
      
      fieldInit.second->accept(*this);
      llvm::Value *val = lastValue_;
      
      llvm::Value *fieldAddr = builder_.CreateStructGEP(structTy, structAddr, fieldIndex);
      builder_.CreateStore(val, fieldAddr);
    }
    
    if (evaluateAsAddr_)
    {
      lastValue_ = structAddr;
    }
    else
    {
      lastValue_ = builder_.CreateLoad(structTy, structAddr);
    }
  }

  void LLVMCodeGen::visit(sema::BoundIfStatement &node)
  {
    if (!currentFn_)
      throw std::runtime_error("currentFn_ is null in visit(BoundIfStatement)");

    auto *thenBB = llvm::BasicBlock::Create(ctx_, "if.then", currentFn_);
    auto *elseBB = node.elseBody
                       ? llvm::BasicBlock::Create(ctx_, "if.else", currentFn_)
                       : nullptr;
    auto *mergeBB = llvm::BasicBlock::Create(ctx_, "if.merge", currentFn_);

    if (!node.condition)
      throw std::runtime_error("condition is null in BoundIfStatement");
    node.condition->accept(*this);
    auto *cond = lastValue_;
    if (!cond)
      throw std::runtime_error(
          "lastValue_ is null after condition in BoundIfStatement");

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
    if (!builder_.GetInsertBlock()->getTerminator())
    {
      builder_.CreateBr(mergeBB);
    }
    thenBB = builder_.GetInsertBlock();

    if (elseBB)
    {
      builder_.SetInsertPoint(elseBB);
      if (node.elseBody)
      {
        lastValue_ = nullptr;
        node.elseBody->accept(*this);
      }
      if (!builder_.GetInsertBlock()->getTerminator())
      {
        builder_.CreateBr(mergeBB);
      }
      elseBB = builder_.GetInsertBlock();
    }

    builder_.SetInsertPoint(mergeBB);
    lastValue_ = nullptr;
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
