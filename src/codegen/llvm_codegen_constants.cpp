#include "../utils/string_type_utils.hpp"
#include "llvm_codegen.hpp"
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Type.h>

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
} // namespace

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
  return llvm::ConstantExpr::getBitCast(gep, ptrTy);
}

llvm::Constant *LLVMCodeGen::lowerZIRConstant(const zir::Constant &constant) {
  if (constant.getType()->getKind() == zir::TypeKind::Record) {
    const auto &rt = static_cast<const zir::RecordType &>(*constant.getType());
    if (isStringRecordName(rt.getName())) {
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
  if (ty->isIntegerTy(8) &&
      constant.getType()->getKind() == zir::TypeKind::Char) {
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
    uint64_t unsignedValue = parseIntegerLiteral(literal);
    if (constant.getType()->isUnsigned()) {
      return llvm::ConstantInt::get(ty, unsignedValue, false);
    }

    return llvm::ConstantInt::get(ty, unsignedValue, true);
  }
  if (ty->isFloatTy()) {
    return llvm::ConstantFP::get(ty, std::stof(literal));
  }
  if (ty->isDoubleTy()) {
    return llvm::ConstantFP::get(ty, std::stod(literal));
  }
  if (ty->isPointerTy()) {
    if (literal == "null") {
      return llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ty));
    }
    auto *address =
        llvm::ConstantInt::get(module_->getDataLayout().getIntPtrType(ctx_),
                               parseIntegerLiteral(literal), false);
    return llvm::ConstantExpr::getIntToPtr(address, ty);
  }
  return llvm::Constant::getNullValue(ty);
}

llvm::Constant *
LLVMCodeGen::lowerZIRAggregateConstant(const zir::AggregateConstant &constant) {
  auto *ty = toLLVMType(*constant.getType());

  auto *structTy = llvm::dyn_cast<llvm::StructType>(ty);
  if (!structTy) {
    return llvm::Constant::getNullValue(ty);
  }

  const auto *recordTy =
      static_cast<const zir::RecordType *>(constant.getType().get());

  const auto &recordFields = recordTy->getFields();
  std::vector<llvm::Constant *> elems(recordFields.size(), nullptr);

  for (const auto &fieldInit : constant.getFields()) {
    int fieldIndex = -1;
    for (size_t i = 0; i < recordFields.size(); ++i) {
      if (recordFields[i].name == fieldInit.name) {
        fieldIndex = static_cast<int>(i);
        break;
      }
    }

    if (fieldIndex < 0 || fieldInit.value == nullptr) {
      continue;
    }

    llvm::Constant *fieldConst = nullptr;
    if (fieldInit.value->getKind() == zir::ValueKind::Constant) {
      fieldConst = lowerZIRConstant(
          static_cast<const zir::Constant &>(*fieldInit.value));
    } else if (fieldInit.value->getKind() ==
               zir::ValueKind::AggregateConstant) {
      fieldConst = lowerZIRAggregateConstant(
          static_cast<const zir::AggregateConstant &>(*fieldInit.value));
    }

    if (fieldConst) {
      auto *expectedFieldTy = toLLVMAggregateFieldType(
          recordFields[static_cast<size_t>(fieldIndex)].type);
      if (fieldConst->getType() != expectedFieldTy) {
        auto sourceFieldType = fieldInit.value->getType();
        auto targetFieldType =
            recordFields[static_cast<size_t>(fieldIndex)].type;
        if (isStringType(sourceFieldType) && isStringType(targetFieldType)) {
          if (auto *srcStruct =
                  llvm::dyn_cast<llvm::ConstantStruct>(fieldConst)) {
            auto *ptr = srcStruct->getAggregateElement((unsigned)0);
            auto *len = srcStruct->getAggregateElement((unsigned)1);
            auto *dstStructTy =
                llvm::dyn_cast<llvm::StructType>(expectedFieldTy);
            if (ptr && len && dstStructTy) {
              fieldConst = llvm::ConstantStruct::get(dstStructTy, {ptr, len});
            }
          }
        } else if (fieldConst->getType()->isIntegerTy() &&
                   expectedFieldTy->isIntegerTy()) {
          unsigned srcBits = fieldConst->getType()->getIntegerBitWidth();
          unsigned dstBits = expectedFieldTy->getIntegerBitWidth();
          if (dstBits < srcBits) {
            fieldConst = llvm::dyn_cast<llvm::Constant>(
                llvm::ConstantExpr::getTrunc(fieldConst, expectedFieldTy));
          } else if (dstBits > srcBits) {
            auto targetFieldType =
                recordFields[static_cast<size_t>(fieldIndex)].type;
            if (targetFieldType->isUnsigned()) {
              fieldConst =
                  llvm::dyn_cast<llvm::Constant>(llvm::ConstantExpr::getCast(
                      llvm::Instruction::ZExt, fieldConst, expectedFieldTy));
            } else {
              fieldConst =
                  llvm::dyn_cast<llvm::Constant>(llvm::ConstantExpr::getCast(
                      llvm::Instruction::SExt, fieldConst, expectedFieldTy));
            }
          }
        } else {
          fieldConst = llvm::dyn_cast<llvm::Constant>(
              llvm::ConstantExpr::getBitCast(fieldConst, expectedFieldTy));
        }
      }
      elems[static_cast<size_t>(fieldIndex)] = fieldConst;
    }
  }

  for (size_t i = 0; i < elems.size(); ++i) {
    if (!elems[i]) {
      elems[i] = llvm::Constant::getNullValue(
          toLLVMAggregateFieldType(recordFields[i].type));
    }
  }

  return llvm::ConstantStruct::get(structTy, elems);
}

llvm::Constant *
LLVMCodeGen::lowerZIRArrayConstant(const zir::ArrayConstant &constant) {
  auto *ty = toLLVMType(*constant.getType());
  auto *arrayTy = llvm::dyn_cast<llvm::ArrayType>(ty);
  if (!arrayTy) {
    return llvm::Constant::getNullValue(ty);
  }

  std::vector<llvm::Constant *> elems;
  elems.reserve(arrayTy->getNumElements());
  for (const auto &element : constant.getElements()) {
    llvm::Constant *elementConst = nullptr;
    if (element->getKind() == zir::ValueKind::Constant) {
      elementConst =
          lowerZIRConstant(static_cast<const zir::Constant &>(*element));
    } else if (element->getKind() == zir::ValueKind::AggregateConstant) {
      elementConst = lowerZIRAggregateConstant(
          static_cast<const zir::AggregateConstant &>(*element));
    } else if (element->getKind() == zir::ValueKind::ArrayConstant) {
      elementConst = lowerZIRArrayConstant(
          static_cast<const zir::ArrayConstant &>(*element));
    }

    if (elementConst) {
      auto *expectedTy = arrayTy->getElementType();
      if (elementConst->getType() != expectedTy) {
        if (elementConst->getType()->isIntegerTy() &&
            expectedTy->isIntegerTy()) {
          auto srcBits = elementConst->getType()->getIntegerBitWidth();
          auto dstBits = expectedTy->getIntegerBitWidth();
          if (dstBits < srcBits) {
            elementConst = llvm::cast<llvm::Constant>(
                llvm::ConstantExpr::getTrunc(elementConst, expectedTy));
          } else if (dstBits > srcBits) {
            elementConst =
                llvm::cast<llvm::Constant>(llvm::ConstantExpr::getCast(
                    llvm::Instruction::SExt, elementConst, expectedTy));
          }
        } else {
          elementConst = llvm::cast<llvm::Constant>(
              llvm::ConstantExpr::getBitCast(elementConst, expectedTy));
        }
      }
    }

    elems.push_back(
        elementConst ? elementConst
                     : llvm::Constant::getNullValue(arrayTy->getElementType()));
  }

  while (elems.size() < arrayTy->getNumElements()) {
    elems.push_back(llvm::Constant::getNullValue(arrayTy->getElementType()));
  }

  return llvm::ConstantArray::get(arrayTy, elems);
}

} // namespace codegen
