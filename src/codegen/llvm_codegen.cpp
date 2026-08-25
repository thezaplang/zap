#include "llvm_codegen.hpp"
#include "../ir/string_type.hpp"
#include "class_arc_emitter.hpp"
#include "class_layout.hpp"
#include <cassert>
#include <cctype>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>
#include <optional>
#include <stdexcept>
#include <utility>

namespace codegen {
namespace {
bool isStringType(const std::shared_ptr<zir::Type> &type) {
  return zir::isIntrinsicStringType(type);
}

void collectStrongReferencedClasses(
    const std::shared_ptr<zir::Type> &type,
    std::unordered_set<std::string> &classNames) {
  if (!type) {
    return;
  }

  if (type->getKind() == zir::TypeKind::Class) {
    auto classType = std::static_pointer_cast<zir::ClassType>(type);
    if (!classType->isWeak()) {
      classNames.insert(classType->getCodegenName());
    }
    return;
  }

  if (type->getKind() == zir::TypeKind::Record) {
    const auto &record = static_cast<const zir::RecordType &>(*type);
    for (const auto &field : record.getFields()) {
      collectStrongReferencedClasses(field.type, classNames);
    }
  } else if (type->getKind() == zir::TypeKind::Array) {
    collectStrongReferencedClasses(
        static_cast<const zir::ArrayType &>(*type).getBaseType(), classNames);
  } else if (type->getKind() == zir::TypeKind::TaggedUnion) {
    const auto &taggedUnion = static_cast<const zir::TaggedUnionType &>(*type);
    for (const auto &variant : taggedUnion.getVariants()) {
      collectStrongReferencedClasses(variant.payloadType, classNames);
    }
  }
}

std::string lowerGccAsmTemplateToLLVM(const std::string &assembly) {
  std::string lowered;
  lowered.reserve(assembly.size());

  for (size_t i = 0; i < assembly.size(); ++i) {
    if (assembly[i] != '%' || i + 1 == assembly.size()) {
      lowered.push_back(assembly[i]);
      continue;
    }

    char next = assembly[i + 1];
    if (std::isdigit(static_cast<unsigned char>(next))) {
      lowered.push_back('$');
      while (i + 1 < assembly.size() &&
             std::isdigit(static_cast<unsigned char>(assembly[i + 1]))) {
        lowered.push_back(assembly[++i]);
      }
    } else if (next == '%') {
      lowered.push_back('%');
      ++i;
    } else {
      lowered.push_back(assembly[i]);
    }
  }

  return lowered;
}

std::string lowerX86GccConstraintToLLVM(const std::string &constraint) {
  std::string lowered;
  lowered.reserve(constraint.size() + 8);

  for (size_t i = 0; i < constraint.size(); ++i) {
    if (constraint[i] == '{') {
      while (i < constraint.size()) {
        lowered.push_back(constraint[i]);
        if (constraint[i] == '}')
          break;
        ++i;
      }
      continue;
    }

    switch (constraint[i]) {
    case 'a':
      lowered += "{ax}";
      break;
    case 'b':
      lowered += "{bx}";
      break;
    case 'c':
      lowered += "{cx}";
      break;
    case 'd':
      lowered += "{dx}";
      break;
    case 'S':
      lowered += "{si}";
      break;
    case 'D':
      lowered += "{di}";
      break;
    default:
      lowered.push_back(constraint[i]);
      break;
    }
  }

  return lowered;
}

std::string lowerAsmConstraintToLLVM(const std::string &constraint,
                                     const llvm::Triple &triple) {
  if (triple.isX86())
    return lowerX86GccConstraintToLLVM(constraint);
  return constraint;
}

void initializeLLVMTargets() {
  static const bool initialized = [] {
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmPrinters();
    llvm::InitializeAllAsmParsers();
    return true;
  }();
  (void)initialized;
}

std::string normalizeTargetTriple(std::string targetTriple) {
  if (targetTriple.empty()) {
    return llvm::sys::getDefaultTargetTriple();
  }
  return llvm::Triple::normalize(targetTriple);
}

llvm::CodeGenOptLevel toCodeGenOptLevel(int optimizationLevel) {
  if (optimizationLevel < 0) {
    optimizationLevel = 0;
  } else if (optimizationLevel > 3) {
    optimizationLevel = 3;
  }

  if (optimizationLevel == 1) {
    return llvm::CodeGenOptLevel::Less;
  }
  if (optimizationLevel == 2) {
    return llvm::CodeGenOptLevel::Default;
  }
  if (optimizationLevel == 3) {
    return llvm::CodeGenOptLevel::Aggressive;
  }
  return llvm::CodeGenOptLevel::None;
}

llvm::OptimizationLevel toOptimizationLevel(int optimizationLevel) {
  if (optimizationLevel <= 1) {
    return llvm::OptimizationLevel::O1;
  }
  if (optimizationLevel == 2) {
    return llvm::OptimizationLevel::O2;
  }
  return llvm::OptimizationLevel::O3;
}

void optimizeModule(llvm::Module &module, int optimizationLevel) {
  if (optimizationLevel <= 0) {
    return;
  }

  llvm::LoopAnalysisManager loopAnalysisManager;
  llvm::FunctionAnalysisManager functionAnalysisManager;
  llvm::CGSCCAnalysisManager cgsccAnalysisManager;
  llvm::ModuleAnalysisManager moduleAnalysisManager;
  llvm::PassBuilder passBuilder;
  passBuilder.registerModuleAnalyses(moduleAnalysisManager);
  passBuilder.registerCGSCCAnalyses(cgsccAnalysisManager);
  passBuilder.registerFunctionAnalyses(functionAnalysisManager);
  passBuilder.registerLoopAnalyses(loopAnalysisManager);
  passBuilder.crossRegisterProxies(loopAnalysisManager, functionAnalysisManager,
                                   cgsccAnalysisManager, moduleAnalysisManager);

  auto pipeline = passBuilder.buildPerModuleDefaultPipeline(
      toOptimizationLevel(optimizationLevel));
  pipeline.run(module, moduleAnalysisManager);
}

} // namespace
LLVMCodeGen::LLVMCodeGen(std::string targetTriple, bool freestanding)
    : builder_(ctx_),
      targetTriple_(normalizeTargetTriple(std::move(targetTriple))),
      freestanding_(freestanding),
      arcEmitter_(std::make_unique<ClassArcEmitter>(*this)), nextStringId_(0) {
  initializeLLVMTargets();
}

LLVMCodeGen::~LLVMCodeGen() = default;

void LLVMCodeGen::initializeModule() {
  module_ = std::make_unique<llvm::Module>("zap_module", ctx_);
  llvm::Triple triple(targetTriple_);
  module_->setTargetTriple(triple);
  std::string error;
  const auto *target = llvm::TargetRegistry::lookupTarget(triple, error);
  if (!target) {
    throw std::runtime_error("target lookup failed for '" + targetTriple_ +
                             "': " + error);
  }

  llvm::TargetOptions opts;
  std::unique_ptr<llvm::TargetMachine> tm(target->createTargetMachine(
      triple, "generic", "", opts, llvm::Reloc::PIC_));
  if (!tm) {
    throw std::runtime_error("failed to create target machine for '" +
                             targetTriple_ + "'");
  }
  module_->setDataLayout(tm->createDataLayout());
}

llvm::StructType *
LLVMCodeGen::getOrCreateClassStruct(const zir::ClassType &ct) {
  std::string objectName = ct.getCodegenName() + ".obj";
  auto it = structCache_.find(objectName);
  if (it != structCache_.end()) {
    return it->second;
  }
  auto *objectTy = llvm::StructType::create(ctx_, objectName);
  structCache_[objectName] = objectTy;
  return objectTy;
}

void LLVMCodeGen::finalizeClassStruct(const zir::ClassType &ct) {
  auto *objectTy = getOrCreateClassStruct(ct);
  if (!objectTy->isOpaque()) {
    return;
  }
  auto *i8PtrTy = llvm::PointerType::getUnqual(ctx_);
  std::vector<llvm::Type *> fieldTypes = {llvm::Type::getInt64Ty(ctx_),
                                          llvm::Type::getInt64Ty(ctx_),
                                          llvm::Type::getInt8Ty(ctx_),
                                          llvm::Type::getInt8Ty(ctx_),
                                          i8PtrTy,
                                          i8PtrTy,
                                          i8PtrTy,
                                          llvm::PointerType::getUnqual(ctx_),
                                          llvm::PointerType::getUnqual(ctx_)};
  assert(fieldTypes.size() == kClassHeaderFieldCount &&
        "class object header field count is out of sync with arc_layout.h");
  fieldTypes.reserve(kClassHeaderFieldCount + ct.getFields().size());
  for (const auto &f : ct.getFields()) {
    fieldTypes.push_back(toLLVMAggregateFieldType(f.type));
  }
  objectTy->setBody(fieldTypes);
}

void LLVMCodeGen::computeCyclicClasses(const zir::Module &module) {
  cyclicClasses_.clear();

  std::unordered_map<std::string, std::shared_ptr<zir::ClassType>> classes;
  for (const auto &type : module.getTypes()) {
    if (type->getKind() == zir::TypeKind::Class) {
      auto cls = std::static_pointer_cast<zir::ClassType>(type);
      classes[cls->getCodegenName()] = cls;
    }
  }

  std::unordered_map<std::string, std::vector<std::string>> subtypesOf;
  for (const auto &[name, cls] : classes) {
    for (auto cur = cls; cur; cur = cur->getBase()) {
      subtypesOf[cur->getCodegenName()].push_back(name);
    }
  }

  std::unordered_map<std::string, std::unordered_set<std::string>> edges;
  for (const auto &[name, cls] : classes) {
    auto &out = edges[name];
    for (auto cur = cls; cur; cur = cur->getBase()) {
      for (const auto &field : cur->getFields()) {
        std::unordered_set<std::string> referencedClasses;
        collectStrongReferencedClasses(field.type, referencedClasses);
        for (const auto &referencedClass : referencedClasses) {
          auto it = subtypesOf.find(referencedClass);
          if (it == subtypesOf.end()) {
            continue;
          }
          for (const auto &sub : it->second) {
            out.insert(sub);
          }
        }
      }
    }
  }

  for (const auto &[start, _] : classes) {
    std::vector<std::string> stack = {start};
    std::unordered_set<std::string> seen;
    bool cyclic = false;
    while (!stack.empty()) {
      std::string node = std::move(stack.back());
      stack.pop_back();
      auto it = edges.find(node);
      if (it == edges.end()) {
        continue;
      }
      for (const auto &next : it->second) {
        if (next == start) {
          cyclic = true;
          break;
        }
        if (seen.insert(next).second) {
          stack.push_back(next);
        }
      }
      if (cyclic) {
        break;
      }
    }
    if (cyclic) {
      cyclicClasses_.insert(start);
    }
  }
}

void LLVMCodeGen::printIR(llvm::raw_ostream &os) const {
  if (module_)
    module_->print(os, nullptr);
}

bool LLVMCodeGen::verifyModule(llvm::raw_ostream &diagnostics) const {
  if (!module_) {
    diagnostics << "zapc: internal error: LLVM module was not generated\n";
    return false;
  }

  std::string verifierOutput;
  llvm::raw_string_ostream verifierStream(verifierOutput);
  if (!llvm::verifyModule(*module_, &verifierStream)) {
    return true;
  }
  verifierStream.flush();

  diagnostics << "zapc: internal error: LLVM module verification failed\n";
  diagnostics << verifierOutput;
  return false;
}

bool LLVMCodeGen::emitObjectFile(const std::string &path,
                                 int optimization_level) {
  llvm::Triple triple(targetTriple_);
  module_->setTargetTriple(triple);
  std::string error;
  const auto *target = llvm::TargetRegistry::lookupTarget(triple, error);
  if (!target) {
    llvm::errs() << "Target lookup failed: " << error << "\n";
    return false;
  }

  llvm::TargetOptions opts;
  std::unique_ptr<llvm::TargetMachine> tm(target->createTargetMachine(
      triple, "generic", "", opts, llvm::Reloc::PIC_, std::nullopt,
      toCodeGenOptLevel(optimization_level)));
  if (!tm) {
    llvm::errs() << "Failed to create target machine for: " << targetTriple_
                 << "\n";
    return false;
  }
  module_->setDataLayout(tm->createDataLayout());

  if (!verifyModule(llvm::errs())) {
    return false;
  }

  optimizeModule(*module_, optimization_level);

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
  return true;
}

bool LLVMCodeGen::emitAssemblyFile(const std::string &path,
                                   int optimization_level) {
  llvm::Triple triple(targetTriple_);
  module_->setTargetTriple(triple);
  std::string error;
  const auto *target = llvm::TargetRegistry::lookupTarget(triple, error);
  if (!target) {
    llvm::errs() << "Target lookup failed: " << error << "\n";
    return false;
  }

  llvm::TargetOptions opts;
  std::unique_ptr<llvm::TargetMachine> tm(target->createTargetMachine(
      triple, "generic", "", opts, llvm::Reloc::PIC_, std::nullopt,
      toCodeGenOptLevel(optimization_level)));
  if (!tm) {
    llvm::errs() << "Failed to create target machine for: " << targetTriple_
                 << "\n";
    return false;
  }
  module_->setDataLayout(tm->createDataLayout());

  if (!verifyModule(llvm::errs())) {
    return false;
  }

  optimizeModule(*module_, optimization_level);

  std::error_code ec;
  llvm::raw_fd_ostream dest(path, ec, llvm::sys::fs::OF_None);
  if (ec) {
    llvm::errs() << "Cannot open output file: " << ec.message() << "\n";
    return false;
  }

  llvm::legacy::PassManager pm;
  if (tm->addPassesToEmitFile(pm, dest, nullptr,
                              llvm::CodeGenFileType::AssemblyFile)) {
    llvm::errs() << "TargetMachine cannot emit assembly file\n";
    return false;
  }

  pm.run(*module_);
  dest.flush();
  return true;
}

llvm::IntegerType *LLVMCodeGen::nativeIntegerType() {
  return llvm::IntegerType::get(
      ctx_, module_->getDataLayout().getPointerSizeInBits());
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
    return nativeIntegerType();
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
    return llvm::PointerType::getUnqual(ctx_);
  }
  case zir::TypeKind::NullPtr:
    return llvm::PointerType::getUnqual(ctx_);
  case zir::TypeKind::Enum:
    if (static_cast<const zir::EnumType &>(ty).hasReprC)
      return llvm::Type::getInt32Ty(ctx_);
    return nativeIntegerType();
  case zir::TypeKind::TaggedUnion: {
    const auto &tu = static_cast<const zir::TaggedUnionType &>(ty);
    auto it = structCache_.find(tu.getCodegenName());
    if (it != structCache_.end())
      return it->second;

    auto *structTy = llvm::StructType::create(ctx_, tu.getCodegenName());
    structCache_[tu.getCodegenName()] = structTy;

    llvm::Type *payloadStorageTy = llvm::Type::getInt8Ty(ctx_);
    uint64_t payloadStorageSize = 0;
    uint64_t payloadStorageAlign = 0;
    for (const auto &variant : tu.getVariants()) {
      if (!variant.payloadType)
        continue;
      auto *candidateTy = toLLVMType(*variant.payloadType);
      auto candidateSize =
          module_->getDataLayout().getTypeAllocSize(candidateTy);
      auto candidateAlign =
          module_->getDataLayout().getABITypeAlign(candidateTy).value();
      if (candidateSize > payloadStorageSize ||
          (candidateSize == payloadStorageSize &&
           candidateAlign > payloadStorageAlign)) {
        payloadStorageTy = candidateTy;
        payloadStorageSize = candidateSize;
        payloadStorageAlign = candidateAlign;
      }
    }

    structTy->setBody({llvm::Type::getInt32Ty(ctx_), payloadStorageTy});
    return structTy;
  }
  case zir::TypeKind::Record: {
    const auto &rt = static_cast<const zir::RecordType &>(ty);
    std::string cacheKey = rt.getCodegenName();
    if (zir::isIntrinsicStringType(rt)) {
      cacheKey = rt.getIntrinsicKind() == zir::IntrinsicTypeKind::String
                     ? "zap.intrinsic.String"
                     : "zap.intrinsic.StringView";
    }

    auto it = structCache_.find(cacheKey);
    if (it != structCache_.end())
      return it->second;

    if (zir::isIntrinsicStringType(rt)) {
      auto *structTy = llvm::StructType::create(ctx_, cacheKey);
      structCache_[cacheKey] = structTy;
      std::vector<llvm::Type *> fieldTypes;
      fieldTypes.push_back(llvm::PointerType::getUnqual(ctx_));
      fieldTypes.push_back(llvm::Type::getInt64Ty(ctx_));
      structTy->setBody(fieldTypes, rt.isPacked);
      return structTy;
    }

    auto *structTy = llvm::StructType::create(ctx_, rt.getCodegenName());
    structCache_[cacheKey] = structTy;
    std::vector<llvm::Type *> fieldTypes;
    for (const auto &f : rt.getFields())
      fieldTypes.push_back(toLLVMAggregateFieldType(f.type));
    structTy->setBody(fieldTypes, rt.isPacked);
    return structTy;
  }
  case zir::TypeKind::Class:
    return llvm::PointerType::getUnqual(ctx_);
  case zir::TypeKind::Array: {
    const auto &at = static_cast<const zir::ArrayType &>(ty);
    return llvm::ArrayType::get(toLLVMType(*at.getBaseType()), at.getSize());
  }
  case zir::TypeKind::FunctionPointer:
    return llvm::PointerType::getUnqual(ctx_);
  }
  throw std::runtime_error("Unknown ZIR type: " + ty.toString());
}

llvm::Type *
LLVMCodeGen::toLLVMAggregateFieldType(const std::shared_ptr<zir::Type> &type) {
  if (type && type->getKind() == zir::TypeKind::Void) {
    return llvm::Type::getInt8Ty(ctx_);
  }
  return type ? toLLVMType(*type) : llvm::Type::getInt8Ty(ctx_);
}

llvm::FunctionType *LLVMCodeGen::buildFunctionType(const zir::Function &fn) {
  std::vector<llvm::Type *> paramTypes;
  if (!freestanding_ && fn.name == "main") {
    paramTypes.push_back(llvm::Type::getInt32Ty(ctx_));
    paramTypes.push_back(llvm::PointerType::getUnqual(ctx_));
  }
  for (const auto &param : fn.getArguments()) {
    if (param->isVariadicPack()) {
      paramTypes.push_back(llvm::Type::getInt32Ty(ctx_));
      paramTypes.push_back(llvm::PointerType::getUnqual(ctx_));
      continue;
    }
    paramTypes.push_back(toLLVMType(*param->getType()));
  }
  llvm::Type *retTy = toLLVMType(*fn.getReturnType());
  if (fn.returnsRef)
    retTy = llvm::PointerType::getUnqual(ctx_);
  return llvm::FunctionType::get(retTy, paramTypes, fn.isCVariadic);
}

llvm::AllocaInst *LLVMCodeGen::createEntryAlloca(llvm::Function *fn,
                                                 const std::string &name,
                                                 llvm::Type *ty) {
  llvm::IRBuilder<> entry(&fn->getEntryBlock(), fn->getEntryBlock().begin());
  return entry.CreateAlloca(ty, nullptr, name);
}

llvm::Value *
LLVMCodeGen::emitStringConversion(llvm::Value *source,
                                  const std::shared_ptr<zir::Type> &sourceType,
                                  const std::shared_ptr<zir::Type> &targetType,
                                  const llvm::Twine &namePrefix) {
  auto *targetLLVMType = toLLVMType(*targetType);
  auto *ptr = builder_.CreateExtractValue(source, {0}, namePrefix + ".ptr");
  auto *len = builder_.CreateExtractValue(source, {1}, namePrefix + ".len");

  if (zir::isIntrinsicStringViewType(sourceType) &&
      isOwnedStringType(targetType)) {
    auto *functionType = llvm::FunctionType::get(
        targetLLVMType, {ptr->getType(), len->getType()}, /*isVarArg=*/false);
    auto callee =
        module_->getOrInsertFunction("zap_string_from_ptrlen", functionType);
    return builder_.CreateCall(functionType, callee.getCallee(), {ptr, len},
                               namePrefix + ".owned");
  }

  if (source->getType() == targetLLVMType) {
    return source;
  }

  llvm::Value *result = llvm::UndefValue::get(targetLLVMType);
  result = builder_.CreateInsertValue(result, ptr, {0}, namePrefix + ".ptr.i");
  return builder_.CreateInsertValue(result, len, {1}, namePrefix + ".len.i");
}

llvm::Value *
LLVMCodeGen::emitStringConcat(llvm::Value *lhs, llvm::Value *rhs,
                              const std::shared_ptr<zir::Type> &lhsType,
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

  auto concatIt = functionMap_.find("string_concat_ptrlen");
  if (concatIt == functionMap_.end()) {
    std::vector<llvm::Type *> params = {
        llvm::PointerType::getUnqual(ctx_), i64Ty,
        llvm::PointerType::getUnqual(ctx_), i64Ty};
    auto *ft = llvm::FunctionType::get(llvm::PointerType::getUnqual(ctx_),
                                       params, false);
    auto *fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                                      "string_concat_ptrlen", *module_);
    concatIt = functionMap_.emplace("string_concat_ptrlen", fn).first;
  }

  auto *concatFn = concatIt->second;
  auto *call = builder_.CreateCall(concatFn, {lhsPtr, lhsLen, rhsPtr, rhsLen});
  auto *sumLen = builder_.CreateAdd(lhsLen, rhsLen);

  auto *structTy = static_cast<llvm::StructType *>(toLLVMType(*resultType));
  llvm::Value *res = llvm::UndefValue::get(structTy);
  res = builder_.CreateInsertValue(res, call, {0});
  res = builder_.CreateInsertValue(res, sumLen, {1});
  return res;
}

void LLVMCodeGen::buildInlineAsmCall(
    const std::string &assembly, const std::vector<std::string> &outConstraints,
    const std::vector<llvm::Value *> &outAddrs,
    const std::vector<llvm::Type *> &outValueTypes,
    const std::vector<std::string> &inConstraints,
    const std::vector<llvm::Value *> &inValues,
    const std::vector<std::string> &clobbers) {
  llvm::Triple triple(module_->getTargetTriple());
  std::vector<std::string> parts;
  for (const auto &c : outConstraints)
    parts.push_back(lowerAsmConstraintToLLVM(c, triple));
  for (const auto &c : inConstraints)
    parts.push_back(lowerAsmConstraintToLLVM(c, triple));
  for (const auto &c : clobbers)
    parts.push_back("~{" + c + "}");

  std::string constraints;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i > 0)
      constraints += ",";
    constraints += parts[i];
  }

  llvm::Type *retTy;
  if (outValueTypes.empty()) {
    retTy = llvm::Type::getVoidTy(ctx_);
  } else if (outValueTypes.size() == 1) {
    retTy = outValueTypes[0];
  } else {
    retTy = llvm::StructType::get(ctx_, outValueTypes);
  }

  std::vector<llvm::Type *> paramTypes;
  for (auto *v : inValues)
    paramTypes.push_back(v->getType());

  auto *fnTy = llvm::FunctionType::get(retTy, paramTypes, /*isVarArg=*/false);
  auto *inlineAsm =
      llvm::InlineAsm::get(fnTy, lowerGccAsmTemplateToLLVM(assembly),
                           constraints, /*hasSideEffects=*/true);
  auto *call = builder_.CreateCall(inlineAsm, inValues);

  if (outValueTypes.size() == 1) {
    builder_.CreateStore(call, outAddrs[0]);
  } else if (outValueTypes.size() > 1) {
    for (unsigned i = 0; i < outValueTypes.size(); ++i) {
      auto *field = builder_.CreateExtractValue(call, {i});
      builder_.CreateStore(field, outAddrs[i]);
    }
  }
}

} // namespace codegen
