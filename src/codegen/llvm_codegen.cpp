#include "llvm_codegen.hpp"
#include "../utils/string_type_utils.hpp"
#include "class_arc_emitter.hpp"
#include "class_layout.hpp"
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InlineAsm.h>
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
#include <optional>
#include <stdexcept>

namespace codegen {
namespace {
bool isStringType(const std::shared_ptr<zir::Type> &type) {
  return zap::text::isStringType(type);
}

bool isStringRecordName(const std::string &full) {
  return zap::text::isStringRecordName(full);
}

} // namespace
LLVMCodeGen::LLVMCodeGen()
    : builder_(ctx_), evaluateAsAddr_(false),
      arcEmitter_(std::make_unique<ClassArcEmitter>(*this)), nextStringId_(0) {
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();
}

LLVMCodeGen::~LLVMCodeGen() = default;

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
  auto *i8PtrTy = llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(ctx_));
  std::vector<llvm::Type *> fieldTypes = {
      llvm::Type::getInt64Ty(ctx_),
      llvm::Type::getInt64Ty(ctx_),
      llvm::Type::getInt8Ty(ctx_),
      llvm::Type::getInt8Ty(ctx_),
      i8PtrTy,
      i8PtrTy,
      i8PtrTy,
      llvm::PointerType::getUnqual(i8PtrTy)};
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
      classes[cls->getName()] = cls;
    }
  }

  std::unordered_map<std::string, std::vector<std::string>> subtypesOf;
  for (const auto &[name, cls] : classes) {
    for (auto cur = cls; cur; cur = cur->getBase()) {
      subtypesOf[cur->getName()].push_back(name);
    }
  }

  std::unordered_map<std::string, std::unordered_set<std::string>> edges;
  for (const auto &[name, cls] : classes) {
    auto &out = edges[name];
    for (auto cur = cls; cur; cur = cur->getBase()) {
      for (const auto &field : cur->getFields()) {
        if (!field.type || field.type->getKind() != zir::TypeKind::Class) {
          continue;
        }
        auto fieldClass = std::static_pointer_cast<zir::ClassType>(field.type);
        if (fieldClass->isWeak()) {
          continue;
        }
        auto it = subtypesOf.find(fieldClass->getName());
        if (it == subtypesOf.end()) {
          continue;
        }
        for (const auto &sub : it->second) {
          out.insert(sub);
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

bool LLVMCodeGen::emitObjectFile(const std::string &path,
                                 int optimization_level) {
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

  if (optimization_level < 0) {
    optimization_level = 0;
  } else if (optimization_level > 3) {
    optimization_level = 3;
  }

  llvm::CodeGenOptLevel codegenOptLevel = llvm::CodeGenOptLevel::None;
  if (optimization_level == 1) {
    codegenOptLevel = llvm::CodeGenOptLevel::Less;
  } else if (optimization_level == 2) {
    codegenOptLevel = llvm::CodeGenOptLevel::Default;
  } else if (optimization_level == 3) {
    codegenOptLevel = llvm::CodeGenOptLevel::Aggressive;
  }

  llvm::TargetOptions opts;
  auto *tm = target->createTargetMachine(triple, "generic", "", opts,
                                         llvm::Reloc::PIC_, std::nullopt,
                                         codegenOptLevel);
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

bool LLVMCodeGen::emitAssemblyFile(const std::string &path,
                                   int optimization_level) {
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

  if (optimization_level < 0) {
    optimization_level = 0;
  } else if (optimization_level > 3) {
    optimization_level = 3;
  }

  llvm::CodeGenOptLevel codegenOptLevel = llvm::CodeGenOptLevel::None;
  if (optimization_level == 1) {
    codegenOptLevel = llvm::CodeGenOptLevel::Less;
  } else if (optimization_level == 2) {
    codegenOptLevel = llvm::CodeGenOptLevel::Default;
  } else if (optimization_level == 3) {
    codegenOptLevel = llvm::CodeGenOptLevel::Aggressive;
  }

  llvm::TargetOptions opts;
  auto *tm = target->createTargetMachine(triple, "generic", "", opts,
                                         llvm::Reloc::PIC_, std::nullopt,
                                         codegenOptLevel);
  module_->setDataLayout(tm->createDataLayout());

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

  bool is_broken = llvm::verifyModule(*module_, &llvm::errs());
  if (!is_broken) {
    pm.run(*module_);
  }
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
    if (static_cast<const zir::EnumType &>(ty).hasReprC)
      return llvm::Type::getInt32Ty(ctx_);
    return llvm::Type::getInt64Ty(ctx_);
  case zir::TypeKind::Record: {
    const auto &rt = static_cast<const zir::RecordType &>(ty);
    auto it = structCache_.find(rt.getCodegenName());
    if (it != structCache_.end())
      return it->second;

    if (isStringRecordName(rt.getName())) {
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
      fieldTypes.push_back(toLLVMAggregateFieldType(f.type));
    structTy->setBody(fieldTypes);
    return structTy;
  }
  case zir::TypeKind::Class: {
    const auto &ct = static_cast<const zir::ClassType &>(ty);
    return llvm::PointerType::getUnqual(getOrCreateClassStruct(ct));
  }
  case zir::TypeKind::Array: {
    const auto &at = static_cast<const zir::ArrayType &>(ty);
    return llvm::ArrayType::get(toLLVMType(*at.getBaseType()), at.getSize());
  }
  case zir::TypeKind::FunctionPointer: {
    const auto &ft = static_cast<const zir::FunctionPointerType &>(ty);
    std::vector<llvm::Type *> paramTypes;
    for (const auto &p : ft.getParams())
      paramTypes.push_back(toLLVMType(*p));
    auto *fnTy = llvm::FunctionType::get(toLLVMType(*ft.getReturnType()),
                                         paramTypes, false);
    return llvm::PointerType::getUnqual(fnTy);
  }
  default:
    break;
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
  if (sym.returnsRef)
    retTy = llvm::PointerType::getUnqual(retTy);
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
  llvm::Type *retTy = toLLVMType(*fn.getReturnType());
  if (fn.returnsRef)
    retTy = llvm::PointerType::getUnqual(retTy);
  return llvm::FunctionType::get(retTy, paramTypes, fn.isCVariadic);
}

llvm::AllocaInst *LLVMCodeGen::createEntryAlloca(llvm::Function *fn,
                                                 const std::string &name,
                                                 llvm::Type *ty) {
  llvm::IRBuilder<> entry(&fn->getEntryBlock(), fn->getEntryBlock().begin());
  return entry.CreateAlloca(ty, nullptr, name);
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
        llvm::PointerType::getUnqual(i8Ty), i64Ty,
        llvm::PointerType::getUnqual(i8Ty), i64Ty};
    auto *ft = llvm::FunctionType::get(llvm::PointerType::getUnqual(i8Ty),
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
  std::vector<std::string> parts;
  for (const auto &c : outConstraints)
    parts.push_back(c);
  for (const auto &c : inConstraints)
    parts.push_back(c);
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
  auto *inlineAsm = llvm::InlineAsm::get(fnTy, assembly, constraints,
                                         /*hasSideEffects=*/true);
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
  scopeStringLocals_.clear();
  scopeClassLocals_.push_back({});
  scopeStringLocals_.push_back({});

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

    auto setArgsIt2 = functionMap_.find("__zap_process_set_args");
    if (setArgsIt2 == functionMap_.end()) {
      auto *ft = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_),
                                         {i32Ty, argvTy}, false);
      auto *setArgsFn =
          llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                                 "__zap_process_set_args", *module_);
      setArgsIt2 =
          functionMap_.emplace("__zap_process_set_args", setArgsFn).first;
    }

    builder_.CreateCall(
        setArgsIt2->second,
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
      if (isOwnedStringType(param->type)) {
        builder_.CreateStore(llvm::Constant::getNullValue(arg.getType()),
                             alloca);
        emitStoreWithStringArc(alloca, &arg, param->type,
                               /*valueIsOwned=*/false);
        scopeStringLocals_.back().push_back({param->type, alloca});
        continue;
      }
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
      for (auto it = scopeStringLocals_.rbegin();
           it != scopeStringLocals_.rend(); ++it) {
        for (auto local = it->rbegin(); local != it->rend(); ++local) {
          auto *value = builder_.CreateLoad(
              toLLVMType(*local->first), local->second, "str.fn.ret.release");
          emitStringReleaseIfNeeded(value, local->first);
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
      for (auto it = scopeStringLocals_.rbegin();
           it != scopeStringLocals_.rend(); ++it) {
        for (auto local = it->rbegin(); local != it->rend(); ++local) {
          auto *value = builder_.CreateLoad(
              toLLVMType(*local->first), local->second, "str.fn.ret.release");
          emitStringReleaseIfNeeded(value, local->first);
        }
      }
      builder_.CreateRetVoid();
    }
  }

  currentFn_ = nullptr;
  scopeClassLocals_.clear();
  scopeStringLocals_.clear();
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
  scopeStringLocals_.push_back({});
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
  scopeStringLocals_.pop_back();
}

void LLVMCodeGen::visit(sema::BoundVariableDeclaration &node) {
  auto *ty = toLLVMType(*node.symbol->type);

  if (currentFn_) {
    if (node.symbol->is_ref) {
      // Ref var: alloca a pointer slot, store the address of the initializer
      auto *ptrTy = llvm::PointerType::getUnqual(ty);
      auto *alloca = createEntryAlloca(currentFn_, node.symbol->name, ptrTy);
      localValues_[node.symbol->name] = alloca;
      if (node.initializer) {
        bool old = evaluateAsAddr_;
        evaluateAsAddr_ = true;
        node.initializer->accept(*this);
        evaluateAsAddr_ = old;
        builder_.CreateStore(lastValue_, alloca);
      }
      return;
    }

    auto *alloca = createEntryAlloca(currentFn_, node.symbol->name, ty);
    localValues_[node.symbol->name] = alloca;
    if (isClassType(node.symbol->type) ||
        isOwnedStringType(node.symbol->type)) {
      builder_.CreateStore(llvm::Constant::getNullValue(ty), alloca);
    }

    if (node.initializer) {
      node.initializer->accept(*this);
      if (isOwnedStringType(node.symbol->type)) {
        emitStoreWithStringArc(
            alloca, lastValue_, node.symbol->type,
            expressionProducesOwnedString(node.initializer.get()));
      } else {
        emitStoreWithArc(alloca, lastValue_, node.symbol->type,
                         expressionProducesOwnedClass(node.initializer.get()));
      }
    }
    if (isClassType(node.symbol->type) && !scopeClassLocals_.empty()) {
      scopeClassLocals_.back().push_back({node.symbol->type, alloca});
    } else if (isOwnedStringType(node.symbol->type) &&
               !scopeStringLocals_.empty()) {
      scopeStringLocals_.back().push_back({node.symbol->type, alloca});
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
    } else if (isOwnedStringType(node.expression->type) &&
               !expressionProducesOwnedString(node.expression.get())) {
      lastValue_ = emitStringRetainIfNeeded(lastValue_, node.expression->type);
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
    for (auto it = scopeStringLocals_.rbegin(); it != scopeStringLocals_.rend();
         ++it) {
      for (auto local = it->rbegin(); local != it->rend(); ++local) {
        auto *value = builder_.CreateLoad(toLLVMType(*local->first),
                                          local->second, "str.ret.release");
        emitStringReleaseIfNeeded(value, local->first);
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
    for (auto it = scopeStringLocals_.rbegin(); it != scopeStringLocals_.rend();
         ++it) {
      for (auto local = it->rbegin(); local != it->rend(); ++local) {
        auto *value = builder_.CreateLoad(toLLVMType(*local->first),
                                          local->second, "str.ret.release");
        emitStringReleaseIfNeeded(value, local->first);
      }
    }
    builder_.CreateRetVoid();
  }
}
void LLVMCodeGen::visit(sema::BoundAssignment &node) {
  bool old = evaluateAsAddr_;
  llvm::Value *alloca = nullptr;
  llvm::Value *oldCompoundTargetAddr = compoundTargetAddr_;
  if (node.isCompound) {
    evaluateAsAddr_ = true;
    node.target->accept(*this);
    alloca = lastValue_;
    evaluateAsAddr_ = old;
    compoundTargetAddr_ = alloca;
  }

  node.expression->accept(*this);
  llvm::Value *val = lastValue_;
  compoundTargetAddr_ = oldCompoundTargetAddr;

  if (!node.isCompound) {
    evaluateAsAddr_ = true;
    node.target->accept(*this);
    alloca = lastValue_;
    evaluateAsAddr_ = old;
  }

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

void LLVMCodeGen::visit(sema::BoundRecordDeclaration &node) {
  toLLVMType(*node.type);
}

void LLVMCodeGen::visit(sema::BoundEnumDeclaration &node) {
  // Enums are typically handled at the type level in ZIR/sema.
  // We don't need to generate code for the declaration itself
  // unless we want to generate debug info or constant values.
  (void)node;
}
void LLVMCodeGen::visit(sema::BoundFailStatement &node) {
  (void)node;
  throw std::runtime_error(
      "BoundFailStatement should be lowered to ZIR before LLVMCodeGen.");
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

void LLVMCodeGen::visit(sema::BoundForStatement &node) {
  if (!currentFn_)
    throw std::runtime_error("currentFn_ is null in visit(BoundForStatement)");

  if (node.initializer) {
    if (auto *initBlock =
            dynamic_cast<sema::BoundBlock *>(node.initializer.get())) {
      for (const auto &stmt : initBlock->statements) {
        if (stmt) {
          stmt->accept(*this);
        }
      }
    } else {
      node.initializer->accept(*this);
    }
  }

  auto *condBB = llvm::BasicBlock::Create(ctx_, "for.cond", currentFn_);
  auto *bodyBB = llvm::BasicBlock::Create(ctx_, "for.body", currentFn_);
  auto *stepBB = llvm::BasicBlock::Create(ctx_, "for.step", currentFn_);
  auto *endBB = llvm::BasicBlock::Create(ctx_, "for.end", currentFn_);

  builder_.CreateBr(condBB);

  builder_.SetInsertPoint(condBB);
  if (!node.condition)
    throw std::runtime_error("condition is null in BoundForStatement");
  node.condition->accept(*this);
  auto *cond = lastValue_;
  if (!cond)
    throw std::runtime_error(
        "lastValue_ is null after condition in BoundForStatement");
  builder_.CreateCondBr(cond, bodyBB, endBB);

  builder_.SetInsertPoint(bodyBB);
  loopBBStack_.push_back({stepBB, endBB});
  if (node.body) {
    node.body->accept(*this);
  }
  loopBBStack_.pop_back();
  if (!builder_.GetInsertBlock()->getTerminator()) {
    builder_.CreateBr(stepBB);
  }

  builder_.SetInsertPoint(stepBB);
  if (node.increment) {
    node.increment->accept(*this);
  }
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

void LLVMCodeGen::visit(sema::BoundAsmStatement &node) {
  std::vector<std::string> outConstraints, inConstraints;
  std::vector<llvm::Value *> outAddrs, inValues;
  std::vector<llvm::Type *> outValueTypes;

  for (auto &out : node.outputs) {
    bool old = evaluateAsAddr_;
    evaluateAsAddr_ = true;
    out.expr->accept(*this);
    evaluateAsAddr_ = old;
    outConstraints.push_back(out.constraint);
    outAddrs.push_back(lastValue_);
    outValueTypes.push_back(toLLVMType(*out.expr->type));
  }
  for (auto &in : node.inputs) {
    in.expr->accept(*this);
    inConstraints.push_back(in.constraint);
    inValues.push_back(lastValue_);
  }

  buildInlineAsmCall(node.assembly, outConstraints, outAddrs, outValueTypes,
                     inConstraints, inValues, node.clobbers);
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
