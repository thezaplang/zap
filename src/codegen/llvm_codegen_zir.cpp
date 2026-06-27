#include "../utils/string_type_utils.hpp"
#include "class_layout.hpp"
#include "llvm_codegen.hpp"
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Type.h>
#include <stdexcept>

namespace codegen {
namespace {
bool isStringType(const std::shared_ptr<zir::Type> &type) {
  return zap::text::isStringType(type);
}

bool isStringLLVMStructType(llvm::Type *ty) {
  auto *st = llvm::dyn_cast_or_null<llvm::StructType>(ty);
  if (!st || !st->hasName()) {
    return false;
  }
  return zap::text::isStringRecordName(st->getName().str());
}
} // namespace

void LLVMCodeGen::generate(const zir::Module &module) {
  initializeModule();

  functionMap_.clear();
  zirFunctionMap_.clear();
  globalValues_.clear();
  zirValueMap_.clear();
  zirBlockMap_.clear();

  for (const auto &type : module.getTypes()) {
    if (type->getKind() == zir::TypeKind::Record ||
        type->getKind() == zir::TypeKind::TaggedUnion ||
        type->getKind() == zir::TypeKind::Class) {
      toLLVMType(*type);
    }
  }

  for (const auto &global : module.getExternalGlobals()) {
    auto *gv = new llvm::GlobalVariable(
        *module_, toLLVMType(*global->getValueType()), false,
        llvm::GlobalVariable::ExternalLinkage, nullptr, global->getLinkName());
    globalValues_[global->getLinkName()] = gv;
  }

  for (const auto &global : module.getGlobals()) {
    auto *valueTy = toLLVMType(*global->getValueType());
    auto *gv = new llvm::GlobalVariable(*module_, valueTy, global->isConstant(),
                                        llvm::GlobalVariable::ExternalLinkage,
                                        llvm::Constant::getNullValue(valueTy),
                                        global->getLinkName());
    globalValues_[global->getLinkName()] = gv;
  }

  for (const auto &global : module.getGlobals()) {
    llvm::Constant *initializer = nullptr;
    if (global->getInitializer()) {
      if (global->getInitializer()->getKind() == zir::ValueKind::Constant) {
        initializer = lowerZIRConstant(
            static_cast<const zir::Constant &>(*global->getInitializer()));
      } else if (global->getInitializer()->getKind() ==
                 zir::ValueKind::AggregateConstant) {
        initializer = lowerZIRAggregateConstant(
            static_cast<const zir::AggregateConstant &>(
                *global->getInitializer()));
      } else if (global->getInitializer()->getKind() ==
                 zir::ValueKind::ArrayConstant) {
        initializer = lowerZIRArrayConstant(
            static_cast<const zir::ArrayConstant &>(*global->getInitializer()));
      } else if (global->getInitializer()->getKind() ==
                 zir::ValueKind::GlobalAddress) {
        const auto &address =
            static_cast<const zir::GlobalAddress &>(*global->getInitializer());
        auto target = globalValues_.find(address.getLinkName());
        if (target != globalValues_.end()) {
          initializer = target->second;
          if (address.getArrayIndex()) {
            auto *zero =
                llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), 0);
            auto *index = llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_),
                                                 *address.getArrayIndex());
            llvm::Constant *indices[] = {zero, index};
            initializer = llvm::ConstantExpr::getInBoundsGetElementPtr(
                target->second->getValueType(), target->second, indices);
          }
        }
      }
    }
    if (!initializer) {
      initializer =
          llvm::Constant::getNullValue(toLLVMType(*global->getValueType()));
    }

    globalValues_.at(global->getLinkName())->setInitializer(initializer);
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
      finalizeClassStruct(static_cast<const zir::ClassType &>(*type));
    }
  }
  computeCyclicClasses(module);
  for (const auto &type : module.getTypes()) {
    if (type->getKind() == zir::TypeKind::Class) {
      ensureClassArcSupport(std::static_pointer_cast<zir::ClassType>(type));
    }
  }
  for (const auto &func : module.getFunctions()) {
    emitZIRFunction(*func);
  }
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

llvm::Value *
LLVMCodeGen::lowerZIRValue(const std::shared_ptr<zir::Value> &value) {
  if (!value) {
    return nullptr;
  }
  if (value->getKind() == zir::ValueKind::Constant) {
    return lowerZIRConstant(static_cast<const zir::Constant &>(*value));
  }
  if (value->getKind() == zir::ValueKind::AggregateConstant) {
    return lowerZIRAggregateConstant(
        static_cast<const zir::AggregateConstant &>(*value));
  }
  if (value->getKind() == zir::ValueKind::ArrayConstant) {
    return lowerZIRArrayConstant(
        static_cast<const zir::ArrayConstant &>(*value));
  }
  if (value->getKind() == zir::ValueKind::Global) {
    const auto &g = static_cast<const zir::Global &>(*value);
    if (g.getValueType()->getKind() == zir::TypeKind::FunctionPointer) {
      auto it = functionMap_.find(g.getLinkName());
      if (it != functionMap_.end())
        return it->second;
      auto zirIt = zirFunctionMap_.find(g.getLinkName());
      if (zirIt != zirFunctionMap_.end()) {
        declareZIRFunction(*zirIt->second, false);
        return functionMap_.at(g.getLinkName());
      }
    }
    return globalValues_.at(g.getLinkName());
  }

  auto it = zirValueMap_.find(value.get());
  if (it == zirValueMap_.end()) {
    throw std::runtime_error("unmapped ZIR value: " + value->getName());
  }
  return it->second;
}

llvm::Value *
LLVMCodeGen::lowerZIRRValue(const std::shared_ptr<zir::Value> &value) {
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

llvm::Value *
LLVMCodeGen::lowerZIRCast(llvm::Value *src,
                          const std::shared_ptr<zir::Type> &sourceType,
                          const std::shared_ptr<zir::Type> &targetType) {
  auto *srcTy = src->getType();
  auto *destTy = toLLVMType(*targetType);

  if (isStringType(sourceType) && isStringType(targetType)) {
    if (srcTy == destTy) {
      return src;
    }
    auto *ptr = builder_.CreateExtractValue(src, {0}, "zir.cast.str.ptr");
    auto *len = builder_.CreateExtractValue(src, {1}, "zir.cast.str.len");
    llvm::Value *result = llvm::UndefValue::get(destTy);
    result = builder_.CreateInsertValue(result, ptr, {0}, "zir.cast.str.ptr.i");
    result = builder_.CreateInsertValue(result, len, {1}, "zir.cast.str.len.i");
    return result;
  }

  if (isStringType(sourceType) && destTy->isPointerTy()) {
    auto *ptr = builder_.CreateExtractValue(src, {0}, "zir.cast.str.ptr");
    return ptr->getType() == destTy ? ptr : builder_.CreateBitCast(ptr, destTy);
  }
  if (srcTy->isPointerTy() && isStringType(targetType)) {
    auto *i8PtrTy = llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(ctx_));
    auto *cstrPtr =
        srcTy == i8PtrTy ? src : builder_.CreateBitCast(src, i8PtrTy);
    std::vector<llvm::Type *> fromCStrParams = {i8PtrTy};
    auto *fromCStrTy = llvm::FunctionType::get(destTy, fromCStrParams, false);
    auto fromCStrCallee =
        module_->getOrInsertFunction("zap_string_from_cstr", fromCStrTy);
    return builder_.CreateCall(fromCStrTy, fromCStrCallee.getCallee(),
                               {cstrPtr}, "zir.cast.cstr.to.string");
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
      bool shouldZeroExtend = sourceType->isUnsigned() ||
                              sourceType->getKind() == zir::TypeKind::Bool ||
                              sourceType->getKind() == zir::TypeKind::Char;
      return shouldZeroExtend ? builder_.CreateZExt(src, destTy)
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
      isBorrowedSelf = !currentZIRFunction_->ownerTypeName.empty() &&
                       currentZIRFunction_->getArguments()[zirParamSpillIndex_]
                               ->getRawName() == "self";
    }
    auto *alloca = createEntryAlloca(
        currentFn_,
        static_cast<const Register &>(*allocaInst.getResult()).getRawName(),
        toLLVMType(*allocaInst.getAllocatedType()));
    zirValueMap_[allocaInst.getResult().get()] = alloca;
    if (isClassType(allocaInst.getAllocatedType())) {
      builder_.CreateStore(llvm::Constant::getNullValue(
                               toLLVMType(*allocaInst.getAllocatedType())),
                           alloca);
      if (isParamSpill) {
        zirClassParamAllocas_.insert(allocaInst.getResult().get());
        zirPendingClassParamInitAllocas_.insert(allocaInst.getResult().get());
      }
      if (!isBorrowedSelf) {
        zirFunctionClassLocals_.push_back(
            {allocaInst.getAllocatedType(), alloca});
      }
    } else if (isOwnedStringType(allocaInst.getAllocatedType())) {
      builder_.CreateStore(llvm::Constant::getNullValue(
                               toLLVMType(*allocaInst.getAllocatedType())),
                           alloca);
      zirFunctionStringLocals_.push_back(
          {allocaInst.getAllocatedType(), alloca});
    }
    if (isParamSpill) {
      ++zirParamSpillIndex_;
    }
    return;
  }
  case OpCode::Load: {
    const auto &loadInst = static_cast<const LoadInst &>(inst);
    auto *src = lowerZIRValue(loadInst.getSource());
    auto *value = builder_.CreateLoad(
        toLLVMType(*loadInst.getResult()->getType()), src,
        static_cast<const Register &>(*loadInst.getResult()).getRawName());
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
    bool skipReleaseOld = storeInst.initStore();
    if (storeInst.bypassArc()) {
      builder_.CreateStore(src, dst);
    } else if (valueType && isClassType(valueType)) {
      if (zirPendingClassParamInitAllocas_.count(
              storeInst.getDestination().get()) > 0) {
        builder_.CreateStore(src, dst);
        zirPendingClassParamInitAllocas_.erase(
            storeInst.getDestination().get());
      } else {
        bool valueIsOwned =
            zirOwnedClassValues_.count(storeInst.getSource().get()) > 0;
        emitStoreWithArc(dst, src, valueType, valueIsOwned, skipReleaseOld);
        if (valueIsOwned) {
          zirOwnedClassValues_.erase(storeInst.getSource().get());
        }
      }
    } else if (valueType && isOwnedStringType(valueType)) {
      bool valueIsOwned =
          zirOwnedStringValues_.count(storeInst.getSource().get()) > 0;
      emitStoreWithStringArc(dst, src, valueType, valueIsOwned, skipReleaseOld);
      if (valueIsOwned) {
        zirOwnedStringValues_.erase(storeInst.getSource().get());
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
  case OpCode::URem:
  case OpCode::Shl:
  case OpCode::LShr:
  case OpCode::AShr:
  case OpCode::BitAnd:
  case OpCode::BitOr:
  case OpCode::BitXor: {
    const auto &binaryInst = static_cast<const BinaryInst &>(inst);
    auto *lhs = lowerZIRRValue(binaryInst.getLhs());
    auto *rhs = lowerZIRRValue(binaryInst.getRhs());
    llvm::Value *result = nullptr;
    bool lhsIsPointer =
        binaryInst.getLhs()->getType()->getKind() == zir::TypeKind::Pointer;
    bool rhsIsPointer =
        binaryInst.getRhs()->getType()->getKind() == zir::TypeKind::Pointer;
    switch (inst.getOpCode()) {
    case OpCode::Add:
      if (isStringType(binaryInst.getLhs()->getType()) ||
          isStringType(binaryInst.getRhs()->getType()) ||
          binaryInst.getLhs()->getType()->getKind() == zir::TypeKind::Char ||
          binaryInst.getRhs()->getType()->getKind() == zir::TypeKind::Char) {
        result = emitStringConcat(lhs, rhs, binaryInst.getLhs()->getType(),
                                  binaryInst.getRhs()->getType(),
                                  binaryInst.getResult()->getType());
        if (isOwnedStringType(binaryInst.getResult()->getType())) {
          zirOwnedStringValues_.insert(binaryInst.getResult().get());
        }
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
      } else if (lhs->getType()->isFloatingPointTy()) {
        result = builder_.CreateFAdd(lhs, rhs);
      } else {
        result = builder_.CreateAdd(lhs, rhs);
      }
      break;
    case OpCode::Sub:
      if (lhsIsPointer && rhsIsPointer) {
        auto pointerType = std::static_pointer_cast<zir::PointerType>(
            binaryInst.getLhs()->getType());
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
        auto pointerType = std::static_pointer_cast<zir::PointerType>(
            binaryInst.getLhs()->getType());
        auto *elemTy = toLLVMType(*pointerType->getBaseType());
        auto *indexTy = llvm::Type::getInt64Ty(ctx_);
        auto *index = builder_.CreateIntCast(rhs, indexTy, /*isSigned=*/true);
        index = builder_.CreateNeg(index);
        result = builder_.CreateInBoundsGEP(elemTy, lhs, index);
      } else if (lhs->getType()->isFloatingPointTy()) {
        result = builder_.CreateFSub(lhs, rhs);
      } else {
        result = builder_.CreateSub(lhs, rhs);
      }
      break;
    case OpCode::Mul:
      result = lhs->getType()->isFloatingPointTy()
                   ? builder_.CreateFMul(lhs, rhs)
                   : builder_.CreateMul(lhs, rhs);
      break;
    case OpCode::SDiv:
      result = lhs->getType()->isFloatingPointTy()
                   ? builder_.CreateFDiv(lhs, rhs)
                   : builder_.CreateSDiv(lhs, rhs);
      break;
    case OpCode::UDiv:
      result = lhs->getType()->isFloatingPointTy()
                   ? builder_.CreateFDiv(lhs, rhs)
                   : builder_.CreateUDiv(lhs, rhs);
      break;
    case OpCode::SRem:
      result = lhs->getType()->isFloatingPointTy()
                   ? builder_.CreateFRem(lhs, rhs)
                   : builder_.CreateSRem(lhs, rhs);
      break;
    case OpCode::URem:
      result = lhs->getType()->isFloatingPointTy()
                   ? builder_.CreateFRem(lhs, rhs)
                   : builder_.CreateURem(lhs, rhs);
      break;
    case OpCode::Shl:
      result = builder_.CreateShl(lhs, rhs);
      break;
    case OpCode::LShr:
      result = builder_.CreateLShr(lhs, rhs);
      break;
    case OpCode::AShr:
      result = builder_.CreateAShr(lhs, rhs);
      break;
    case OpCode::BitAnd:
      result = builder_.CreateAnd(lhs, rhs);
      break;
    case OpCode::BitOr:
      result = builder_.CreateOr(lhs, rhs);
      break;
    case OpCode::BitXor:
      result = builder_.CreateXor(lhs, rhs);
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
      if (lhsTy->isIntegerTy() && rhsTy->isIntegerTy()) {
        unsigned lhsBits = lhsTy->getIntegerBitWidth();
        unsigned rhsBits = rhsTy->getIntegerBitWidth();
        if (lhsBits < rhsBits) {
          bool lhsUnsigned = cmpInst.getLhs() && cmpInst.getLhs()->getType()
                                 ? cmpInst.getLhs()->getType()->isUnsigned()
                                 : false;
          lhs = lhsUnsigned ? builder_.CreateZExt(lhs, rhsTy)
                            : builder_.CreateSExt(lhs, rhsTy);
          lhsTy = lhs->getType();
        } else if (rhsBits < lhsBits) {
          bool rhsUnsigned = cmpInst.getRhs() && cmpInst.getRhs()->getType()
                                 ? cmpInst.getRhs()->getType()->isUnsigned()
                                 : false;
          rhs = rhsUnsigned ? builder_.CreateZExt(rhs, lhsTy)
                            : builder_.CreateSExt(rhs, lhsTy);
          rhsTy = rhs->getType();
        }
      } else if (lhsTy->isFloatingPointTy() && rhsTy->isFloatingPointTy()) {
        unsigned lhsBits = lhsTy->getPrimitiveSizeInBits();
        unsigned rhsBits = rhsTy->getPrimitiveSizeInBits();
        if (lhsBits < rhsBits) {
          lhs = builder_.CreateFPExt(lhs, rhsTy);
          lhsTy = lhs->getType();
        } else if (rhsBits < lhsBits) {
          rhs = builder_.CreateFPExt(rhs, lhsTy);
          rhsTy = rhs->getType();
        }
      } else if (lhsTy->isIntegerTy() && rhsTy->isFloatingPointTy()) {
        bool lhsUnsigned = cmpInst.getLhs() && cmpInst.getLhs()->getType()
                               ? cmpInst.getLhs()->getType()->isUnsigned()
                               : false;
        lhs = lhsUnsigned ? builder_.CreateUIToFP(lhs, rhsTy)
                          : builder_.CreateSIToFP(lhs, rhsTy);
        lhsTy = lhs->getType();
      } else if (lhsTy->isFloatingPointTy() && rhsTy->isIntegerTy()) {
        bool rhsUnsigned = cmpInst.getRhs() && cmpInst.getRhs()->getType()
                               ? cmpInst.getRhs()->getType()->isUnsigned()
                               : false;
        rhs = rhsUnsigned ? builder_.CreateUIToFP(rhs, lhsTy)
                          : builder_.CreateSIToFP(rhs, lhsTy);
        rhsTy = rhs->getType();
      }
    }
    if (lhsTy != rhsTy) {
      throw std::runtime_error(
          "ZIR cmp operand type mismatch after coercion: " +
          cmpInst.getLhs()->getTypeName() + " vs " +
          cmpInst.getRhs()->getTypeName());
    }
    if (isStringLLVMStructType(lhsTy) && isStringLLVMStructType(rhsTy) &&
        (pred == "eq" || pred == "ne")) {
      auto *boolTy = llvm::Type::getInt1Ty(ctx_);
      auto *strTy = lhsTy;
      auto *eqFnTy = llvm::FunctionType::get(boolTy, {strTy, strTy}, false);
      auto eqCallee = module_->getOrInsertFunction("eq", eqFnTy);
      auto *isEq = builder_.CreateCall(eqFnTy, eqCallee.getCallee(), {lhs, rhs},
                                       "str.eq");
      zirValueMap_[cmpInst.getResult().get()] =
          pred == "eq" ? isEq : builder_.CreateNot(isEq, "str.ne");
      return;
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
      } else if (isOwnedStringType(retType) &&
                 zirOwnedStringValues_.count(returnInst.getValue().get()) ==
                     0) {
        retValue = emitStringRetainIfNeeded(retValue, retType);
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
      for (auto it = zirFunctionStringLocals_.rbegin();
           it != zirFunctionStringLocals_.rend(); ++it) {
        auto *value = builder_.CreateLoad(toLLVMType(*it->first), it->second,
                                          "zir.str.ret.release");
        emitStringReleaseIfNeeded(value, it->first);
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
      for (auto it = zirFunctionStringLocals_.rbegin();
           it != zirFunctionStringLocals_.rend(); ++it) {
        auto *value = builder_.CreateLoad(toLLVMType(*it->first), it->second,
                                          "zir.str.ret.release");
        emitStringReleaseIfNeeded(value, it->first);
      }
      builder_.CreateRetVoid();
    }
    return;
  }
  case OpCode::Call: {
    const auto &callInst = static_cast<const CallInst &>(inst);
    std::vector<llvm::Value *> args;

    if (callInst.isIndirect()) {
      auto *calleePtr = lowerZIRRValue(callInst.getCalleeValue());
      const auto &fpType = static_cast<const zir::FunctionPointerType &>(
          *callInst.getCalleeValue()->getType());
      std::vector<llvm::Type *> paramTypes;
      for (const auto &p : fpType.getParams())
        paramTypes.push_back(toLLVMType(*p));
      auto *fnTy = llvm::FunctionType::get(toLLVMType(*fpType.getReturnType()),
                                           paramTypes, false);
      for (const auto &arg : callInst.getArguments())
        args.push_back(lowerZIRRValue(arg));
      auto *call = builder_.CreateCall(fnTy, calleePtr, args);
      if (callInst.getResult()) {
        zirValueMap_[callInst.getResult().get()] = call;
        if (callInst.returnsRef()) {
          refReturnValues_.insert(callInst.getResult().get());
        }
      }
      return;
    }

    auto calleeIt = functionMap_.find(callInst.getFunctionName());
    if (calleeIt == functionMap_.end()) {
      auto zirDeclIt = zirFunctionMap_.find(callInst.getFunctionName());
      if (zirDeclIt != zirFunctionMap_.end()) {
        declareZIRFunction(*zirDeclIt->second, true);
        calleeIt = functionMap_.find(callInst.getFunctionName());
      }
    }
    if (calleeIt == functionMap_.end()) {
      auto *existing = module_->getFunction(callInst.getFunctionName());
      if (!existing) {
        std::vector<llvm::Type *> inferredParamTypes;
        inferredParamTypes.reserve(callInst.getArguments().size());
        for (size_t i = 0; i < callInst.getArguments().size(); ++i) {
          bool isRef = i < callInst.getArgumentIsRef().size() &&
                       callInst.getArgumentIsRef()[i];
          auto *argTy =
              isRef ? lowerZIRValue(callInst.getArguments()[i])->getType()
                    : lowerZIRRValue(callInst.getArguments()[i])->getType();
          inferredParamTypes.push_back(argTy);
        }

        llvm::Type *retTy = llvm::Type::getVoidTy(ctx_);
        if (callInst.getResult()) {
          retTy = toLLVMType(*callInst.getResult()->getType());
        }

        auto *fnTy = llvm::FunctionType::get(retTy, inferredParamTypes, false);
        existing = llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage,
                                          callInst.getFunctionName(), *module_);
      }

      functionMap_[callInst.getFunctionName()] = existing;
      calleeIt = functionMap_.find(callInst.getFunctionName());
    }
    if (calleeIt == functionMap_.end()) {
      throw std::runtime_error("missing callee in LLVM function map: " +
                               callInst.getFunctionName());
    }
    auto *callee = calleeIt->second;
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
      bool isRef = i < callInst.getArgumentIsRef().size() &&
                   callInst.getArgumentIsRef()[i];
      auto *arg = isRef ? lowerZIRValue(callInst.getArguments()[i])
                        : lowerZIRRValue(callInst.getArguments()[i]);
      std::shared_ptr<zir::Type> calleeParamType = nullptr;
      if (zirIt != zirFunctionMap_.end() && i < fixedParamCount &&
          i < zirIt->second->getArguments().size()) {
        calleeParamType = zirIt->second->getArguments()[i]->getType();
      }
      llvm::Type *paramTy = nullptr;
      if (i < fixedParamCount &&
          static_cast<unsigned>(i) < calleeTy->getNumParams()) {
        paramTy = calleeTy->getParamType(static_cast<unsigned>(i));
      }
      if (paramTy && arg->getType() != paramTy) {
        if (isStringLLVMStructType(arg->getType()) &&
            isStringLLVMStructType(paramTy)) {
          auto *ptr = builder_.CreateExtractValue(arg, {0}, "zir.call.str.ptr");
          auto *len = builder_.CreateExtractValue(arg, {1}, "zir.call.str.len");
          llvm::Value *converted = llvm::UndefValue::get(paramTy);
          converted = builder_.CreateInsertValue(converted, ptr, {0},
                                                 "zir.call.str.ptr.i");
          converted = builder_.CreateInsertValue(converted, len, {1},
                                                 "zir.call.str.len.i");
          arg = converted;
        }
        auto *argPtrTy = llvm::dyn_cast<llvm::PointerType>(arg->getType());
        if (argPtrTy && !paramTy->isPointerTy()) {
          arg = builder_.CreateLoad(paramTy, arg, "zir.call.arg");
        }
      } else if (isCVariadic && i >= fixedParamCount) {
        // C varargs default argument promotions:
        // - integer types narrower than int -> int
        // - float -> double
        // (already-typed varargs keep their value category)
        auto *argTy = arg->getType();
        if (argTy->isIntegerTy()) {
          unsigned bits = argTy->getIntegerBitWidth();
          if (bits < 32) {
            auto argType = callInst.getArguments()[i]
                               ? callInst.getArguments()[i]->getType()
                               : nullptr;
            bool isUnsignedArg =
                argType && (argType->isUnsigned() ||
                            argType->getKind() == zir::TypeKind::Bool ||
                            argType->getKind() == zir::TypeKind::Char);
            auto *i32Ty = llvm::Type::getInt32Ty(ctx_);
            arg = isUnsignedArg
                      ? builder_.CreateZExt(arg, i32Ty, "cvararg.zext")
                      : builder_.CreateSExt(arg, i32Ty, "cvararg.sext");
          }
        } else if (argTy->isFloatTy()) {
          auto *f64Ty = llvm::Type::getDoubleTy(ctx_);
          arg = builder_.CreateFPExt(arg, f64Ty, "cvararg.fpext");
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
      size_t explicitVariadicCount =
          callInst.getArguments().size() - fixedParamCount;

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
          totalCount = builder_.CreateAdd(explicitCount, forwardedCount,
                                          "varargs.total");
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
          auto *src = builder_.CreateInBoundsGEP(elemTy, forwardedData,
                                                 indexPhi, "varargs.src");
          auto *srcVal = builder_.CreateLoad(elemTy, src, "varargs.load");
          auto *dstIndex = builder_.CreateAdd(indexPhi, explicitCount);
          auto *dst = builder_.CreateInBoundsGEP(elemTy, buffer, dstIndex,
                                                 "varargs.dst");
          builder_.CreateStore(srcVal, dst);
          auto *next = builder_.CreateAdd(
              indexPhi,
              llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 1));
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
        auto baseType = std::static_pointer_cast<zir::PointerType>(receiverType)
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
            objectTy, selfPtr, kClassVTableIndex, "zir.method.vtable.addr");
        auto *i8PtrTy =
            llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(ctx_));
        auto *vtablePtrTy = llvm::PointerType::getUnqual(i8PtrTy);
        auto *vtablePtr =
            builder_.CreateLoad(vtablePtrTy, vtableAddr, "zir.method.vtable");
        auto *slotAddr = builder_.CreateInBoundsGEP(
            i8PtrTy, vtablePtr,
            llvm::ConstantInt::get(
                llvm::Type::getInt32Ty(ctx_),
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
      if (callInst.returnsRef()) {
        refReturnValues_.insert(callInst.getResult().get());
      }
      if (isClassType(callInst.getResult()->getType())) {
        zirOwnedClassValues_.insert(callInst.getResult().get());
      } else if (isOwnedStringType(callInst.getResult()->getType())) {
        zirOwnedStringValues_.insert(callInst.getResult().get());
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
    } else if (baseType->getKind() == zir::TypeKind::TaggedUnion) {
      auto taggedUnionType =
          std::static_pointer_cast<zir::TaggedUnionType>(baseType);
      auto *structTy =
          llvm::cast<llvm::StructType>(toLLVMType(*taggedUnionType));
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
          objectTy, objectPtr,
          static_cast<unsigned>(gepInst.getIndex() + kClassFieldStartIndex),
          static_cast<const Register &>(*gepInst.getResult()).getRawName());
    } else {
      llvm::Value *basePtr = ptr;
      llvm::Value *index =
          gepInst.getIndexValue()
              ? lowerZIRRValue(gepInst.getIndexValue())
              : llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_),
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
    auto *phi = builder_.CreatePHI(
        toLLVMType(*phiInst.getResult()->getType()),
        phiInst.getIncoming().size(),
        static_cast<const Register &>(*phiInst.getResult()).getRawName());
    bool phiOwnsClassValue = isClassType(phiInst.getResult()->getType());
    for (const auto &incoming : phiInst.getIncoming()) {
      auto blockIt = zirBlockExitMap_.find(incoming.first);
      auto *incomingBlock = blockIt != zirBlockExitMap_.end()
                                ? blockIt->second
                                : zirBlockMap_.at(incoming.first);
      phi->addIncoming(lowerZIRValue(incoming.second), incomingBlock);
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
    auto *result =
        lowerZIRCast(lowerZIRRValue(castInst.getSource()),
                     castInst.getSource()->getType(), castInst.getTargetType());
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
  case OpCode::Alloc: {
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

    auto mallocIt = functionMap_.find("malloc");
    if (mallocIt == functionMap_.end()) {
      auto *mallocTy = llvm::FunctionType::get(
          llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(ctx_)), {sizeTy},
          false);
      auto *mallocFn = llvm::Function::Create(
          mallocTy, llvm::Function::ExternalLinkage, "malloc", *module_);
      mallocIt = functionMap_.emplace("malloc", mallocFn).first;
    }

    auto *rawPtr =
        builder_.CreateCall(mallocIt->second, {sizeValue}, "class.alloc");
    auto *typedPtr = builder_.CreateBitCast(rawPtr, ptrTy, "class.obj");

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
    builder_.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx_), 1),
                         aliveAddr);
    auto *gcMarkAddr =
        builder_.CreateStructGEP(objectTy, typedPtr, 3, "gcmark.addr");
    builder_.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx_), 0),
                         gcMarkAddr);
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
    auto *metadataAddr = builder_.CreateStructGEP(
        objectTy, typedPtr, kClassMetadataIndex, "metadata.addr");
    auto *metadataPtr = builder_.CreateBitCast(
        classMetadataGlobals_.at(classType->getName()),
        llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(ctx_)));
    builder_.CreateStore(metadataPtr, metadataAddr);
    auto *vtableAddr = builder_.CreateStructGEP(
        objectTy, typedPtr, kClassVTableIndex, "vtable.addr");
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
          objectTy, typedPtr, static_cast<unsigned>(i + kClassFieldStartIndex));
      builder_.CreateStore(llvm::Constant::getNullValue(
                               toLLVMType(*classType->getFields()[i].type)),
                           fieldAddr);
    }

    zirValueMap_[allocInst.getResult().get()] = typedPtr;
    zirOwnedClassValues_.insert(allocInst.getResult().get());
    return;
  }
  case OpCode::InlineAsm: {
    const auto &asmInst = static_cast<const InlineAsmInst &>(inst);
    std::vector<std::string> outConstraints, inConstraints;
    std::vector<llvm::Value *> outAddrs, inValues;
    std::vector<llvm::Type *> outValueTypes;
    for (const auto &out : asmInst.getOutputs()) {
      outConstraints.push_back(out.constraint);
      outAddrs.push_back(lowerZIRValue(out.value));
      outValueTypes.push_back(toLLVMType(*out.valueType));
    }
    for (const auto &in : asmInst.getInputs()) {
      inConstraints.push_back(in.constraint);
      inValues.push_back(lowerZIRRValue(in.value));
    }
    buildInlineAsmCall(asmInst.getAssembly(), outConstraints, outAddrs,
                       outValueTypes, inConstraints, inValues,
                       asmInst.getClobbers());
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
  zirOwnedStringValues_.clear();
  zirClassParamAllocas_.clear();
  zirPendingClassParamInitAllocas_.clear();
  zirFunctionClassLocals_.clear();
  zirFunctionStringLocals_.clear();
  zirParamSpillIndex_ = 0;
  zirBlockExitMap_.clear();

  auto llvmArgIt = currentFn_->arg_begin();
  if (!freestanding_ && fn.name == "main") {
    auto *i32Ty = llvm::Type::getInt32Ty(ctx_);
    auto *i8PtrTy = llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(ctx_));
    auto *argvTy = llvm::PointerType::getUnqual(i8PtrTy);

    llvmArgIt->setName("argc");
    llvm::Value *argcValue = &*llvmArgIt++;
    llvmArgIt->setName("argv");
    llvm::Value *argvValue = &*llvmArgIt++;

    auto setArgsIt = functionMap_.find("__zap_process_set_args");
    if (setArgsIt == functionMap_.end()) {
      auto *ft = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_),
                                         {i32Ty, argvTy}, false);
      auto *setArgsFn =
          llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                                 "__zap_process_set_args", *module_);
      setArgsIt =
          functionMap_.emplace("__zap_process_set_args", setArgsFn).first;
    }
    builder_.SetInsertPoint(
        llvm::BasicBlock::Create(ctx_, "entry", currentFn_));
    builder_.CreateCall(
        setArgsIt->second,
        {builder_.CreateIntCast(argcValue, i32Ty, true), argvValue});
    builder_.CreateBr(llvm::BasicBlock::Create(
        ctx_, fn.getBlocks().front()->label, currentFn_));
    zirBlockMap_[fn.getBlocks().front()->label] = &currentFn_->back();
  }

  std::vector<llvm::Value *> physicalArgs;
  for (; llvmArgIt != currentFn_->arg_end(); ++llvmArgIt) {
    physicalArgs.push_back(&*llvmArgIt);
  }

  for (size_t i = 0; i < fn.getBlocks().size(); ++i) {
    const auto &block = fn.getBlocks()[i];
    if (!freestanding_ && fn.name == "main" && i == 0 &&
        zirBlockMap_.count(block->label) != 0) {
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
      auto *sliceTy =
          static_cast<llvm::StructType *>(toLLVMType(*arg->getType()));
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

    auto *endBlock = builder_.GetInsertBlock();
    if (endBlock && endBlock->getTerminator()) {
      zirBlockExitMap_[block->label] = endBlock;
    } else {
      zirBlockExitMap_[block->label] = zirBlockMap_.at(block->label);
    }
  }

  currentFn_ = nullptr;
  currentZIRFunction_ = nullptr;
  zirBlockMap_.clear();
  zirValueMap_.clear();
  zirOwnedClassValues_.clear();
  zirOwnedStringValues_.clear();
  zirBlockExitMap_.clear();
  zirClassParamAllocas_.clear();
  zirPendingClassParamInitAllocas_.clear();
  zirFunctionClassLocals_.clear();
  zirFunctionStringLocals_.clear();
}

} // namespace codegen
