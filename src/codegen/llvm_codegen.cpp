#include "llvm_codegen.hpp"
#include "class_arc_emitter.hpp"
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
namespace {
bool isStringType(const std::shared_ptr<zir::Type> &type) {
  return type && type->getKind() == zir::TypeKind::Record &&
         static_cast<zir::RecordType *>(type.get())->getName() == "String";
}

bool isVariadicViewType(const std::shared_ptr<zir::Type> &type) {
  return type && type->getKind() == zir::TypeKind::Record &&
         static_cast<zir::RecordType *>(type.get())
                 ->getName()
                 .rfind("__zap_varargs_", 0) == 0;
}
} // namespace
LLVMCodeGen::LLVMCodeGen()
    : builder_(ctx_), evaluateAsAddr_(false),
      arcEmitter_(std::make_unique<ClassArcEmitter>(*this)), nextStringId_(0) {
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
}

LLVMCodeGen::~LLVMCodeGen() = default;

llvm::Constant *LLVMCodeGen::getOrCreateGlobalString(const std::string &str,
                                                     std::string &globalName) {
  globalName = ".str." + std::to_string(nextStringId_++);

  auto *arrayTy = llvm::ArrayType::get(llvm::Type::getInt8Ty(ctx_),
                                       static_cast<unsigned>(str.size() + 1));
  auto *constArray = llvm::ConstantDataArray::getString(ctx_, str, true);

  auto *gv = new llvm::GlobalVariable(*module_, arrayTy, /*isConstant=*/true,
                                      llvm::GlobalValue::PrivateLinkage,
                                      constArray, globalName);

  auto *zero32 = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
  llvm::Constant *indices[] = {zero32, zero32};
  auto *gep =
      llvm::ConstantExpr::getInBoundsGetElementPtr(arrayTy, gv, indices);
  auto *ptrTy = llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(ctx_));
  auto *ptr = llvm::ConstantExpr::getBitCast(gep, ptrTy);
  return ptr;
}

void LLVMCodeGen::initializeModule() {
  module_ = std::make_unique<llvm::Module>("zap_module", ctx_);
  auto targetTripleStr = llvm::sys::getDefaultTargetTriple();
  llvm::Triple triple(targetTripleStr);
  module_->setTargetTriple(triple);
  std::string error;
  const auto *target =
      llvm::TargetRegistry::lookupTarget(targetTripleStr, error);
  if (target) {
    llvm::TargetOptions opts;
    if (auto *tm = target->createTargetMachine(triple, "generic", "", opts,
                                               llvm::Reloc::PIC_)) {
      module_->setDataLayout(tm->createDataLayout());
      delete tm;
    }
  }
}

void LLVMCodeGen::generate(sema::BoundRootNode &root) {
  initializeModule();
  ensureArcSupport(root);
  root.accept(*this);
}

void LLVMCodeGen::generate(const zir::Module &module) {
  initializeModule();

  functionMap_.clear();
  zirFunctionMap_.clear();
  globalValues_.clear();
  zirValueMap_.clear();
  zirBlockMap_.clear();

  for (const auto &type : module.getTypes()) {
    if (type->getKind() == zir::TypeKind::Record ||
        type->getKind() == zir::TypeKind::Class) {
      toLLVMType(*type);
    }
  }

  for (const auto &global : module.getGlobals()) {
    llvm::Constant *initializer = nullptr;
    if (global->getInitializer() &&
        global->getInitializer()->getKind() == zir::ValueKind::Constant) {
      initializer = lowerZIRConstant(
          static_cast<const zir::Constant &>(*global->getInitializer()));
    }
    if (!initializer) {
      initializer = llvm::Constant::getNullValue(toLLVMType(*global->getValueType()));
    }

    auto *gv = new llvm::GlobalVariable(
        *module_, toLLVMType(*global->getValueType()), global->isConstant(),
        llvm::GlobalVariable::ExternalLinkage, initializer,
        global->getLinkName());
    globalValues_[global->getLinkName()] = gv;
  }

  for (const auto &func : module.getExternalFunctions()) {
    zirFunctionMap_[func->name] = func.get();
    declareZIRFunction(*func, true);
  }
  for (const auto &func : module.getFunctions()) {
    zirFunctionMap_[func->name] = func.get();
    declareZIRFunction(*func, false);
    auto *llvmFn = functionMap_.at(func->name);
    if (func->isDestructor && !func->ownerTypeName.empty()) {
      classDestructorFns_[func->ownerTypeName] = llvmFn;
    }
    if (func->vtableSlot >= 0 && !func->ownerTypeName.empty()) {
      classVirtualMethodFns_[func->ownerTypeName][func->vtableSlot] = llvmFn;
    }
  }
  for (const auto &type : module.getTypes()) {
    if (type->getKind() == zir::TypeKind::Class) {
      ensureClassArcSupport(std::static_pointer_cast<zir::ClassType>(type));
    }
  }
  for (const auto &func : module.getFunctions()) {
    emitZIRFunction(*func);
  }
}

void LLVMCodeGen::printIR(llvm::raw_ostream &os) const {
  if (module_)
    module_->print(os, nullptr);
}

bool LLVMCodeGen::emitObjectFile(const std::string &path) {
  auto targetTripleStr = llvm::sys::getDefaultTargetTriple();
  llvm::Triple triple(targetTripleStr);
  module_->setTargetTriple(triple);
  std::string error;
  const auto *target =
      llvm::TargetRegistry::lookupTarget(targetTripleStr, error);
  if (!target) {
    llvm::errs() << "Target lookup failed: " << error << "\n";
    return false;
  }

  llvm::TargetOptions opts;
  auto *tm = target->createTargetMachine(triple, "generic", "", opts,
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

  // TODO: Improve handling of verifying the module.
  bool is_broken = llvm::verifyModule(*module_, &llvm::errs());

  if (!is_broken)
    pm.run(*module_);
  dest.flush();
  delete tm;
  return !is_broken;
}

llvm::Type *LLVMCodeGen::toLLVMType(const zir::Type &ty) {
  switch (ty.getKind()) {
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
  case zir::TypeKind::Pointer: {
    const auto &pt = static_cast<const zir::PointerType &>(ty);
    auto *baseTy = toLLVMType(*pt.getBaseType());
    if (baseTy->isVoidTy()) {
      baseTy = llvm::Type::getInt8Ty(ctx_);
    }
    return llvm::PointerType::getUnqual(baseTy);
  }
  case zir::TypeKind::NullPtr:
    return llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(ctx_));
  case zir::TypeKind::Enum:
    return llvm::Type::getInt64Ty(ctx_);
  case zir::TypeKind::Record: {
    const auto &rt = static_cast<const zir::RecordType &>(ty);
    auto it = structCache_.find(rt.getCodegenName());
    if (it != structCache_.end())
      return it->second;

    if (rt.getName() == "String") {
      auto *structTy = llvm::StructType::create(ctx_, rt.getCodegenName());
      structCache_[rt.getCodegenName()] = structTy;
      std::vector<llvm::Type *> fieldTypes;
      fieldTypes.push_back(
          llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(ctx_)));
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
  case zir::TypeKind::Class: {
    const auto &ct = static_cast<const zir::ClassType &>(ty);
    std::string objectName = ct.getCodegenName() + ".obj";
    auto it = structCache_.find(objectName);
    llvm::StructType *objectTy = nullptr;
    if (it != structCache_.end()) {
      objectTy = it->second;
    } else {
      objectTy = llvm::StructType::create(ctx_, objectName);
      structCache_[objectName] = objectTy;
      std::vector<llvm::Type *> fieldTypes;
      fieldTypes.push_back(llvm::Type::getInt64Ty(ctx_));
      fieldTypes.push_back(llvm::Type::getInt64Ty(ctx_));
      fieldTypes.push_back(llvm::Type::getInt8Ty(ctx_));
      fieldTypes.push_back(llvm::Type::getInt8Ty(ctx_));
      fieldTypes.push_back(
          llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(ctx_)));
      fieldTypes.push_back(
          llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(ctx_)));
      fieldTypes.push_back(
          llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(ctx_)));
      fieldTypes.push_back(llvm::PointerType::getUnqual(
          llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(ctx_))));
      for (const auto &f : ct.getFields()) {
        fieldTypes.push_back(toLLVMType(*f.type));
      }
      objectTy->setBody(fieldTypes);
    }
    return llvm::PointerType::getUnqual(objectTy);
  }
  case zir::TypeKind::Array: {
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
                               bool injectMainProcessArgs) {
  std::vector<llvm::Type *> paramTypes;
  if (injectMainProcessArgs) {
    paramTypes.push_back(llvm::Type::getInt32Ty(ctx_));
    paramTypes.push_back(llvm::PointerType::getUnqual(
        llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(ctx_))));
  }
  for (const auto &param : sym.parameters) {
    if (param->is_variadic_pack) {
      paramTypes.push_back(llvm::Type::getInt32Ty(ctx_));
      paramTypes.push_back(llvm::PointerType::getUnqual(
          toLLVMType(*param->variadic_element_type)));
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

llvm::FunctionType *LLVMCodeGen::buildFunctionType(const zir::Function &fn) {
  std::vector<llvm::Type *> paramTypes;
  if (fn.name == "main") {
    paramTypes.push_back(llvm::Type::getInt32Ty(ctx_));
    paramTypes.push_back(llvm::PointerType::getUnqual(
        llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(ctx_))));
  }
  for (const auto &param : fn.getArguments()) {
    if (param->isVariadicPack()) {
      paramTypes.push_back(llvm::Type::getInt32Ty(ctx_));
      paramTypes.push_back(llvm::PointerType::getUnqual(
          toLLVMType(*param->getVariadicElementType())));
      continue;
    }
    paramTypes.push_back(toLLVMType(*param->getType()));
  }
  return llvm::FunctionType::get(toLLVMType(*fn.getReturnType()), paramTypes,
                                 fn.isCVariadic);
}

llvm::AllocaInst *LLVMCodeGen::createEntryAlloca(llvm::Function *fn,
                                                 const std::string &name,
                                                 llvm::Type *ty) {
  llvm::IRBuilder<> entry(&fn->getEntryBlock(), fn->getEntryBlock().begin());
  return entry.CreateAlloca(ty, nullptr, name);
}

void LLVMCodeGen::declareZIRFunction(const zir::Function &fn, bool isExternal) {
  auto *ft = buildFunctionType(fn);
  auto linkage = llvm::Function::ExternalLinkage;
  auto *llvmFn = llvm::Function::Create(ft, linkage, fn.name, *module_);
  auto argIt = llvmFn->arg_begin();
  for (const auto &arg : fn.getArguments()) {
    if (arg->isVariadicPack()) {
      argIt->setName(arg->getRawName() + ".count");
      ++argIt;
      argIt->setName(arg->getRawName() + ".data");
      ++argIt;
      continue;
    }
    argIt->setName(arg->getRawName());
    ++argIt;
  }
  functionMap_[fn.name] = llvmFn;
  (void)isExternal;
}

llvm::Constant *LLVMCodeGen::lowerZIRConstant(const zir::Constant &constant) {
  if (constant.getType()->getKind() == zir::TypeKind::Record) {
    const auto &rt =
        static_cast<const zir::RecordType &>(*constant.getType());
    if (rt.getName() == "String") {
      std::string gname;
      auto *ptrConst = getOrCreateGlobalString(constant.getLiteral(), gname);
      auto *lenConst = llvm::ConstantInt::get(
          llvm::Type::getInt64Ty(ctx_),
          static_cast<uint64_t>(constant.getLiteral().size()));
      auto *structTy =
          static_cast<llvm::StructType *>(toLLVMType(*constant.getType()));
      return llvm::ConstantStruct::get(structTy, {ptrConst, lenConst});
    }
  }

  auto *ty = toLLVMType(*constant.getType());
  const auto &literal = constant.getLiteral();
  if (ty->isIntegerTy(1)) {
    return llvm::ConstantInt::get(ty, literal == "true" ? 1 : 0);
  }
  if (ty->isIntegerTy(8)) {
    if (literal == "null") {
      return llvm::ConstantInt::get(ty, 0, false);
    }
    int64_t code = 0;
    if (!literal.empty()) {
      if (literal.size() >= 2 && literal[0] == '\\') {
        switch (literal[1]) {
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
          code = static_cast<unsigned char>(literal[1]);
          break;
        }
      } else {
        code = static_cast<unsigned char>(literal[0]);
      }
    }
    return llvm::ConstantInt::get(ty, code, false);
  }
  if (ty->isIntegerTy()) {
    if (literal == "null") {
      return llvm::ConstantInt::get(ty, 0, false);
    }
    if (constant.getType()->isUnsigned()) {
      return llvm::ConstantInt::get(ty, std::stoull(literal), false);
    }
    return llvm::ConstantInt::get(ty, std::stoll(literal), true);
  }
  if (ty->isFloatTy()) {
    return llvm::ConstantFP::get(ty, std::stof(literal));
  }
  if (ty->isDoubleTy()) {
    return llvm::ConstantFP::get(ty, std::stod(literal));
  }
  if (ty->isPointerTy()) {
    return llvm::ConstantPointerNull::get(
        llvm::cast<llvm::PointerType>(ty));
  }
  return llvm::Constant::getNullValue(ty);
}

llvm::Value *LLVMCodeGen::lowerZIRValue(
    const std::shared_ptr<zir::Value> &value) {
  if (!value) {
    return nullptr;
  }
  if (value->getKind() == zir::ValueKind::Constant) {
    return lowerZIRConstant(
        static_cast<const zir::Constant &>(*value));
  }
  if (value->getKind() == zir::ValueKind::Global) {
    return globalValues_.at(
        static_cast<const zir::Global &>(*value).getLinkName());
  }

  auto it = zirValueMap_.find(value.get());
  if (it == zirValueMap_.end()) {
    throw std::runtime_error("unmapped ZIR value: " + value->getName());
  }
  return it->second;
}

llvm::Value *LLVMCodeGen::lowerZIRRValue(
    const std::shared_ptr<zir::Value> &value) {
  auto *raw = lowerZIRValue(value);
  if (!value) {
    return raw;
  }

  auto *expectedTy = toLLVMType(*value->getType());
  if (raw->getType() == expectedTy) {
    return raw;
  }

  auto *ptrTy = llvm::dyn_cast<llvm::PointerType>(raw->getType());
  if (!ptrTy) {
    return raw;
  }
  if (value->getType()->getKind() == zir::TypeKind::Pointer ||
      value->getType()->getKind() == zir::TypeKind::Class ||
      value->getType()->getKind() == zir::TypeKind::NullPtr) {
    return raw;
  }
  return builder_.CreateLoad(expectedTy, raw, "zir.rvalue");
}

llvm::Value *LLVMCodeGen::lowerZIRCast(
    llvm::Value *src, const std::shared_ptr<zir::Type> &sourceType,
    const std::shared_ptr<zir::Type> &targetType) {
  auto *srcTy = src->getType();
  auto *destTy = toLLVMType(*targetType);

  if (isStringType(sourceType) && destTy->isPointerTy()) {
    auto *ptr = builder_.CreateExtractValue(src, {0}, "zir.cast.str.ptr");
    return ptr->getType() == destTy ? ptr : builder_.CreateBitCast(ptr, destTy);
  }
  if (srcTy == destTy) {
    return src;
  }
  if (srcTy->isPointerTy() && destTy->isPointerTy()) {
    return builder_.CreateBitCast(src, destTy);
  }
  if (srcTy->isPointerTy() && destTy->isIntegerTy()) {
    return builder_.CreatePtrToInt(src, destTy);
  }
  if (srcTy->isIntegerTy() && destTy->isPointerTy()) {
    return builder_.CreateIntToPtr(src, destTy);
  }
  if (srcTy->isIntegerTy() && destTy->isIntegerTy()) {
    unsigned srcBits = srcTy->getIntegerBitWidth();
    unsigned destBits = destTy->getIntegerBitWidth();
    if (destBits > srcBits) {
      return sourceType->isUnsigned() ? builder_.CreateZExt(src, destTy)
                                      : builder_.CreateSExt(src, destTy);
    }
    if (destBits < srcBits) {
      return builder_.CreateTrunc(src, destTy);
    }
    return src;
  }
  if (srcTy->isIntegerTy() && destTy->isFloatingPointTy()) {
    return sourceType->isUnsigned() ? builder_.CreateUIToFP(src, destTy)
                                    : builder_.CreateSIToFP(src, destTy);
  }
  if (srcTy->isFloatingPointTy() && destTy->isIntegerTy()) {
    return targetType->isUnsigned() ? builder_.CreateFPToUI(src, destTy)
                                    : builder_.CreateFPToSI(src, destTy);
  }
  if (srcTy->isFloatingPointTy() && destTy->isFloatingPointTy()) {
    if (srcTy->getPrimitiveSizeInBits() < destTy->getPrimitiveSizeInBits()) {
      return builder_.CreateFPExt(src, destTy);
    }
    if (srcTy->getPrimitiveSizeInBits() > destTy->getPrimitiveSizeInBits()) {
      return builder_.CreateFPTrunc(src, destTy);
    }
    return src;
  }
  return builder_.CreateBitCast(src, destTy);
}

llvm::Value *LLVMCodeGen::emitStringConcat(
    llvm::Value *lhs, llvm::Value *rhs, const std::shared_ptr<zir::Type> &lhsType,
    const std::shared_ptr<zir::Type> &rhsType,
    const std::shared_ptr<zir::Type> &resultType) {
  auto *i8Ty = llvm::Type::getInt8Ty(ctx_);
  auto *i64Ty = llvm::Type::getInt64Ty(ctx_);

  llvm::Value *lhsPtr = nullptr;
  llvm::Value *lhsLen = nullptr;
  llvm::Value *rhsPtr = nullptr;
  llvm::Value *rhsLen = nullptr;

  if (isStringType(lhsType)) {
    lhsPtr = builder_.CreateExtractValue(lhs, {0});
    lhsLen = builder_.CreateExtractValue(lhs, {1});
  } else if (lhsType && lhsType->getKind() == zir::TypeKind::Char) {
    auto *buf = createEntryAlloca(currentFn_, "zir_char_buf_l", i8Ty);
    builder_.CreateStore(lhs, buf);
    lhsPtr = buf;
    lhsLen = llvm::ConstantInt::get(i64Ty, 1);
  }

  if (isStringType(rhsType)) {
    rhsPtr = builder_.CreateExtractValue(rhs, {0});
    rhsLen = builder_.CreateExtractValue(rhs, {1});
  } else if (rhsType && rhsType->getKind() == zir::TypeKind::Char) {
    auto *buf = createEntryAlloca(currentFn_, "zir_char_buf_r", i8Ty);
    builder_.CreateStore(rhs, buf);
    rhsPtr = buf;
    rhsLen = llvm::ConstantInt::get(i64Ty, 1);
  }

  if (functionMap_.count("string_concat_ptrlen") == 0) {
    std::vector<llvm::Type *> params = {
        llvm::PointerType::getUnqual(i8Ty), i64Ty,
        llvm::PointerType::getUnqual(i8Ty), i64Ty};
    auto *ft = llvm::FunctionType::get(llvm::PointerType::getUnqual(i8Ty),
                                       params, false);
    auto *fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                                      "string_concat_ptrlen", *module_);
    functionMap_["string_concat_ptrlen"] = fn;
  }

  auto *concatFn = functionMap_.at("string_concat_ptrlen");
  auto *call = builder_.CreateCall(concatFn, {lhsPtr, lhsLen, rhsPtr, rhsLen});
  auto *sumLen = builder_.CreateAdd(lhsLen, rhsLen);

  auto *structTy = static_cast<llvm::StructType *>(toLLVMType(*resultType));
  llvm::Value *res = llvm::UndefValue::get(structTy);
  res = builder_.CreateInsertValue(res, call, {0});
  res = builder_.CreateInsertValue(res, sumLen, {1});
  return res;
}

void LLVMCodeGen::emitZIRInstruction(const zir::Instruction &inst) {
  using namespace zir;

  switch (inst.getOpCode()) {
  case OpCode::Alloca: {
    const auto &allocaInst = static_cast<const AllocaInst &>(inst);
    bool isParamSpill =
        currentZIRFunction_ &&
        zirParamSpillIndex_ < currentZIRFunction_->getArguments().size();
    bool isBorrowedSelf = false;
    if (isParamSpill) {
      isBorrowedSelf =
          !currentZIRFunction_->ownerTypeName.empty() &&
          currentZIRFunction_->getArguments()[zirParamSpillIndex_]->getRawName() ==
              "self";
    }
    auto *alloca = createEntryAlloca(currentFn_,
                                     static_cast<const Register &>(
                                         *allocaInst.getResult()).getRawName(),
                                     toLLVMType(*allocaInst.getAllocatedType()));
    zirValueMap_[allocaInst.getResult().get()] = alloca;
    if (isClassType(allocaInst.getAllocatedType())) {
      builder_.CreateStore(
          llvm::Constant::getNullValue(toLLVMType(*allocaInst.getAllocatedType())),
          alloca);
      if (isParamSpill) {
        zirClassParamAllocas_.insert(allocaInst.getResult().get());
        zirPendingClassParamInitAllocas_.insert(allocaInst.getResult().get());
      }
      if (!isBorrowedSelf) {
        zirFunctionClassLocals_.push_back({allocaInst.getAllocatedType(), alloca});
      }
    }
    if (isParamSpill) {
      ++zirParamSpillIndex_;
    }
    return;
  }
  case OpCode::Load: {
    const auto &loadInst = static_cast<const LoadInst &>(inst);
    auto *src = lowerZIRValue(loadInst.getSource());
    auto *value = builder_.CreateLoad(toLLVMType(*loadInst.getResult()->getType()),
                                      src,
                                      static_cast<const Register &>(
                                          *loadInst.getResult()).getRawName());
    zirValueMap_[loadInst.getResult().get()] = value;
    return;
  }
  case OpCode::Store: {
    const auto &storeInst = static_cast<const StoreInst &>(inst);
    auto *dst = lowerZIRValue(storeInst.getDestination());
    auto *src = lowerZIRRValue(storeInst.getSource());
    auto dstType = storeInst.getDestination()->getType();
    auto ptrType = std::dynamic_pointer_cast<zir::PointerType>(dstType);
    auto valueType = ptrType ? ptrType->getBaseType() : nullptr;
    if (valueType && isClassType(valueType)) {
      if (zirPendingClassParamInitAllocas_.count(storeInst.getDestination().get()) >
          0) {
        builder_.CreateStore(src, dst);
        zirPendingClassParamInitAllocas_.erase(storeInst.getDestination().get());
      } else {
        bool valueIsOwned =
            zirOwnedClassValues_.count(storeInst.getSource().get()) > 0;
        emitStoreWithArc(dst, src, valueType, valueIsOwned);
        if (valueIsOwned) {
          zirOwnedClassValues_.erase(storeInst.getSource().get());
        }
      }
    } else {
      builder_.CreateStore(src, dst);
    }
    return;
  }
  case OpCode::Add:
  case OpCode::Sub:
  case OpCode::Mul:
  case OpCode::SDiv:
  case OpCode::UDiv:
  case OpCode::SRem:
  case OpCode::URem: {
    const auto &binaryInst = static_cast<const BinaryInst &>(inst);
    auto *lhs = lowerZIRRValue(binaryInst.getLhs());
    auto *rhs = lowerZIRRValue(binaryInst.getRhs());
    llvm::Value *result = nullptr;
    bool lhsIsPointer = binaryInst.getLhs()->getType()->getKind() ==
                        zir::TypeKind::Pointer;
    bool rhsIsPointer = binaryInst.getRhs()->getType()->getKind() ==
                        zir::TypeKind::Pointer;
    switch (inst.getOpCode()) {
    case OpCode::Add:
      if (isStringType(binaryInst.getLhs()->getType()) ||
          isStringType(binaryInst.getRhs()->getType()) ||
          binaryInst.getLhs()->getType()->getKind() == zir::TypeKind::Char ||
          binaryInst.getRhs()->getType()->getKind() == zir::TypeKind::Char) {
        result = emitStringConcat(lhs, rhs, binaryInst.getLhs()->getType(),
                                  binaryInst.getRhs()->getType(),
                                  binaryInst.getResult()->getType());
      } else if (lhsIsPointer || rhsIsPointer) {
        llvm::Value *pointerValue = lhsIsPointer ? lhs : rhs;
        llvm::Value *offsetValue = lhsIsPointer ? rhs : lhs;
        auto pointerType = std::static_pointer_cast<zir::PointerType>(
            lhsIsPointer ? binaryInst.getLhs()->getType()
                         : binaryInst.getRhs()->getType());
        auto *elemTy = toLLVMType(*pointerType->getBaseType());
        auto *indexTy = llvm::Type::getInt64Ty(ctx_);
        auto *index =
            builder_.CreateIntCast(offsetValue, indexTy, /*isSigned=*/true);
        result = builder_.CreateInBoundsGEP(elemTy, pointerValue, index);
      } else {
        result = builder_.CreateAdd(lhs, rhs);
      }
      break;
    case OpCode::Sub:
      if (lhsIsPointer && rhsIsPointer) {
        auto pointerType =
            std::static_pointer_cast<zir::PointerType>(binaryInst.getLhs()->getType());
        auto *elemTy = toLLVMType(*pointerType->getBaseType());
        auto *i64Ty = llvm::Type::getInt64Ty(ctx_);
        auto *lhsInt = builder_.CreatePtrToInt(lhs, i64Ty);
        auto *rhsInt = builder_.CreatePtrToInt(rhs, i64Ty);
        auto *bytes = builder_.CreateSub(lhsInt, rhsInt);
        llvm::Value *elemSize = llvm::ConstantExpr::getSizeOf(elemTy);
        if (elemSize->getType() != i64Ty) {
          elemSize = builder_.CreateIntCast(elemSize, i64Ty, false);
        }
        result = builder_.CreateSDiv(bytes, elemSize);
      } else if (lhsIsPointer) {
        auto pointerType =
            std::static_pointer_cast<zir::PointerType>(binaryInst.getLhs()->getType());
        auto *elemTy = toLLVMType(*pointerType->getBaseType());
        auto *indexTy = llvm::Type::getInt64Ty(ctx_);
        auto *index = builder_.CreateIntCast(rhs, indexTy, /*isSigned=*/true);
        index = builder_.CreateNeg(index);
        result = builder_.CreateInBoundsGEP(elemTy, lhs, index);
      } else {
        result = builder_.CreateSub(lhs, rhs);
      }
      break;
    case OpCode::Mul:
      result = builder_.CreateMul(lhs, rhs);
      break;
    case OpCode::SDiv:
      result = builder_.CreateSDiv(lhs, rhs);
      break;
    case OpCode::UDiv:
      result = builder_.CreateUDiv(lhs, rhs);
      break;
    case OpCode::SRem:
      result = builder_.CreateSRem(lhs, rhs);
      break;
    case OpCode::URem:
      result = builder_.CreateURem(lhs, rhs);
      break;
    default:
      break;
    }
    zirValueMap_[binaryInst.getResult().get()] = result;
    return;
  }
  case OpCode::Cmp: {
    const auto &cmpInst = static_cast<const CmpInst &>(inst);
    auto *lhs = lowerZIRRValue(cmpInst.getLhs());
    auto *rhs = lowerZIRRValue(cmpInst.getRhs());
    llvm::Value *result = nullptr;
    const auto &pred = cmpInst.getPredicate();
    auto *lhsTy = lhs->getType();
    auto *rhsTy = rhs->getType();
    if (lhsTy != rhsTy) {
      throw std::runtime_error("ZIR cmp operand type mismatch: " +
                               cmpInst.getLhs()->getTypeName() + " vs " +
                               cmpInst.getRhs()->getTypeName());
    }
    if (lhsTy->isFloatingPointTy()) {
      if (pred == "eq")
        result = builder_.CreateFCmpOEQ(lhs, rhs);
      else if (pred == "ne")
        result = builder_.CreateFCmpONE(lhs, rhs);
      else if (pred == "slt" || pred == "ult")
        result = builder_.CreateFCmpOLT(lhs, rhs);
      else if (pred == "sle" || pred == "ule")
        result = builder_.CreateFCmpOLE(lhs, rhs);
      else if (pred == "sgt" || pred == "ugt")
        result = builder_.CreateFCmpOGT(lhs, rhs);
      else if (pred == "sge" || pred == "uge")
        result = builder_.CreateFCmpOGE(lhs, rhs);
    } else if (lhsTy->isIntOrIntVectorTy() || lhsTy->isPtrOrPtrVectorTy()) {
      if (pred == "eq")
        result = builder_.CreateICmpEQ(lhs, rhs);
      else if (pred == "ne")
        result = builder_.CreateICmpNE(lhs, rhs);
      else if (pred == "slt")
        result = builder_.CreateICmpSLT(lhs, rhs);
      else if (pred == "sle")
        result = builder_.CreateICmpSLE(lhs, rhs);
      else if (pred == "sgt")
        result = builder_.CreateICmpSGT(lhs, rhs);
      else if (pred == "sge")
        result = builder_.CreateICmpSGE(lhs, rhs);
      else if (pred == "ult")
        result = builder_.CreateICmpULT(lhs, rhs);
      else if (pred == "ule")
        result = builder_.CreateICmpULE(lhs, rhs);
      else if (pred == "ugt")
        result = builder_.CreateICmpUGT(lhs, rhs);
      else if (pred == "uge")
        result = builder_.CreateICmpUGE(lhs, rhs);
    }
    if (!result) {
      throw std::runtime_error("unsupported ZIR cmp predicate/type: " + pred +
                               " on " + cmpInst.getLhs()->getTypeName());
    }
    zirValueMap_[cmpInst.getResult().get()] = result;
    return;
  }
  case OpCode::Br: {
    const auto &branchInst = static_cast<const BranchInst &>(inst);
    builder_.CreateBr(zirBlockMap_.at(branchInst.getTarget()));
    return;
  }
  case OpCode::CondBr: {
    const auto &branchInst = static_cast<const CondBranchInst &>(inst);
    builder_.CreateCondBr(lowerZIRRValue(branchInst.getCondition()),
                          zirBlockMap_.at(branchInst.getTrueLabel()),
                          zirBlockMap_.at(branchInst.getFalseLabel()));
    return;
  }
  case OpCode::Ret: {
    const auto &returnInst = static_cast<const ReturnInst &>(inst);
    if (returnInst.getValue()) {
      auto *retValue = lowerZIRRValue(returnInst.getValue());
      auto retType = returnInst.getValue()->getType();
      if (isClassType(retType) &&
          zirOwnedClassValues_.count(returnInst.getValue().get()) == 0) {
        emitRetainIfNeeded(retValue, retType);
      }
      for (auto it = zirFunctionClassLocals_.rbegin();
           it != zirFunctionClassLocals_.rend(); ++it) {
        auto *value = builder_.CreateLoad(toLLVMType(*it->first), it->second,
                                          "zir.arc.ret.release");
        if (isWeakClassType(it->first)) {
          emitReleaseWeakIfNeeded(value, it->first);
        } else {
          emitReleaseIfNeeded(value, it->first);
        }
      }
      builder_.CreateRet(retValue);
    } else {
      for (auto it = zirFunctionClassLocals_.rbegin();
           it != zirFunctionClassLocals_.rend(); ++it) {
        auto *value = builder_.CreateLoad(toLLVMType(*it->first), it->second,
                                          "zir.arc.ret.release");
        if (isWeakClassType(it->first)) {
          emitReleaseWeakIfNeeded(value, it->first);
        } else {
          emitReleaseIfNeeded(value, it->first);
        }
      }
      builder_.CreateRetVoid();
    }
    return;
  }
  case OpCode::Call: {
    const auto &callInst = static_cast<const CallInst &>(inst);
    std::vector<llvm::Value *> args;
    auto *callee = functionMap_.at(callInst.getFunctionName());
    auto *calleeTy = callee->getFunctionType();
    auto zirIt = zirFunctionMap_.find(callInst.getFunctionName());
    size_t fixedParamCount = callInst.getArguments().size();
    bool hasVariadicParameter = false;
    std::shared_ptr<zir::Type> variadicElementType = nullptr;
    bool isCVariadic = calleeTy->isVarArg();
    if (zirIt != zirFunctionMap_.end()) {
      fixedParamCount = 0;
      for (const auto &param : zirIt->second->getArguments()) {
        if (param->isVariadicPack()) {
          hasVariadicParameter = true;
          variadicElementType = param->getVariadicElementType();
          break;
        }
        ++fixedParamCount;
      }
      isCVariadic = zirIt->second->isCVariadic;
    }
    for (size_t i = 0; i < callInst.getArguments().size(); ++i) {
      bool isRef =
          i < callInst.getArgumentIsRef().size() && callInst.getArgumentIsRef()[i];
      auto *arg = isRef ? lowerZIRValue(callInst.getArguments()[i])
                        : lowerZIRRValue(callInst.getArguments()[i]);
      std::shared_ptr<zir::Type> calleeParamType = nullptr;
      if (zirIt != zirFunctionMap_.end() && i < fixedParamCount &&
          i < zirIt->second->getArguments().size()) {
        calleeParamType = zirIt->second->getArguments()[i]->getType();
      }
      llvm::Type *paramTy = nullptr;
      if (i < fixedParamCount && static_cast<unsigned>(i) < calleeTy->getNumParams()) {
        paramTy = calleeTy->getParamType(static_cast<unsigned>(i));
      }
      if (paramTy && arg->getType() != paramTy) {
        auto *argPtrTy = llvm::dyn_cast<llvm::PointerType>(arg->getType());
        if (argPtrTy && !paramTy->isPointerTy()) {
          arg = builder_.CreateLoad(paramTy, arg, "zir.call.arg");
        }
      }
      bool isBorrowedSelfArg =
          zirIt != zirFunctionMap_.end() && i == 0 &&
          !zirIt->second->ownerTypeName.empty() &&
          i < zirIt->second->getArguments().size() &&
          zirIt->second->getArguments()[i]->getRawName() == "self";
      if (!isRef && i < fixedParamCount && !isBorrowedSelfArg &&
          !(calleeParamType && isWeakClassType(calleeParamType)) &&
          isClassType(callInst.getArguments()[i]->getType()) &&
          zirOwnedClassValues_.count(callInst.getArguments()[i].get()) == 0) {
        emitRetainIfNeeded(arg, callInst.getArguments()[i]->getType());
      }
      args.push_back(arg);
    }
    if (hasVariadicParameter) {
      auto *elemTy = toLLVMType(*variadicElementType);
      auto *elemPtrTy = llvm::PointerType::getUnqual(elemTy);
      size_t explicitVariadicCount = callInst.getArguments().size() - fixedParamCount;

      llvm::Value *forwardedCount =
          llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
      llvm::Value *forwardedData = llvm::ConstantPointerNull::get(elemPtrTy);
      if (callInst.getVariadicPack()) {
        auto *packValue = lowerZIRRValue(callInst.getVariadicPack());
        forwardedData =
            builder_.CreateExtractValue(packValue, {0}, "varargs.forward.data");
        forwardedCount =
            builder_.CreateExtractValue(packValue, {1}, "varargs.forward.len");
        if (forwardedCount->getType() != llvm::Type::getInt32Ty(ctx_)) {
          forwardedCount = builder_.CreateIntCast(
              forwardedCount, llvm::Type::getInt32Ty(ctx_), /*isSigned=*/true);
        }
      }

      llvm::Value *explicitCount =
          llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_),
                                 static_cast<uint64_t>(explicitVariadicCount));

      if (explicitVariadicCount == 0 && !callInst.getVariadicPack()) {
        args.push_back(explicitCount);
        args.push_back(llvm::ConstantPointerNull::get(elemPtrTy));
      } else if (explicitVariadicCount == 0) {
        args.push_back(forwardedCount);
        args.push_back(forwardedData);
      } else {
        llvm::Value *totalCount = explicitCount;
        if (callInst.getVariadicPack()) {
          totalCount =
              builder_.CreateAdd(explicitCount, forwardedCount, "varargs.total");
        }

        auto *buffer = builder_.CreateAlloca(elemTy, totalCount, "varargs.buf");
        for (size_t i = 0; i < explicitVariadicCount; ++i) {
          auto *dst = builder_.CreateInBoundsGEP(
              elemTy, buffer,
              llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_),
                                     static_cast<uint64_t>(i)));
          builder_.CreateStore(args[fixedParamCount + i], dst);
        }

        if (callInst.getVariadicPack()) {
          auto *copyCondBB =
              llvm::BasicBlock::Create(ctx_, "varargs.copy.cond", currentFn_);
          auto *copyBodyBB =
              llvm::BasicBlock::Create(ctx_, "varargs.copy.body", currentFn_);
          auto *copyDoneBB =
              llvm::BasicBlock::Create(ctx_, "varargs.copy.done", currentFn_);
          auto *copyPreheaderBB = builder_.GetInsertBlock();

          builder_.CreateBr(copyCondBB);
          builder_.SetInsertPoint(copyCondBB);
          auto *indexPhi =
              builder_.CreatePHI(llvm::Type::getInt32Ty(ctx_), 2, "varargs.i");
          indexPhi->addIncoming(
              llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0),
              copyPreheaderBB);
          auto *cond =
              builder_.CreateICmpULT(indexPhi, forwardedCount, "varargs.cond");
          builder_.CreateCondBr(cond, copyBodyBB, copyDoneBB);

          builder_.SetInsertPoint(copyBodyBB);
          auto *src = builder_.CreateInBoundsGEP(elemTy, forwardedData, indexPhi,
                                                 "varargs.src");
          auto *srcVal = builder_.CreateLoad(elemTy, src, "varargs.load");
          auto *dstIndex = builder_.CreateAdd(indexPhi, explicitCount);
          auto *dst = builder_.CreateInBoundsGEP(elemTy, buffer, dstIndex,
                                                 "varargs.dst");
          builder_.CreateStore(srcVal, dst);
          auto *next = builder_.CreateAdd(
              indexPhi, llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 1));
          builder_.CreateBr(copyCondBB);
          indexPhi->addIncoming(next, copyBodyBB);

          builder_.SetInsertPoint(copyDoneBB);
        }

        args.erase(args.begin() + static_cast<std::ptrdiff_t>(fixedParamCount),
                   args.end());
        args.push_back(totalCount);
        args.push_back(buffer);
      }
    } else if (isCVariadic) {
      // Extra arguments are passed through unchanged after the fixed params.
    }
    llvm::Value *call = nullptr;
    if (zirIt != zirFunctionMap_.end() && zirIt->second->vtableSlot >= 0 &&
        !args.empty()) {
      auto receiverType = callInst.getArguments().front()->getType();
      std::shared_ptr<zir::ClassType> classType = nullptr;
      if (receiverType->getKind() == zir::TypeKind::Class) {
        classType = std::static_pointer_cast<zir::ClassType>(receiverType);
      } else if (receiverType->getKind() == zir::TypeKind::Pointer) {
        auto baseType =
            std::static_pointer_cast<zir::PointerType>(receiverType)
                ->getBaseType();
        if (baseType->getKind() == zir::TypeKind::Class) {
          classType = std::static_pointer_cast<zir::ClassType>(baseType);
        }
      }

      if (classType) {
        auto *objectTy = structCache_.at(classType->getCodegenName() + ".obj");
        auto *selfPtr = builder_.CreateBitCast(
            args[0], llvm::PointerType::getUnqual(objectTy), "zir.method.self");
        auto *vtableAddr = builder_.CreateStructGEP(
            objectTy, selfPtr, 7, "zir.method.vtable.addr");
        auto *i8PtrTy =
            llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(ctx_));
        auto *vtablePtrTy = llvm::PointerType::getUnqual(i8PtrTy);
        auto *vtablePtr = builder_.CreateLoad(vtablePtrTy, vtableAddr,
                                              "zir.method.vtable");
        auto *slotAddr = builder_.CreateInBoundsGEP(
            i8PtrTy, vtablePtr,
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_),
                                   static_cast<uint64_t>(zirIt->second->vtableSlot)));
        auto *fnRaw =
            builder_.CreateLoad(i8PtrTy, slotAddr, "zir.method.fn.raw");
        auto *fnPtr = builder_.CreateBitCast(
            fnRaw, llvm::PointerType::getUnqual(calleeTy), "zir.method.fn");
        call = builder_.CreateCall(calleeTy, fnPtr, args);
      }
    }
    if (!call) {
      call = builder_.CreateCall(callee, args);
    }
    if (callInst.getResult()) {
      zirValueMap_[callInst.getResult().get()] = call;
      if (isClassType(callInst.getResult()->getType())) {
        zirOwnedClassValues_.insert(callInst.getResult().get());
      }
    }
    return;
  }
  case OpCode::GetElementPtr: {
    const auto &gepInst = static_cast<const GetElementPtrInst &>(inst);
    auto *ptr = lowerZIRValue(gepInst.getPointer());
    auto operandType = gepInst.getPointer()->getType();
    auto pointerType = std::dynamic_pointer_cast<zir::PointerType>(operandType);
    auto baseType = pointerType ? pointerType->getBaseType() : operandType;
    llvm::Value *gep = nullptr;

    if (baseType->getKind() == zir::TypeKind::Record) {
      auto recordType = std::static_pointer_cast<zir::RecordType>(baseType);
      auto *structTy = llvm::cast<llvm::StructType>(toLLVMType(*recordType));
      llvm::Value *structPtr = ptr;
      if (!pointerType) {
        auto *tmp = createEntryAlloca(
            currentFn_,
            static_cast<const Register &>(*gepInst.getResult()).getRawName() +
                ".addr",
            structTy);
        builder_.CreateStore(ptr, tmp);
        structPtr = tmp;
      }
      gep = builder_.CreateStructGEP(
          structTy, structPtr, static_cast<unsigned>(gepInst.getIndex()),
          static_cast<const Register &>(*gepInst.getResult()).getRawName());
    } else if (baseType->getKind() == zir::TypeKind::Class) {
      auto classType = std::static_pointer_cast<zir::ClassType>(baseType);
      auto *objectTy = structCache_.at(classType->getCodegenName() + ".obj");
      llvm::Value *objectPtr = ptr;
      gep = builder_.CreateStructGEP(
          objectTy, objectPtr, static_cast<unsigned>(gepInst.getIndex() + 8),
          static_cast<const Register &>(*gepInst.getResult()).getRawName());
    } else {
      llvm::Value *basePtr = ptr;
      auto *index = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_),
                                           gepInst.getIndex());
      if (baseType->getKind() == zir::TypeKind::Array) {
        auto *arrayTy = toLLVMType(*baseType);
        if (!pointerType) {
          auto *tmp = createEntryAlloca(
              currentFn_,
              static_cast<const Register &>(*gepInst.getResult()).getRawName() +
                  ".array.addr",
              arrayTy);
          builder_.CreateStore(ptr, tmp);
          basePtr = tmp;
        }
        auto *i32Ty = llvm::Type::getInt32Ty(ctx_);
        llvm::Value *indices[] = {llvm::ConstantInt::get(i32Ty, 0), index};
        gep = builder_.CreateInBoundsGEP(
            arrayTy, basePtr, indices,
            static_cast<const Register &>(*gepInst.getResult()).getRawName());
      } else {
        if (!pointerType) {
          throw std::runtime_error("ZIR getelementptr expects pointer operand");
        }
        auto *elemTy = toLLVMType(*baseType);
        gep = builder_.CreateInBoundsGEP(
            elemTy, ptr, index,
            static_cast<const Register &>(*gepInst.getResult()).getRawName());
      }
    }
    zirValueMap_[gepInst.getResult().get()] = gep;
    return;
  }
  case OpCode::Phi: {
    const auto &phiInst = static_cast<const PhiInst &>(inst);
    auto *phi = builder_.CreatePHI(toLLVMType(*phiInst.getResult()->getType()),
                                   phiInst.getIncoming().size(),
                                   static_cast<const Register &>(
                                       *phiInst.getResult()).getRawName());
    bool phiOwnsClassValue = isClassType(phiInst.getResult()->getType());
    for (const auto &incoming : phiInst.getIncoming()) {
      phi->addIncoming(lowerZIRValue(incoming.second),
                       zirBlockMap_.at(incoming.first));
      if (phiOwnsClassValue &&
          zirOwnedClassValues_.count(incoming.second.get()) == 0) {
        phiOwnsClassValue = false;
      }
    }
    zirValueMap_[phiInst.getResult().get()] = phi;
    if (phiOwnsClassValue) {
      zirOwnedClassValues_.insert(phiInst.getResult().get());
    }
    return;
  }
  case OpCode::Cast: {
    const auto &castInst = static_cast<const CastInst &>(inst);
    auto *result = lowerZIRCast(lowerZIRRValue(castInst.getSource()),
                                castInst.getSource()->getType(),
                                castInst.getTargetType());
    zirValueMap_[castInst.getResult().get()] = result;
    if (isClassType(castInst.getTargetType()) &&
        zirOwnedClassValues_.count(castInst.getSource().get()) > 0) {
      zirOwnedClassValues_.insert(castInst.getResult().get());
      zirOwnedClassValues_.erase(castInst.getSource().get());
    }
    return;
  }
  case OpCode::WeakLock: {
    const auto &weakLockInst = static_cast<const WeakLockInst &>(inst);
    auto *result = emitWeakLock(lowerZIRRValue(weakLockInst.getWeakValue()),
                                weakLockInst.getWeakValue()->getType());
    zirValueMap_[weakLockInst.getResult().get()] = result;
    if (isClassType(weakLockInst.getResult()->getType())) {
      zirOwnedClassValues_.insert(weakLockInst.getResult().get());
    }
    return;
  }
  case OpCode::WeakAlive: {
    const auto &weakAliveInst = static_cast<const WeakAliveInst &>(inst);
    auto *result = emitWeakAlive(lowerZIRRValue(weakAliveInst.getWeakValue()),
                                 weakAliveInst.getWeakValue()->getType());
    zirValueMap_[weakAliveInst.getResult().get()] = result;
    return;
  }
  case OpCode::Alloc:
  {
    const auto &allocInst = static_cast<const AllocInst &>(inst);
    auto allocType = allocInst.getAllocatedType();
    if (allocType->getKind() != zir::TypeKind::Class) {
      throw std::runtime_error("ZIR alloc currently supports only class types");
    }

    auto classType = std::static_pointer_cast<zir::ClassType>(allocType);
    auto *ptrTy = llvm::cast<llvm::PointerType>(toLLVMType(*classType));
    auto *objectTy = structCache_.at(classType->getCodegenName() + ".obj");
    auto *sizeOfObj = llvm::ConstantExpr::getSizeOf(objectTy);
    auto *sizeTy = llvm::Type::getInt64Ty(ctx_);
    llvm::Value *sizeValue = sizeOfObj;
    if (sizeValue->getType() != sizeTy) {
      sizeValue = builder_.CreateIntCast(sizeValue, sizeTy, /*isSigned=*/false);
    }

    if (functionMap_.count("malloc") == 0) {
      auto *mallocTy = llvm::FunctionType::get(
          llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(ctx_)), {sizeTy},
          false);
      auto *mallocFn = llvm::Function::Create(
          mallocTy, llvm::Function::ExternalLinkage, "malloc", *module_);
      functionMap_["malloc"] = mallocFn;
    }

    auto *rawPtr = builder_.CreateCall(functionMap_.at("malloc"), {sizeValue},
                                       "class.alloc");
    auto *typedPtr = builder_.CreateBitCast(rawPtr, ptrTy, "class.obj");

    if (functionMap_.count("zap_arc_register") == 0) {
      auto *registerTy = llvm::FunctionType::get(
          llvm::Type::getVoidTy(ctx_),
          {llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(ctx_))}, false);
      auto *registerFn =
          llvm::Function::Create(registerTy, llvm::Function::ExternalLinkage,
                                 "zap_arc_register", *module_);
      functionMap_["zap_arc_register"] = registerFn;
    }
    builder_.CreateCall(functionMap_.at("zap_arc_register"), {rawPtr});

    auto *refCountAddr =
        builder_.CreateStructGEP(objectTy, typedPtr, 0, "refcount.addr");
    builder_.CreateStore(
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), 1), refCountAddr);
    auto *weakCountAddr =
        builder_.CreateStructGEP(objectTy, typedPtr, 1, "weakcount.addr");
    builder_.CreateStore(
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), 0), weakCountAddr);
    auto *aliveAddr =
        builder_.CreateStructGEP(objectTy, typedPtr, 2, "alive.addr");
    builder_.CreateStore(
        llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx_), 1), aliveAddr);
    auto *gcMarkAddr =
        builder_.CreateStructGEP(objectTy, typedPtr, 3, "gcmark.addr");
    builder_.CreateStore(
        llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx_), 0), gcMarkAddr);
    auto *releaseFnAddr =
        builder_.CreateStructGEP(objectTy, typedPtr, 4, "release.fn.addr");
    auto *releaseFnPtr = builder_.CreateBitCast(
        classReleaseFns_.at(classType->getName()),
        llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(ctx_)));
    builder_.CreateStore(releaseFnPtr, releaseFnAddr);
    auto *destroyFnAddr =
        builder_.CreateStructGEP(objectTy, typedPtr, 5, "destroy.fn.addr");
    auto *destroyFnPtr = builder_.CreateBitCast(
        classDestroyFns_.at(classType->getName()),
        llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(ctx_)));
    builder_.CreateStore(destroyFnPtr, destroyFnAddr);
    auto *metadataAddr =
        builder_.CreateStructGEP(objectTy, typedPtr, 6, "metadata.addr");
    auto *metadataPtr = builder_.CreateBitCast(
        classMetadataGlobals_.at(classType->getName()),
        llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(ctx_)));
    builder_.CreateStore(metadataPtr, metadataAddr);
    auto *vtableAddr =
        builder_.CreateStructGEP(objectTy, typedPtr, 7, "vtable.addr");
    auto *i8PtrTy = llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(ctx_));
    auto *vtablePtrTy = llvm::PointerType::getUnqual(i8PtrTy);
    auto *vtableGlobal = classVTables_.at(classType->getName());
    auto *zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
    llvm::Constant *vtableIndices[] = {zero, zero};
    auto *vtablePtr = llvm::ConstantExpr::getInBoundsGetElementPtr(
        vtableGlobal->getValueType(), vtableGlobal, vtableIndices);
    builder_.CreateStore(llvm::ConstantExpr::getBitCast(vtablePtr, vtablePtrTy),
                         vtableAddr);

    for (size_t i = 0; i < classType->getFields().size(); ++i) {
      auto *fieldAddr = builder_.CreateStructGEP(
          objectTy, typedPtr, static_cast<unsigned>(i + 8));
      builder_.CreateStore(
          llvm::Constant::getNullValue(
              toLLVMType(*classType->getFields()[i].type)),
          fieldAddr);
    }

    zirValueMap_[allocInst.getResult().get()] = typedPtr;
    zirOwnedClassValues_.insert(allocInst.getResult().get());
    return;
  }
  case OpCode::Retain:
  case OpCode::Release:
    throw std::runtime_error("ZIR opcode not lowered yet in LLVM backend");
  }
}

void LLVMCodeGen::emitZIRFunction(const zir::Function &fn) {
  currentZIRFunction_ = &fn;
  currentFn_ = functionMap_.at(fn.name);
  zirBlockMap_.clear();
  zirValueMap_.clear();
  zirOwnedClassValues_.clear();
  zirClassParamAllocas_.clear();
  zirPendingClassParamInitAllocas_.clear();
  zirFunctionClassLocals_.clear();
  zirParamSpillIndex_ = 0;

  auto llvmArgIt = currentFn_->arg_begin();
  if (fn.name == "main") {
    auto *i32Ty = llvm::Type::getInt32Ty(ctx_);
    auto *i8PtrTy = llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(ctx_));
    auto *argvTy = llvm::PointerType::getUnqual(i8PtrTy);

    llvmArgIt->setName("argc");
    llvm::Value *argcValue = &*llvmArgIt++;
    llvmArgIt->setName("argv");
    llvm::Value *argvValue = &*llvmArgIt++;

    if (functionMap_.count("__zap_process_set_args") == 0) {
      auto *ft = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_),
                                         {i32Ty, argvTy}, false);
      auto *setArgsFn =
          llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                                 "__zap_process_set_args", *module_);
      functionMap_["__zap_process_set_args"] = setArgsFn;
    }
    builder_.SetInsertPoint(
        llvm::BasicBlock::Create(ctx_, "entry", currentFn_));
    builder_.CreateCall(
        functionMap_.at("__zap_process_set_args"),
        {builder_.CreateIntCast(argcValue, i32Ty, true), argvValue});
    builder_.CreateBr(
        llvm::BasicBlock::Create(ctx_, fn.getBlocks().front()->label, currentFn_));
    zirBlockMap_[fn.getBlocks().front()->label] = &currentFn_->back();
  }

  std::vector<llvm::Value *> physicalArgs;
  for (; llvmArgIt != currentFn_->arg_end(); ++llvmArgIt) {
    physicalArgs.push_back(&*llvmArgIt);
  }

  for (size_t i = 0; i < fn.getBlocks().size(); ++i) {
    const auto &block = fn.getBlocks()[i];
    if (fn.name == "main" && i == 0 && zirBlockMap_.count(block->label) != 0) {
      continue;
    }
    zirBlockMap_[block->label] =
        llvm::BasicBlock::Create(ctx_, block->label, currentFn_);
  }

  auto *argInsertBlock = zirBlockMap_.at(fn.getBlocks().front()->label);
  builder_.SetInsertPoint(argInsertBlock, argInsertBlock->begin());
  size_t physicalArgIndex = 0;
  for (const auto &arg : fn.getArguments()) {
    if (arg->isVariadicPack()) {
      auto *sliceTy = static_cast<llvm::StructType *>(toLLVMType(*arg->getType()));
      llvm::Value *sliceValue = llvm::PoisonValue::get(sliceTy);
      llvm::Value *countValue = physicalArgs.at(physicalArgIndex++);
      llvm::Value *dataValue = physicalArgs.at(physicalArgIndex++);
      sliceValue = builder_.CreateInsertValue(sliceValue, dataValue, {0},
                                              arg->getRawName() + ".data");
      if (countValue->getType() != sliceTy->getElementType(1)) {
        countValue = builder_.CreateIntCast(
            countValue, sliceTy->getElementType(1),
            /*isSigned=*/true, arg->getRawName() + ".len.cast");
      }
      sliceValue = builder_.CreateInsertValue(sliceValue, countValue, {1},
                                              arg->getRawName() + ".len");
      zirValueMap_[arg.get()] = sliceValue;
      continue;
    }
    zirValueMap_[arg.get()] = physicalArgs.at(physicalArgIndex++);
  }

  for (const auto &block : fn.getBlocks()) {
    builder_.SetInsertPoint(zirBlockMap_.at(block->label));
    for (const auto &inst : block->getInstructions()) {
      emitZIRInstruction(*inst);
      if (builder_.GetInsertBlock()->getTerminator()) {
        break;
      }
    }
  }

  currentFn_ = nullptr;
  currentZIRFunction_ = nullptr;
  zirBlockMap_.clear();
  zirValueMap_.clear();
  zirOwnedClassValues_.clear();
  zirClassParamAllocas_.clear();
  zirPendingClassParamInitAllocas_.clear();
  zirFunctionClassLocals_.clear();
}

void LLVMCodeGen::visit(sema::BoundRootNode &node) {
  for (const auto &extFn : node.externalFunctions) {
    auto *ft = buildFunctionType(*extFn->symbol);
    auto *f = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                                     extFn->symbol->linkName, *module_);
    auto argIt = f->arg_begin();
    for (const auto &param : extFn->symbol->parameters) {
      if (param->is_variadic_pack) {
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

  for (const auto &fn : node.functions) {
    bool isEntryMain = fn->symbol->linkName == "main";
    auto *ft = buildFunctionType(*fn->symbol, isEntryMain);
    auto *f = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                                     fn->symbol->linkName, *module_);
    auto argIt = f->arg_begin();
    if (isEntryMain) {
      argIt->setName("argc");
      ++argIt;
      argIt->setName("argv");
      ++argIt;
    }
    for (const auto &param : fn->symbol->parameters) {
      if (param->is_variadic_pack) {
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
    if (fn->symbol->isDestructor) {
      classDestructorFns_[fn->symbol->ownerTypeName] = f;
    }
    if (fn->symbol->vtableSlot >= 0) {
      classVirtualMethodFns_[fn->symbol->ownerTypeName]
                            [fn->symbol->vtableSlot] = f;
    }
  }

  for (const auto &rec : node.records) {
    if (rec->type && rec->type->getKind() == zir::TypeKind::Class) {
      ensureClassArcSupport(
          std::static_pointer_cast<zir::ClassType>(rec->type));
    }
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

void LLVMCodeGen::visit(sema::BoundFunctionDeclaration &node) {
  auto *fn = functionMap_.at(node.symbol->linkName);
  currentFn_ = fn;
  localValues_.clear();
  scopeClassLocals_.clear();
  scopeClassLocals_.push_back({});

  auto *entry = llvm::BasicBlock::Create(ctx_, "entry", fn);
  builder_.SetInsertPoint(entry);

  auto argIt = fn->arg_begin();
  if (node.symbol->linkName == "main") {
    auto *i32Ty = llvm::Type::getInt32Ty(ctx_);
    auto *i8PtrTy = llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(ctx_));
    auto *argvTy = llvm::PointerType::getUnqual(i8PtrTy);

    argIt->setName("argc");
    llvm::Value *argcValue = &*argIt++;
    argIt->setName("argv");
    llvm::Value *argvValue = &*argIt++;

    if (functionMap_.count("__zap_process_set_args") == 0) {
      auto *ft = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_),
                                         {i32Ty, argvTy}, false);
      auto *setArgsFn =
          llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                                 "__zap_process_set_args", *module_);
      functionMap_["__zap_process_set_args"] = setArgsFn;
    }

    builder_.CreateCall(
        functionMap_.at("__zap_process_set_args"),
        {builder_.CreateIntCast(argcValue, i32Ty, true), argvValue});
  }

  // Spill each argument to a stack slot so we can reassign params later.
  size_t idx = 0;
  for (; argIt != fn->arg_end(); ++argIt) {
    const auto &param = node.symbol->parameters[idx++];
    if (param->is_variadic_pack) {
      auto &countArg = *argIt;
      ++argIt;
      auto &dataArg = *argIt;
      auto *sliceTy = static_cast<llvm::StructType *>(toLLVMType(*param->type));
      auto *sliceAlloca = createEntryAlloca(fn, param->name, sliceTy);
      llvm::Value *sliceValue = llvm::PoisonValue::get(sliceTy);
      sliceValue = builder_.CreateInsertValue(sliceValue, &dataArg, {0},
                                              param->name + ".data");
      llvm::Value *countValue = &countArg;
      if (countValue->getType() != sliceTy->getElementType(1)) {
        countValue = builder_.CreateIntCast(
            countValue, sliceTy->getElementType(1),
            /*isSigned=*/true, param->name + ".len.cast");
      }
      sliceValue = builder_.CreateInsertValue(sliceValue, countValue, {1},
                                              param->name + ".len");
      builder_.CreateStore(sliceValue, sliceAlloca);
      localValues_[param->name] = sliceAlloca;
      continue;
    }

    auto &arg = *argIt;
    auto *alloca = createEntryAlloca(fn, param->name, arg.getType());
    localValues_[param->name] = alloca;
    if (isClassType(param->type) && !param->is_ref) {
      bool isBorrowedSelf =
          node.symbol->isMethod && !node.symbol->isStatic && idx == 1;
      builder_.CreateStore(llvm::Constant::getNullValue(arg.getType()), alloca);
      if (isWeakClassType(param->type)) {
        emitStoreWithArc(alloca, &arg, param->type, /*valueIsOwned=*/false);
        scopeClassLocals_.back().push_back({param->type, alloca});
      } else if (!isBorrowedSelf) {
        builder_.CreateStore(&arg, alloca);
        scopeClassLocals_.back().push_back({param->type, alloca});
      } else {
        builder_.CreateStore(&arg, alloca);
      }
    } else {
      builder_.CreateStore(&arg, alloca);
    }
  }

  node.body->accept(*this);

  if (!builder_.GetInsertBlock()->getTerminator()) {
    if (node.body->result) {
      for (auto it = scopeClassLocals_.rbegin(); it != scopeClassLocals_.rend();
           ++it) {
        for (auto local = it->rbegin(); local != it->rend(); ++local) {
          auto *value = builder_.CreateLoad(
              toLLVMType(*local->first), local->second, "arc.fn.ret.release");
          if (isWeakClassType(local->first))
            emitReleaseWeakIfNeeded(value, local->first);
          else
            emitReleaseIfNeeded(value, local->first);
        }
      }
      builder_.CreateRet(lastValue_);
    } else if (fn->getReturnType()->isVoidTy()) {
      for (auto it = scopeClassLocals_.rbegin(); it != scopeClassLocals_.rend();
           ++it) {
        for (auto local = it->rbegin(); local != it->rend(); ++local) {
          auto *value = builder_.CreateLoad(
              toLLVMType(*local->first), local->second, "arc.fn.ret.release");
          if (isWeakClassType(local->first))
            emitReleaseWeakIfNeeded(value, local->first);
          else
            emitReleaseIfNeeded(value, local->first);
        }
      }
      builder_.CreateRetVoid();
    }
  }

  currentFn_ = nullptr;
  scopeClassLocals_.clear();
}

void LLVMCodeGen::visit(sema::BoundExternalFunctionDeclaration &node) {
  (void)node;
}

void LLVMCodeGen::visit(sema::BoundModuleReference &node) {
  (void)node;
  throw std::runtime_error("module reference reached codegen");
}

void LLVMCodeGen::visit(sema::BoundBlock &node) {
  scopeClassLocals_.push_back({});
  for (const auto &stmt : node.statements) {
    if (builder_.GetInsertBlock()->getTerminator()) {
      break;
    }
    stmt->accept(*this);
  }
  if (node.result && !builder_.GetInsertBlock()->getTerminator()) {
    node.result->accept(*this);
    if (isClassType(node.result->type) &&
        !expressionProducesOwnedClass(node.result.get())) {
      emitRetainIfNeeded(lastValue_, node.result->type);
    }
  }
  if (!builder_.GetInsertBlock()->getTerminator()) {
    emitScopeReleases();
  }
  scopeClassLocals_.pop_back();
}

void LLVMCodeGen::visit(sema::BoundVariableDeclaration &node) {
  auto *ty = toLLVMType(*node.symbol->type);

  if (currentFn_) {
    auto *alloca = createEntryAlloca(currentFn_, node.symbol->name, ty);
    localValues_[node.symbol->name] = alloca;
    if (isClassType(node.symbol->type)) {
      builder_.CreateStore(llvm::Constant::getNullValue(ty), alloca);
    }

    if (node.initializer) {
      node.initializer->accept(*this);
      emitStoreWithArc(alloca, lastValue_, node.symbol->type,
                       expressionProducesOwnedClass(node.initializer.get()));
    }
    if (isClassType(node.symbol->type) && !scopeClassLocals_.empty()) {
      scopeClassLocals_.back().push_back({node.symbol->type, alloca});
    }
  } else {
    llvm::Constant *initializer = nullptr;
    if (node.initializer) {
      node.initializer->accept(*this);
      initializer = llvm::dyn_cast<llvm::Constant>(lastValue_);
    }

    if (!initializer) {
      // If no initializer or initializer is not a constant, use null
      // initializer
      initializer = llvm::Constant::getNullValue(ty);
    }

    auto *gv = new llvm::GlobalVariable(*module_, ty, node.symbol->is_const,
                                        llvm::GlobalVariable::ExternalLinkage,
                                        initializer, node.symbol->linkName);
    globalValues_[node.symbol->linkName] = gv;
  }
}

void LLVMCodeGen::visit(sema::BoundReturnStatement &node) {
  if (node.expression) {
    node.expression->accept(*this);
    if (isClassType(node.expression->type) &&
        !expressionProducesOwnedClass(node.expression.get())) {
      emitRetainIfNeeded(lastValue_, node.expression->type);
    }
    for (auto it = scopeClassLocals_.rbegin(); it != scopeClassLocals_.rend();
         ++it) {
      for (auto local = it->rbegin(); local != it->rend(); ++local) {
        auto *value = builder_.CreateLoad(toLLVMType(*local->first),
                                          local->second, "arc.ret.release");
        if (isWeakClassType(local->first))
          emitReleaseWeakIfNeeded(value, local->first);
        else
          emitReleaseIfNeeded(value, local->first);
      }
    }
    builder_.CreateRet(lastValue_);
  } else {
    for (auto it = scopeClassLocals_.rbegin(); it != scopeClassLocals_.rend();
         ++it) {
      for (auto local = it->rbegin(); local != it->rend(); ++local) {
        auto *value = builder_.CreateLoad(toLLVMType(*local->first),
                                          local->second, "arc.ret.release");
        if (isWeakClassType(local->first))
          emitReleaseWeakIfNeeded(value, local->first);
        else
          emitReleaseIfNeeded(value, local->first);
      }
    }
    builder_.CreateRetVoid();
  }
}

void LLVMCodeGen::visit(sema::BoundCast &node) {
  node.expression->accept(*this);
  auto *src = lastValue_;
  auto *srcTy = src->getType();
  auto *destTy = toLLVMType(*node.type);

  if (isStringType(node.expression->type) && destTy->isPointerTy()) {
    auto *ptr = builder_.CreateExtractValue(src, {0}, "str.ptr");
    if (ptr->getType() == destTy) {
      lastValue_ = ptr;
    } else {
      lastValue_ = builder_.CreateBitCast(ptr, destTy);
    }
    return;
  }

  if (srcTy == destTy) {
    lastValue_ = src;
    return;
  }

  if (srcTy->isPointerTy() && destTy->isPointerTy()) {
    lastValue_ = builder_.CreateBitCast(src, destTy);
  } else if (srcTy->isPointerTy() && destTy->isIntegerTy()) {
    lastValue_ = builder_.CreatePtrToInt(src, destTy);
  } else if (srcTy->isIntegerTy() && destTy->isPointerTy()) {
    lastValue_ = builder_.CreateIntToPtr(src, destTy);
  } else if (srcTy->isIntegerTy() && destTy->isIntegerTy()) {
    unsigned srcBits = srcTy->getIntegerBitWidth();
    unsigned destBits = destTy->getIntegerBitWidth();

    if (destBits > srcBits) {
      if (node.expression->type->isUnsigned()) {
        lastValue_ = builder_.CreateZExt(src, destTy);
      } else {
        lastValue_ = builder_.CreateSExt(src, destTy);
      }
    } else if (destBits < srcBits) {
      lastValue_ = builder_.CreateTrunc(src, destTy);
    } else {
      // Same bit width but different signedness in our ZIR, but LLVM doesn't
      // care
      lastValue_ = src;
    }
  } else if (srcTy->isIntegerTy() && destTy->isFloatingPointTy()) {
    if (node.expression->type->isUnsigned()) {
      lastValue_ = builder_.CreateUIToFP(src, destTy);
    } else {
      lastValue_ = builder_.CreateSIToFP(src, destTy);
    }
  } else if (srcTy->isFloatingPointTy() && destTy->isIntegerTy()) {
    if (node.type->isUnsigned()) {
      lastValue_ = builder_.CreateFPToUI(src, destTy);
    } else {
      lastValue_ = builder_.CreateFPToSI(src, destTy);
    }
  } else if (srcTy->isFloatingPointTy() && destTy->isFloatingPointTy()) {
    if (srcTy->getPrimitiveSizeInBits() < destTy->getPrimitiveSizeInBits()) {
      lastValue_ = builder_.CreateFPExt(src, destTy);
    } else if (srcTy->getPrimitiveSizeInBits() >
               destTy->getPrimitiveSizeInBits()) {
      lastValue_ = builder_.CreateFPTrunc(src, destTy);
    } else {
      lastValue_ = src;
    }
  } else {
    // Fallback or other pointer casts
    lastValue_ = builder_.CreateBitCast(src, destTy);
  }
}

void LLVMCodeGen::visit(sema::BoundAssignment &node) {
  node.expression->accept(*this);
  llvm::Value *val = lastValue_;

  bool old = evaluateAsAddr_;
  evaluateAsAddr_ = true;
  node.target->accept(*this);
  llvm::Value *alloca = lastValue_;
  evaluateAsAddr_ = old;

  if (isClassType(node.target->type)) {
    emitStoreWithArc(alloca, val, node.target->type,
                     expressionProducesOwnedClass(node.expression.get()));
  } else {
    builder_.CreateStore(val, alloca);
  }
}

void LLVMCodeGen::visit(sema::BoundExpressionStatement &node) {
  node.expression->accept(*this);
  if (isClassType(node.expression->type) &&
      expressionProducesOwnedClass(node.expression.get())) {
    emitReleaseIfNeeded(lastValue_, node.expression->type);
  }
}

void LLVMCodeGen::visit(sema::BoundLiteral &node) {
  if (node.type->getKind() == zir::TypeKind::Record) {
    const auto &rt = static_cast<const zir::RecordType &>(*node.type);
    if (rt.getName() == "String") {
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
  if (ty->isIntegerTy(1)) {
    lastValue_ = llvm::ConstantInt::get(ty, node.value == "true" ? 1 : 0);
  } else if (ty->isIntegerTy(8)) {
    int64_t code = 0;
    if (!node.value.empty()) {
      if (node.value.size() >= 2 && node.value[0] == '\\') {
        switch (node.value[1]) {
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
      } else {
        code = static_cast<unsigned char>(node.value[0]);
      }
    }
    lastValue_ = llvm::ConstantInt::get(ty, code, /*isSigned=*/false);
  } else if (ty->isIntegerTy()) {
    if (node.type->isUnsigned()) {
      lastValue_ = llvm::ConstantInt::get(ty, std::stoull(node.value),
                                          /*isSigned=*/false);
    } else {
      lastValue_ =
          llvm::ConstantInt::get(ty, std::stoll(node.value), /*isSigned=*/true);
    }
  } else if (ty->isFloatTy()) {
    lastValue_ = llvm::ConstantFP::get(ty, std::stof(node.value));
  } else if (ty->isDoubleTy()) {
    lastValue_ = llvm::ConstantFP::get(ty, std::stod(node.value));
  } else {
    lastValue_ = llvm::Constant::getNullValue(ty);
  }
}

void LLVMCodeGen::visit(sema::BoundVariableExpression &node) {
  llvm::Value *addr = nullptr;
  if (localValues_.count(node.symbol->name)) {
    addr = localValues_.at(node.symbol->name);
  } else {
    addr = globalValues_.at(node.symbol->linkName);
  }

  if (node.symbol->is_ref) {
    // addr is a pointer to the pointer passed as argument
    auto *ptrTy = llvm::PointerType::getUnqual(toLLVMType(*node.symbol->type));
    addr = builder_.CreateLoad(ptrTy, addr, node.symbol->name + ".ptr");
  }

  if (evaluateAsAddr_) {
    lastValue_ = addr;
  } else {
    auto *ty = toLLVMType(*node.symbol->type);
    lastValue_ = builder_.CreateLoad(ty, addr, node.symbol->name);
  }
}

void LLVMCodeGen::visit(sema::BoundBinaryExpression &node) {
  if (node.op == "&&") {
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
    phi->addIncoming(llvm::ConstantInt::get(llvm::Type::getInt1Ty(ctx_), 0),
                     leftBB);
    phi->addIncoming(rhs, actualRhsBB);
    lastValue_ = phi;
    return;
  }

  if (node.op == "||") {
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
    phi->addIncoming(llvm::ConstantInt::get(llvm::Type::getInt1Ty(ctx_), 1),
                     leftBB);
    phi->addIncoming(rhs, actualRhsBB);
    lastValue_ = phi;
    return;
  }

  node.left->accept(*this);
  auto *lhs = lastValue_;
  node.right->accept(*this);
  auto *rhs = lastValue_;

  bool isPointer =
      lhs->getType()->isPointerTy() || rhs->getType()->isPointerTy();
  bool isFP = lhs->getType()->isFloatingPointTy();
  bool isUnsigned = node.left->type->isUnsigned();

  if (isPointer && (node.op == "+" || node.op == "-")) {
    llvm::Value *pointerValue = lhs->getType()->isPointerTy() ? lhs : rhs;
    llvm::Value *offsetValue = lhs->getType()->isPointerTy() ? rhs : lhs;
    auto ptrType = std::static_pointer_cast<zir::PointerType>(
        lhs->getType()->isPointerTy() ? node.left->type : node.right->type);
    auto *elemTy = toLLVMType(*ptrType->getBaseType());

    if (lhs->getType()->isPointerTy() && rhs->getType()->isPointerTy()) {
      auto *i64Ty = llvm::Type::getInt64Ty(ctx_);
      auto *lhsInt = builder_.CreatePtrToInt(lhs, i64Ty);
      auto *rhsInt = builder_.CreatePtrToInt(rhs, i64Ty);
      auto *bytes = builder_.CreateSub(lhsInt, rhsInt);
      llvm::Value *elemSize64 = llvm::ConstantExpr::getSizeOf(elemTy);
      if (elemSize64->getType() != i64Ty) {
        elemSize64 =
            builder_.CreateIntCast(elemSize64, i64Ty, /*isSigned=*/false);
      }
      lastValue_ = builder_.CreateSDiv(bytes, elemSize64);
      return;
    }

    auto *indexTy = llvm::Type::getInt64Ty(ctx_);
    auto *index =
        builder_.CreateIntCast(offsetValue, indexTy, /*isSigned=*/true);
    if (node.op == "-") {
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
    lastValue_ = isFP ? builder_.CreateFDiv(lhs, rhs)
                      : (isUnsigned ? builder_.CreateUDiv(lhs, rhs)
                                    : builder_.CreateSDiv(lhs, rhs));
  else if (node.op == "%")
    lastValue_ = isFP ? builder_.CreateFRem(lhs, rhs)
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
  else if (node.op == "~") {
    auto *i8Ty = llvm::Type::getInt8Ty(ctx_);
    auto *i64Ty = llvm::Type::getInt64Ty(ctx_);

    llvm::Value *lhs_ptr = nullptr;
    llvm::Value *lhs_len = nullptr;
    llvm::Value *rhs_ptr = nullptr;
    llvm::Value *rhs_len = nullptr;

    if (node.left->type->getKind() == zir::TypeKind::Record) {
      lhs_ptr = builder_.CreateExtractValue(lhs, {0});
      lhs_len = builder_.CreateExtractValue(lhs, {1});
    } else if (node.left->type->getKind() == zir::TypeKind::Char) {
      auto *buf = createEntryAlloca(currentFn_, "char_buf_l", i8Ty);
      builder_.CreateStore(lhs, buf);
      lhs_ptr = buf;
      lhs_len = llvm::ConstantInt::get(i64Ty, 1);
    }

    if (node.right->type->getKind() == zir::TypeKind::Record) {
      rhs_ptr = builder_.CreateExtractValue(rhs, {0});
      rhs_len = builder_.CreateExtractValue(rhs, {1});
    } else if (node.right->type->getKind() == zir::TypeKind::Char) {
      auto *buf = createEntryAlloca(currentFn_, "char_buf_r", i8Ty);
      builder_.CreateStore(rhs, buf);
      rhs_ptr = buf;
      rhs_len = llvm::ConstantInt::get(i64Ty, 1);
    }

    if (functionMap_.count("string_concat_ptrlen") == 0) {
      std::vector<llvm::Type *> params = {
          llvm::PointerType::getUnqual(i8Ty), i64Ty,
          llvm::PointerType::getUnqual(i8Ty), i64Ty};
      auto *ft = llvm::FunctionType::get(llvm::PointerType::getUnqual(i8Ty),
                                         params, false);
      auto *fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                                        "string_concat_ptrlen", *module_);
      functionMap_["string_concat_ptrlen"] = fn;
    }

    auto *concatFn = functionMap_.at("string_concat_ptrlen");
    auto *call =
        builder_.CreateCall(concatFn, {lhs_ptr, lhs_len, rhs_ptr, rhs_len});

    auto *sumLen = builder_.CreateAdd(lhs_len, rhs_len);

    auto *structTy = static_cast<llvm::StructType *>(toLLVMType(*node.type));
    llvm::Value *res = llvm::UndefValue::get(structTy);
    res = builder_.CreateInsertValue(res, call, {0});
    res = builder_.CreateInsertValue(res, sumLen, {1});
    lastValue_ = res;
  }
}

void LLVMCodeGen::visit(sema::BoundTernaryExpression &node) {
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

void LLVMCodeGen::visit(sema::BoundUnaryExpression &node) {
  if (node.op == "&") {
    bool old = evaluateAsAddr_;
    evaluateAsAddr_ = true;
    node.expr->accept(*this);
    evaluateAsAddr_ = old;
    return;
  } else if (node.op == "*") {
    bool old = evaluateAsAddr_;
    evaluateAsAddr_ = false;
    node.expr->accept(*this);
    evaluateAsAddr_ = old;
    auto *ptr = lastValue_;
    if (evaluateAsAddr_) {
      lastValue_ = ptr;
    } else {
      lastValue_ = builder_.CreateLoad(toLLVMType(*node.type), ptr, "deref");
    }
    return;
  }

  node.expr->accept(*this);
  if (node.op == "-") {
    lastValue_ = node.type->isFloatingPoint() ? builder_.CreateFNeg(lastValue_)
                                              : builder_.CreateNeg(lastValue_);
  } else if (node.op == "!") {
    lastValue_ = builder_.CreateNot(lastValue_);
  }
}

void LLVMCodeGen::visit(sema::BoundFunctionCall &node) {
  auto *callee = functionMap_.at(node.symbol->linkName);
  std::vector<llvm::Value *> args;
  size_t fixedParamCount = node.symbol->fixedParameterCount();
  for (size_t i = 0; i < node.arguments.size(); ++i) {
    bool isRef = false;
    if (i < fixedParamCount && i < node.argumentIsRef.size())
      isRef = node.argumentIsRef[i];

    bool old = evaluateAsAddr_;
    if (isRef)
      evaluateAsAddr_ = true;

    node.arguments[i]->accept(*this);
    if (!isRef && i < fixedParamCount) {
      const auto &param = node.symbol->parameters[i];
      bool isBorrowedSelf =
          node.symbol->isMethod && !node.symbol->isStatic && i == 0;
      if (!isBorrowedSelf && isClassType(param->type) &&
          !isWeakClassType(param->type) &&
          !expressionProducesOwnedClass(node.arguments[i].get())) {
        emitRetainIfNeeded(lastValue_, param->type);
      }
    }
    args.push_back(lastValue_);

    evaluateAsAddr_ = old;
  }

  if (node.symbol->hasVariadicParameter()) {
    auto variadicParam = node.symbol->variadicParameter();
    auto *elemTy = toLLVMType(*variadicParam->variadic_element_type);
    auto *elemPtrTy = llvm::PointerType::getUnqual(elemTy);
    size_t explicitVariadicCount = node.arguments.size() - fixedParamCount;

    llvm::Value *forwardedCount =
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
    llvm::Value *forwardedData = llvm::ConstantPointerNull::get(elemPtrTy);
    if (node.variadicPack) {
      node.variadicPack->accept(*this);
      forwardedData =
          builder_.CreateExtractValue(lastValue_, {0}, "varargs.forward.data");
      forwardedCount =
          builder_.CreateExtractValue(lastValue_, {1}, "varargs.forward.len");
      if (forwardedCount->getType() != llvm::Type::getInt32Ty(ctx_)) {
        forwardedCount = builder_.CreateIntCast(
            forwardedCount, llvm::Type::getInt32Ty(ctx_), /*isSigned=*/true);
      }
    }

    llvm::Value *explicitCount =
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_),
                               static_cast<uint64_t>(explicitVariadicCount));

    if (explicitVariadicCount == 0 && !node.variadicPack) {
      args.push_back(explicitCount);
      args.push_back(llvm::ConstantPointerNull::get(elemPtrTy));
    } else if (explicitVariadicCount == 0) {
      args.push_back(forwardedCount);
      args.push_back(forwardedData);
    } else {
      llvm::Value *totalCount = explicitCount;
      if (node.variadicPack) {
        totalCount =
            builder_.CreateAdd(explicitCount, forwardedCount, "varargs.total");
      }

      auto *buffer = builder_.CreateAlloca(elemTy, totalCount, "varargs.buf");
      for (size_t i = 0; i < explicitVariadicCount; ++i) {
        auto *dst = builder_.CreateInBoundsGEP(
            elemTy, buffer,
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_),
                                   static_cast<uint64_t>(i)));
        builder_.CreateStore(args[fixedParamCount + i], dst);
      }

      if (node.variadicPack) {
        auto *copyCondBB =
            llvm::BasicBlock::Create(ctx_, "varargs.copy.cond", currentFn_);
        auto *copyBodyBB =
            llvm::BasicBlock::Create(ctx_, "varargs.copy.body", currentFn_);
        auto *copyDoneBB =
            llvm::BasicBlock::Create(ctx_, "varargs.copy.done", currentFn_);
        auto *copyPreheaderBB = builder_.GetInsertBlock();

        builder_.CreateBr(copyCondBB);
        builder_.SetInsertPoint(copyCondBB);
        auto *indexPhi =
            builder_.CreatePHI(llvm::Type::getInt32Ty(ctx_), 2, "varargs.i");
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
  if (node.symbol->vtableSlot >= 0 && !args.empty()) {
    auto classType =
        std::static_pointer_cast<zir::ClassType>(node.arguments[0]->type);
    auto *objectTy = structCache_.at(classType->getCodegenName() + ".obj");
    auto *selfPtr = builder_.CreateBitCast(
        args[0], llvm::PointerType::getUnqual(objectTy), "method.self");
    auto *vtableAddr =
        builder_.CreateStructGEP(objectTy, selfPtr, 7, "method.vtable.addr");
    auto *i8PtrTy = llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(ctx_));
    auto *vtablePtrTy = llvm::PointerType::getUnqual(i8PtrTy);
    auto *vtablePtr =
        builder_.CreateLoad(vtablePtrTy, vtableAddr, "method.vtable");
    auto *slotAddr = builder_.CreateInBoundsGEP(
        i8PtrTy, vtablePtr,
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_),
                               static_cast<uint64_t>(node.symbol->vtableSlot)));
    auto *fnRaw = builder_.CreateLoad(i8PtrTy, slotAddr, "method.fn.raw");
    auto *fnPtr = builder_.CreateBitCast(
        fnRaw, llvm::PointerType::getUnqual(callee->getFunctionType()),
        "method.fn");
    lastValue_ = builder_.CreateCall(callee->getFunctionType(), fnPtr, args);
    return;
  }

  lastValue_ = builder_.CreateCall(callee, args);

  for (size_t i = 0; i < node.arguments.size() && i < fixedParamCount; ++i) {
    if (i < node.argumentIsRef.size() && node.argumentIsRef[i]) {
      continue;
    }
    const auto &param = node.symbol->parameters[i];
    if (isWeakClassType(param->type) &&
        expressionProducesOwnedClass(node.arguments[i].get())) {
      auto strongType = std::make_shared<zir::ClassType>(
          *std::static_pointer_cast<zir::ClassType>(param->type));
      strongType->setWeak(false);
      emitReleaseIfNeeded(args[i], strongType);
    }
  }
}

void LLVMCodeGen::visit(sema::BoundArrayLiteral &node) {
  auto *arrayTy = static_cast<llvm::ArrayType *>(toLLVMType(*node.type));
  auto *elemTy = arrayTy->getElementType();
  (void)elemTy;

  std::vector<llvm::Constant *> constants;
  bool allConstants = true;

  for (const auto &expr : node.elements) {
    expr->accept(*this);
    if (auto *c = llvm::dyn_cast<llvm::Constant>(lastValue_)) {
      constants.push_back(c);
    } else {
      allConstants = false;
      break;
    }
  }

  if (allConstants) {
    lastValue_ = llvm::ConstantArray::get(arrayTy, constants);
  } else {
    // If not all elements are constants, allocate on stack and store
    auto *alloca = createEntryAlloca(currentFn_, "array_lit", arrayTy);
    for (size_t i = 0; i < node.elements.size(); ++i) {
      node.elements[i]->accept(*this);
      auto *ptr = builder_.CreateConstGEP2_32(arrayTy, alloca, 0, (unsigned)i);
      builder_.CreateStore(lastValue_, ptr);
    }
    lastValue_ = builder_.CreateLoad(arrayTy, alloca);
  }
}

void LLVMCodeGen::visit(sema::BoundIndexAccess &node) {
  llvm::Value *leftAddr = nullptr;
  llvm::Value *leftValue = nullptr;
  bool old = evaluateAsAddr_;

  if (isStringType(node.left->type)) {
    evaluateAsAddr_ = false;
    node.left->accept(*this);
    leftValue = lastValue_;
  } else {
    evaluateAsAddr_ = true;
    node.left->accept(*this);
    leftAddr = lastValue_;
  }

  evaluateAsAddr_ = false;
  node.index->accept(*this);
  llvm::Value *indexVal = lastValue_;
  evaluateAsAddr_ = old;

  llvm::Value *elemAddr = nullptr;

  if (node.left->type->getKind() == zir::TypeKind::Array) {
    auto *leftTy = toLLVMType(*node.left->type);
    auto *i32Ty = llvm::Type::getInt32Ty(ctx_);
    std::vector<llvm::Value *> indices = {
        llvm::ConstantInt::get(i32Ty, 0),
        builder_.CreateIntCast(indexVal, i32Ty, /*isSigned=*/false)};
    elemAddr = builder_.CreateInBoundsGEP(leftTy, leftAddr, indices);
  } else if (isVariadicViewType(node.left->type)) {
    auto *sliceTy =
        static_cast<llvm::StructType *>(toLLVMType(*node.left->type));
    auto *dataAddr =
        builder_.CreateStructGEP(sliceTy, leftAddr, 0, "varargs.data.addr");
    auto *dataPtrTy = llvm::cast<llvm::PointerType>(sliceTy->getElementType(0));
    auto *dataPtr = builder_.CreateLoad(dataPtrTy, dataAddr, "varargs.data");
    auto recordType =
        std::static_pointer_cast<zir::RecordType>(node.left->type);
    auto dataType = std::static_pointer_cast<zir::PointerType>(
        recordType->getFields()[0].type);
    auto *elemTy = toLLVMType(*dataType->getBaseType());
    elemAddr =
        builder_.CreateInBoundsGEP(elemTy, dataPtr, indexVal, "varargs.index");
  } else if (isStringType(node.left->type)) {
    if (evaluateAsAddr_) {
      throw std::runtime_error("String index access is not assignable.");
    }

    auto *ptr = builder_.CreateExtractValue(leftValue, {0}, "string.ptr");
    auto *i8Ty = llvm::Type::getInt8Ty(ctx_);
    elemAddr = builder_.CreateInBoundsGEP(i8Ty, ptr, indexVal, "string.index");
  } else {
    throw std::runtime_error("Type '" + node.left->type->toString() +
                             "' does not support indexing.");
  }

  if (evaluateAsAddr_) {
    lastValue_ = elemAddr;
  } else {
    auto *ty = toLLVMType(*node.type);
    lastValue_ = builder_.CreateLoad(ty, elemAddr, "index_access");
  }
}

void LLVMCodeGen::visit(sema::BoundRecordDeclaration &node) {
  toLLVMType(*node.type);
}

void LLVMCodeGen::visit(sema::BoundEnumDeclaration &node) {
  // Enums are typically handled at the type level in ZIR/sema.
  // We don't need to generate code for the declaration itself
  // unless we want to generate debug info or constant values.
  (void)node;
}

void LLVMCodeGen::visit(sema::BoundMemberAccess &node) {
  if (node.left->type->getKind() == zir::TypeKind::Class) {
    bool old = evaluateAsAddr_;
    evaluateAsAddr_ = false;
    node.left->accept(*this);
    auto *objectPtr = lastValue_;
    evaluateAsAddr_ = old;

    auto classType = std::static_pointer_cast<zir::ClassType>(node.left->type);
    int fieldIndex = -1;
    const auto &fields = classType->getFields();
    for (size_t i = 0; i < fields.size(); ++i) {
      if (fields[i].name == node.member) {
        fieldIndex = static_cast<int>(i) + 8;
        break;
      }
    }

    if (fieldIndex == -1)
      throw std::runtime_error("Field '" + node.member +
                               "' not found in class '" +
                               node.left->type->toString() + "'");

    auto *objectStructTy =
        structCache_.at(classType->getCodegenName() + ".obj");
    llvm::Value *fieldAddr = builder_.CreateStructGEP(objectStructTy, objectPtr,
                                                      fieldIndex, node.member);

    if (evaluateAsAddr_) {
      lastValue_ = fieldAddr;
    } else {
      lastValue_ =
          builder_.CreateLoad(toLLVMType(*node.type), fieldAddr, node.member);
    }
    return;
  }

  llvm::Value *leftAddr = nullptr;

  // Check if left is a dereference (*ptr).field
  auto *unary = dynamic_cast<sema::BoundUnaryExpression *>(node.left.get());
  if (unary && unary->op == "*") {
    // Evaluate the pointer directly, skip the dereference
    bool old = evaluateAsAddr_;
    evaluateAsAddr_ = false;
    unary->expr->accept(*this);
    leftAddr = lastValue_;
    evaluateAsAddr_ = old;
  } else {
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
  for (size_t i = 0; i < fields.size(); ++i) {
    if (fields[i].name == node.member) {
      fieldIndex = static_cast<int>(i);
      break;
    }
  }

  if (fieldIndex == -1)
    throw std::runtime_error("Field '" + node.member + "' not found in type '" +
                             node.left->type->toString() + "'");

  llvm::StructType *structTy =
      static_cast<llvm::StructType *>(toLLVMType(*recordType));
  llvm::Value *fieldAddr =
      builder_.CreateStructGEP(structTy, leftAddr, fieldIndex, node.member);

  if (evaluateAsAddr_) {
    lastValue_ = fieldAddr;
  } else {
    lastValue_ =
        builder_.CreateLoad(toLLVMType(*node.type), fieldAddr, node.member);
  }
}

void LLVMCodeGen::visit(sema::BoundStructLiteral &node) {
  auto recordType = std::static_pointer_cast<zir::RecordType>(node.type);
  llvm::StructType *structTy =
      static_cast<llvm::StructType *>(toLLVMType(*recordType));

  // Create an alloca for the struct
  llvm::Value *structAddr =
      createEntryAlloca(currentFn_, "struct_literal", structTy);

  for (const auto &fieldInit : node.fields) {
    // Find field index
    int fieldIndex = -1;
    const auto &fields = recordType->getFields();
    for (size_t i = 0; i < fields.size(); ++i) {
      if (fields[i].name == fieldInit.first) {
        fieldIndex = static_cast<int>(i);
        break;
      }
    }

    fieldInit.second->accept(*this);
    llvm::Value *val = lastValue_;

    llvm::Value *fieldAddr =
        builder_.CreateStructGEP(structTy, structAddr, fieldIndex);
    builder_.CreateStore(val, fieldAddr);
  }

  if (evaluateAsAddr_) {
    lastValue_ = structAddr;
  } else {
    lastValue_ = builder_.CreateLoad(structTy, structAddr);
  }
}

void LLVMCodeGen::visit(sema::BoundNewExpression &node) {
  auto *ptrTy = llvm::cast<llvm::PointerType>(toLLVMType(*node.classType));
  auto *objectTy = structCache_.at(node.classType->getCodegenName() + ".obj");
  auto *sizeOfObj = llvm::ConstantExpr::getSizeOf(objectTy);
  auto *sizeTy = llvm::Type::getInt64Ty(ctx_);
  llvm::Value *sizeValue = sizeOfObj;
  if (sizeValue->getType() != sizeTy) {
    sizeValue = builder_.CreateIntCast(sizeValue, sizeTy, /*isSigned=*/false);
  }

  if (functionMap_.count("malloc") == 0) {
    auto *mallocTy = llvm::FunctionType::get(
        llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(ctx_)), {sizeTy},
        false);
    auto *mallocFn = llvm::Function::Create(
        mallocTy, llvm::Function::ExternalLinkage, "malloc", *module_);
    functionMap_["malloc"] = mallocFn;
  }

  auto *rawPtr = builder_.CreateCall(functionMap_.at("malloc"), {sizeValue},
                                     "class.alloc");
  auto *typedPtr = builder_.CreateBitCast(rawPtr, ptrTy, "class.obj");

  if (functionMap_.count("zap_arc_register") == 0) {
    auto *registerTy = llvm::FunctionType::get(
        llvm::Type::getVoidTy(ctx_),
        {llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(ctx_))}, false);
    auto *registerFn =
        llvm::Function::Create(registerTy, llvm::Function::ExternalLinkage,
                               "zap_arc_register", *module_);
    functionMap_["zap_arc_register"] = registerFn;
  }
  builder_.CreateCall(functionMap_.at("zap_arc_register"), {rawPtr});

  auto *refCountAddr =
      builder_.CreateStructGEP(objectTy, typedPtr, 0, "refcount.addr");
  builder_.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), 1),
                       refCountAddr);
  auto *weakCountAddr =
      builder_.CreateStructGEP(objectTy, typedPtr, 1, "weakcount.addr");
  builder_.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), 0),
                       weakCountAddr);
  auto *aliveAddr =
      builder_.CreateStructGEP(objectTy, typedPtr, 2, "alive.addr");
  builder_.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx_), 1),
                       aliveAddr);
  auto *gcMarkAddr =
      builder_.CreateStructGEP(objectTy, typedPtr, 3, "gcmark.addr");
  builder_.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx_), 0),
                       gcMarkAddr);
  auto *releaseFnAddr =
      builder_.CreateStructGEP(objectTy, typedPtr, 4, "release.fn.addr");
  auto *releaseFnPtr = builder_.CreateBitCast(
      classReleaseFns_.at(node.classType->getName()),
      llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(ctx_)));
  builder_.CreateStore(releaseFnPtr, releaseFnAddr);
  auto *destroyFnAddr =
      builder_.CreateStructGEP(objectTy, typedPtr, 5, "destroy.fn.addr");
  auto *destroyFnPtr = builder_.CreateBitCast(
      classDestroyFns_.at(node.classType->getName()),
      llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(ctx_)));
  builder_.CreateStore(destroyFnPtr, destroyFnAddr);
  auto *metadataAddr =
      builder_.CreateStructGEP(objectTy, typedPtr, 6, "metadata.addr");
  auto *metadataPtr = builder_.CreateBitCast(
      classMetadataGlobals_.at(node.classType->getName()),
      llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(ctx_)));
  builder_.CreateStore(metadataPtr, metadataAddr);
  auto *vtableAddr =
      builder_.CreateStructGEP(objectTy, typedPtr, 7, "vtable.addr");
  auto *i8PtrTy = llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(ctx_));
  auto *vtablePtrTy = llvm::PointerType::getUnqual(i8PtrTy);
  auto *vtableGlobal = classVTables_.at(node.classType->getName());
  auto *zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
  llvm::Constant *vtableIndices[] = {zero, zero};
  auto *vtablePtr = llvm::ConstantExpr::getInBoundsGetElementPtr(
      vtableGlobal->getValueType(), vtableGlobal, vtableIndices);
  builder_.CreateStore(llvm::ConstantExpr::getBitCast(vtablePtr, vtablePtrTy),
                       vtableAddr);

  for (size_t i = 0; i < node.classType->getFields().size(); ++i) {
    auto *fieldAddr = builder_.CreateStructGEP(objectTy, typedPtr,
                                               static_cast<unsigned>(i + 8));
    builder_.CreateStore(llvm::Constant::getNullValue(
                             toLLVMType(*node.classType->getFields()[i].type)),
                         fieldAddr);
  }

  if (node.constructor) {
    auto *callee = functionMap_.at(node.constructor->linkName);
    std::vector<llvm::Value *> args;
    args.push_back(typedPtr);
    for (size_t i = 0; i < node.arguments.size(); ++i) {
      bool old = evaluateAsAddr_;
      if (i < node.argumentIsRef.size() && node.argumentIsRef[i])
        evaluateAsAddr_ = true;
      node.arguments[i]->accept(*this);
      args.push_back(lastValue_);
      evaluateAsAddr_ = old;
    }
    builder_.CreateCall(callee, args);
  }

  lastValue_ = typedPtr;
}

void LLVMCodeGen::visit(sema::BoundWeakLockExpression &node) {
  node.weakExpression->accept(*this);
  lastValue_ = emitWeakLock(lastValue_, node.weakExpression->type);
}

void LLVMCodeGen::visit(sema::BoundWeakAliveExpression &node) {
  node.weakExpression->accept(*this);
  lastValue_ = emitWeakAlive(lastValue_, node.weakExpression->type);
}

void LLVMCodeGen::visit(sema::BoundIfStatement &node) {
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

  if (elseBB) {
    builder_.CreateCondBr(cond, thenBB, elseBB);
  } else {
    builder_.CreateCondBr(cond, thenBB, mergeBB);
  }

  builder_.SetInsertPoint(thenBB);
  if (node.thenBody) {
    lastValue_ = nullptr;
    node.thenBody->accept(*this);
  }
  if (!builder_.GetInsertBlock()->getTerminator()) {
    builder_.CreateBr(mergeBB);
  }
  thenBB = builder_.GetInsertBlock();

  if (elseBB) {
    builder_.SetInsertPoint(elseBB);
    if (node.elseBody) {
      lastValue_ = nullptr;
      node.elseBody->accept(*this);
    }
    if (!builder_.GetInsertBlock()->getTerminator()) {
      builder_.CreateBr(mergeBB);
    }
    elseBB = builder_.GetInsertBlock();
  }

  builder_.SetInsertPoint(mergeBB);
  lastValue_ = nullptr;
}

void LLVMCodeGen::visit(sema::BoundWhileStatement &node) {
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
  if (!builder_.GetInsertBlock()->getTerminator()) {
    builder_.CreateBr(condBB);
  }

  builder_.SetInsertPoint(endBB);
}

void LLVMCodeGen::visit(sema::BoundBreakStatement &node) {
  if (loopBBStack_.empty())
    return; // binder should have diagnosed
  auto endBB = loopBBStack_.back().second;
  builder_.CreateBr(endBB);
  // Create a new continuation block so subsequent instructions have a place
  auto *contBB = llvm::BasicBlock::Create(ctx_, "after.break", currentFn_);
  builder_.SetInsertPoint(contBB);
}

void LLVMCodeGen::visit(sema::BoundContinueStatement &node) {
  if (loopBBStack_.empty())
    return;
  auto condBB = loopBBStack_.back().first;
  builder_.CreateBr(condBB);
  auto *contBB = llvm::BasicBlock::Create(ctx_, "after.continue", currentFn_);
  builder_.SetInsertPoint(contBB);
}

} // namespace codegen
