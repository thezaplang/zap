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
uint64_t parseIntegerLiteral(const std::string &literal) {
  if (literal.size() > 2 && literal[0] == '0') {
    if (literal[1] == 'x' || literal[1] == 'X') {
      return std::stoull(literal, nullptr, 16);
    }
    if (literal[1] == 'b' || literal[1] == 'B') {
      return std::stoull(literal.substr(2), nullptr, 2);
    }
    if (literal[1] == 'o' || literal[1] == 'O') {
      return std::stoull(literal.substr(2), nullptr, 8);
    }
  }
  return std::stoull(literal, nullptr, 10);
}

bool isStringType(const std::shared_ptr<zir::Type> &type) {
  return zap::text::isStringType(type);
}

bool isStringRecordName(const std::string &full) {
  return zap::text::isStringRecordName(full);
}

bool isVariadicViewType(const std::shared_ptr<zir::Type> &type) {
  return type && type->getKind() == zir::TypeKind::Record &&
         static_cast<zir::RecordType *>(type.get())
                 ->getName()
                 .rfind("__zap_varargs_", 0) == 0;
}

} // namespace

void LLVMCodeGen::visit(sema::BoundCast &node) {
  node.expression->accept(*this);
  auto *src = lastValue_;
  auto *srcTy = src->getType();
  auto *destTy = toLLVMType(*node.type);

  if (isStringType(node.expression->type) && isStringType(node.type)) {
    if (srcTy == destTy) {
      lastValue_ = src;
      return;
    }
    auto *ptr = builder_.CreateExtractValue(src, {0}, "str.cast.ptr");
    auto *len = builder_.CreateExtractValue(src, {1}, "str.cast.len");
    llvm::Value *result = llvm::UndefValue::get(destTy);
    result = builder_.CreateInsertValue(result, ptr, {0}, "str.cast.ptr.i");
    result = builder_.CreateInsertValue(result, len, {1}, "str.cast.len.i");
    lastValue_ = result;
    return;
  }

  if (isStringType(node.expression->type) && destTy->isPointerTy()) {
    auto *ptr = builder_.CreateExtractValue(src, {0}, "str.ptr");
    if (ptr->getType() == destTy) {
      lastValue_ = ptr;
    } else {
      lastValue_ = builder_.CreateBitCast(ptr, destTy);
    }
    return;
  }

  if (srcTy->isPointerTy() && isStringType(node.type)) {
    auto *i8PtrTy = llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(ctx_));
    auto *cstrPtr =
        srcTy == i8PtrTy ? src : builder_.CreateBitCast(src, i8PtrTy);
    std::vector<llvm::Type *> fromCStrParams = {i8PtrTy};
    auto *fromCStrTy = llvm::FunctionType::get(destTy, fromCStrParams, false);
    auto fromCStrCallee =
        module_->getOrInsertFunction("zap_string_from_cstr", fromCStrTy);
    lastValue_ = builder_.CreateCall(fromCStrTy, fromCStrCallee.getCallee(),
                                     {cstrPtr}, "str.from.cstr");
    return;
  }

  if (srcTy == destTy) {
    lastValue_ = src;
    return;
  }

  auto *srcConst = llvm::dyn_cast<llvm::Constant>(src);

  if (srcTy->isPointerTy() && destTy->isPointerTy()) {
    lastValue_ = srcConst ? llvm::ConstantExpr::getBitCast(srcConst, destTy)
                          : builder_.CreateBitCast(src, destTy);
  } else if (srcTy->isPointerTy() && destTy->isIntegerTy()) {
    lastValue_ = srcConst ? llvm::ConstantExpr::getPtrToInt(srcConst, destTy)
                          : builder_.CreatePtrToInt(src, destTy);
  } else if (srcTy->isIntegerTy() && destTy->isPointerTy()) {
    lastValue_ = srcConst ? llvm::ConstantExpr::getIntToPtr(srcConst, destTy)
                          : builder_.CreateIntToPtr(src, destTy);
  } else if (srcTy->isIntegerTy() && destTy->isIntegerTy()) {
    unsigned srcBits = srcTy->getIntegerBitWidth();
    unsigned destBits = destTy->getIntegerBitWidth();

    if (destBits > srcBits) {
      if (node.expression->type->isUnsigned() ||
          node.expression->type->getKind() == zir::TypeKind::Char) {
        lastValue_ = srcConst ? llvm::ConstantExpr::getCast(
                                    llvm::Instruction::ZExt, srcConst, destTy)
                              : builder_.CreateZExt(src, destTy);
      } else {
        lastValue_ = srcConst ? llvm::ConstantExpr::getCast(
                                    llvm::Instruction::SExt, srcConst, destTy)
                              : builder_.CreateSExt(src, destTy);
      }
    } else if (destBits < srcBits) {
      lastValue_ = srcConst ? llvm::ConstantExpr::getTrunc(srcConst, destTy)
                            : builder_.CreateTrunc(src, destTy);
    } else {
      lastValue_ = src;
    }
  } else if (srcTy->isIntegerTy() && destTy->isFloatingPointTy()) {
    if (node.expression->type->isUnsigned()) {
      lastValue_ = srcConst ? llvm::ConstantExpr::getCast(
                                  llvm::Instruction::UIToFP, srcConst, destTy)
                            : builder_.CreateUIToFP(src, destTy);
    } else {
      lastValue_ = srcConst ? llvm::ConstantExpr::getCast(
                                  llvm::Instruction::SIToFP, srcConst, destTy)
                            : builder_.CreateSIToFP(src, destTy);
    }
  } else if (srcTy->isFloatingPointTy() && destTy->isIntegerTy()) {
    if (node.type->isUnsigned()) {
      lastValue_ = srcConst ? llvm::ConstantExpr::getCast(
                                  llvm::Instruction::FPToUI, srcConst, destTy)
                            : builder_.CreateFPToUI(src, destTy);
    } else {
      lastValue_ = srcConst ? llvm::ConstantExpr::getCast(
                                  llvm::Instruction::FPToSI, srcConst, destTy)
                            : builder_.CreateFPToSI(src, destTy);
    }
  } else if (srcTy->isFloatingPointTy() && destTy->isFloatingPointTy()) {
    if (srcTy->getPrimitiveSizeInBits() < destTy->getPrimitiveSizeInBits()) {
      lastValue_ = srcConst ? llvm::ConstantExpr::getCast(
                                  llvm::Instruction::FPExt, srcConst, destTy)
                            : builder_.CreateFPExt(src, destTy);
    } else if (srcTy->getPrimitiveSizeInBits() >
               destTy->getPrimitiveSizeInBits()) {
      lastValue_ = srcConst ? llvm::ConstantExpr::getCast(
                                  llvm::Instruction::FPTrunc, srcConst, destTy)
                            : builder_.CreateFPTrunc(src, destTy);
    } else {
      lastValue_ = src;
    }
  } else {
    lastValue_ = srcConst ? llvm::ConstantExpr::getBitCast(srcConst, destTy)
                          : builder_.CreateBitCast(src, destTy);
  }
}

void LLVMCodeGen::visit(sema::BoundCompoundTargetLoad &node) {
  lastValue_ = builder_.CreateLoad(toLLVMType(*node.type), compoundTargetAddr_);
}

void LLVMCodeGen::visit(sema::BoundLiteral &node) {
  if (node.type->getKind() == zir::TypeKind::Record) {
    const auto &rt = static_cast<const zir::RecordType &>(*node.type);
    if (isStringRecordName(rt.getName())) {
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
    uint64_t unsignedValue = parseIntegerLiteral(node.value);
    if (node.type->isUnsigned()) {
      lastValue_ = llvm::ConstantInt::get(ty, unsignedValue,
                                          /*isSigned=*/false);
    } else {
      lastValue_ = llvm::ConstantInt::get(ty, unsignedValue, /*isSigned=*/true);
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
  auto localIt = localValues_.find(node.symbol->name);
  if (localIt != localValues_.end()) {
    addr = localIt->second;
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

  if (node.op == "-")
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
  else if (node.op == "<<")
    lastValue_ = builder_.CreateShl(lhs, rhs);
  else if (node.op == ">>")
    lastValue_ = isUnsigned ? builder_.CreateLShr(lhs, rhs)
                            : builder_.CreateAShr(lhs, rhs);
  else if (node.op == "&")
    lastValue_ = builder_.CreateAnd(lhs, rhs);
  else if (node.op == "|")
    lastValue_ = builder_.CreateOr(lhs, rhs);
  else if (node.op == "^")
    lastValue_ = builder_.CreateXor(lhs, rhs);
  else if ((node.op == "==" || node.op == "!=") &&
           isStringType(node.left->type) && isStringType(node.right->type)) {
    auto *boolTy = llvm::Type::getInt1Ty(ctx_);
    auto *strTy = lhs->getType();
    auto *eqFnTy = llvm::FunctionType::get(boolTy, {strTy, strTy}, false);
    auto eqCallee = module_->getOrInsertFunction("eq", eqFnTy);
    auto *isEq =
        builder_.CreateCall(eqFnTy, eqCallee.getCallee(), {lhs, rhs}, "str.eq");
    lastValue_ = node.op == "==" ? isEq : builder_.CreateNot(isEq, "str.ne");
  } else if (node.op == "==")
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
  else if (node.op == "+") {
    if (isStringType(node.left->type) || isStringType(node.right->type) ||
        node.left->type->getKind() == zir::TypeKind::Char ||
        node.right->type->getKind() == zir::TypeKind::Char) {
      lastValue_ = emitStringConcat(lhs, rhs, node.left->type, node.right->type,
                                    node.type);
    } else {
      lastValue_ =
          isFP ? builder_.CreateFAdd(lhs, rhs) : builder_.CreateAdd(lhs, rhs);
    }
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
  } else if (node.op == "~") {
    if (!node.type->isInteger()) {
      throw std::runtime_error("Unary '~' requires integer operand");
    }
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
    evaluateAsAddr_ = isRef; // false for value args, true for ref args

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
    auto *vtableAddr = builder_.CreateStructGEP(
        objectTy, selfPtr, kClassVTableIndex, "method.vtable.addr");
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
    if (node.symbol->returnsRef && !evaluateAsAddr_) {
      lastValue_ = builder_.CreateLoad(toLLVMType(*node.symbol->returnType),
                                       lastValue_, "ref.load");
    }
    return;
  }

  lastValue_ = builder_.CreateCall(callee, args);

  if (node.symbol->returnsRef && !evaluateAsAddr_) {
    lastValue_ = builder_.CreateLoad(toLLVMType(*node.symbol->returnType),
                                     lastValue_, "ref.load");
  }

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

void LLVMCodeGen::visit(sema::BoundFunctionReference &node) {
  auto it = functionMap_.find(node.symbol->linkName);
  if (it == functionMap_.end()) {
    // Declare it if not yet seen
    auto zirIt = zirFunctionMap_.find(node.symbol->linkName);
    if (zirIt != zirFunctionMap_.end()) {
      declareZIRFunction(*zirIt->second, false);
      it = functionMap_.find(node.symbol->linkName);
    }
  }
  if (it == functionMap_.end())
    throw std::runtime_error("Unknown function reference: " +
                             node.symbol->linkName);
  lastValue_ = it->second;
}

void LLVMCodeGen::visit(sema::BoundIndirectCall &node) {
  node.callee->accept(*this);
  auto *calleePtr = lastValue_;

  const auto &fpType =
      static_cast<const zir::FunctionPointerType &>(*node.callee->type);
  std::vector<llvm::Type *> paramTypes;
  for (const auto &p : fpType.getParams())
    paramTypes.push_back(toLLVMType(*p));
  auto *fnTy = llvm::FunctionType::get(toLLVMType(*fpType.getReturnType()),
                                       paramTypes, false);

  std::vector<llvm::Value *> args;
  for (auto &arg : node.arguments) {
    arg->accept(*this);
    args.push_back(lastValue_);
  }

  lastValue_ = builder_.CreateCall(fnTy, calleePtr, args);
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
        fieldIndex = static_cast<int>(i + kClassFieldStartIndex);
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

  if (node.left->type->getKind() == zir::TypeKind::Pointer) {
    bool old = evaluateAsAddr_;
    evaluateAsAddr_ = false;
    node.left->accept(*this);
    llvm::Value *basePtr = lastValue_;
    evaluateAsAddr_ = old;

    auto pointerType =
        std::static_pointer_cast<zir::PointerType>(node.left->type);
    auto baseType = pointerType->getBaseType();

    // Some ABI paths pass aggregate parameters as an extra pointer wrapper.
    // Peel one pointer level transparently for member access.
    if (baseType->getKind() == zir::TypeKind::Pointer) {
      auto innerPtrType = std::static_pointer_cast<zir::PointerType>(baseType);
      auto *loaded = builder_.CreateLoad(toLLVMType(*baseType), basePtr,
                                         "member.ptr.unwrap");
      basePtr = loaded;
      baseType = innerPtrType->getBaseType();
    }

    if (baseType->getKind() == zir::TypeKind::Record) {
      auto recordType = std::static_pointer_cast<zir::RecordType>(baseType);
      int fieldIndex = -1;
      const auto &fields = recordType->getFields();
      for (size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].name == node.member) {
          fieldIndex = static_cast<int>(i);
          break;
        }
      }

      if (fieldIndex == -1) {
        throw std::runtime_error("Field '" + node.member +
                                 "' not found in type '" +
                                 baseType->toString() + "'");
      }

      auto *structTy = static_cast<llvm::StructType *>(toLLVMType(*recordType));
      llvm::Value *fieldAddr =
          builder_.CreateStructGEP(structTy, basePtr, fieldIndex, node.member);

      if (evaluateAsAddr_) {
        lastValue_ = fieldAddr;
      } else {
        lastValue_ =
            builder_.CreateLoad(toLLVMType(*node.type), fieldAddr, node.member);
      }
      return;
    }

    if (baseType->getKind() == zir::TypeKind::Class) {
      auto classType = std::static_pointer_cast<zir::ClassType>(baseType);
      int fieldIndex = -1;
      const auto &fields = classType->getFields();
      for (size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].name == node.member) {
          fieldIndex = static_cast<int>(i + kClassFieldStartIndex);
          break;
        }
      }

      if (fieldIndex == -1) {
        throw std::runtime_error("Field '" + node.member +
                                 "' not found in type '" +
                                 baseType->toString() + "'");
      }

      auto *objectStructTy =
          structCache_.at(classType->getCodegenName() + ".obj");
      llvm::Value *fieldAddr = builder_.CreateStructGEP(
          objectStructTy, basePtr, fieldIndex, node.member);

      if (evaluateAsAddr_) {
        lastValue_ = fieldAddr;
      } else {
        lastValue_ =
            builder_.CreateLoad(toLLVMType(*node.type), fieldAddr, node.member);
      }
      return;
    }
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

  auto mallocIt2 = functionMap_.find("malloc");
  if (mallocIt2 == functionMap_.end()) {
    auto *mallocTy = llvm::FunctionType::get(
        llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(ctx_)), {sizeTy},
        false);
    auto *mallocFn = llvm::Function::Create(
        mallocTy, llvm::Function::ExternalLinkage, "malloc", *module_);
    mallocIt2 = functionMap_.emplace("malloc", mallocFn).first;
  }

  auto *rawPtr =
      builder_.CreateCall(mallocIt2->second, {sizeValue}, "class.alloc");
  auto *typedPtr = builder_.CreateBitCast(rawPtr, ptrTy, "class.obj");

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
  auto *metadataAddr = builder_.CreateStructGEP(
      objectTy, typedPtr, kClassMetadataIndex, "metadata.addr");
  auto *metadataPtr = builder_.CreateBitCast(
      classMetadataGlobals_.at(node.classType->getName()),
      llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(ctx_)));
  builder_.CreateStore(metadataPtr, metadataAddr);
  auto *vtableAddr = builder_.CreateStructGEP(objectTy, typedPtr,
                                              kClassVTableIndex, "vtable.addr");
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
    auto *fieldAddr = builder_.CreateStructGEP(
        objectTy, typedPtr, static_cast<unsigned>(i + kClassFieldStartIndex));
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

void LLVMCodeGen::visit(sema::BoundTryExpression &node) {
  (void)node;
  throw std::runtime_error(
      "BoundTryExpression should be lowered to ZIR before LLVMCodeGen.");
}

void LLVMCodeGen::visit(sema::BoundFallbackExpression &node) {
  (void)node;
  throw std::runtime_error(
      "BoundFallbackExpression should be lowered to ZIR before LLVMCodeGen.");
}

void LLVMCodeGen::visit(sema::BoundFailableHandleExpression &node) {
  (void)node;
  throw std::runtime_error("BoundFailableHandleExpression should be lowered to "
                           "ZIR before LLVMCodeGen.");
}

} // namespace codegen
