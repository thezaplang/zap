#include "ir_generator.hpp"
#include "../sema/constant_evaluator.hpp"
#include "failable_type.hpp"
#include "string_type.hpp"
#include <cstdint>
#include <iostream>
#include <optional>

namespace zir {
namespace {
std::string renderTypeForUser(const std::shared_ptr<Type> &type) {
  if (!type) {
    return "<unknown>";
  }

  if (type->getKind() == TypeKind::Pointer) {
    auto ptr = std::static_pointer_cast<PointerType>(type);
    return renderTypeForUser(ptr->getBaseType()) + "*";
  }
  if (type->getKind() == TypeKind::Record) {
    auto rec = std::static_pointer_cast<RecordType>(type);
    auto full = rec->getName();
    auto dot = full.find_last_of('.');
    return dot == std::string::npos ? full : full.substr(dot + 1);
  }
  if (type->getKind() == TypeKind::Class) {
    auto cls = std::static_pointer_cast<ClassType>(type);
    auto full = cls->getName();
    auto dot = full.find_last_of('.');
    return dot == std::string::npos ? full : full.substr(dot + 1);
  }
  if (type->getKind() == TypeKind::Enum) {
    auto en = std::static_pointer_cast<EnumType>(type);
    auto full = en->getName();
    auto dot = full.find_last_of('.');
    return dot == std::string::npos ? full : full.substr(dot + 1);
  }

  return type->toString();
}

bool isTerminated(const BasicBlock *block) {
  if (!block || block->instructions.empty()) {
    return false;
  }
  const auto opcode = block->instructions.back()->getOpCode();
  return opcode == OpCode::Ret || opcode == OpCode::Br ||
         opcode == OpCode::CondBr;
}

ValueOwnership ownershipForPhi(
    const std::shared_ptr<Type> &type,
    const std::vector<std::pair<std::string, std::shared_ptr<Value>>>
        &incoming) {
  if (!containsManagedValues(type) || incoming.empty()) {
    return ValueOwnership::Borrowed;
  }
  const auto ownership = incoming.front().second
                             ? incoming.front().second->getOwnership()
                             : ValueOwnership::Borrowed;
  if (!isOwned(ownership)) {
    return ValueOwnership::Borrowed;
  }
  for (const auto &[label, value] : incoming) {
    (void)label;
    if (!value || value->getOwnership() != ownership) {
      return ValueOwnership::Borrowed;
    }
  }
  return ownership;
}

ValueOwnership ownershipForCast(const std::shared_ptr<Value> &source,
                                const std::shared_ptr<Type> &targetType) {
  if (!targetType || !containsManagedValues(targetType)) {
    return ValueOwnership::Borrowed;
  }
  if (targetType->getIntrinsicKind() == IntrinsicTypeKind::String) {
    return ValueOwnership::OwnedStrong;
  }
  return source && isOwned(source->getOwnership()) ? source->getOwnership()
                                                   : ValueOwnership::Borrowed;
}

ParameterOwnership parameterOwnershipFor(const sema::FunctionSymbol &function,
                                         size_t parameterIndex) {
  if (parameterIndex >= function.parameters.size()) {
    return ParameterOwnership::Borrow;
  }
  const auto &parameter = function.parameters[parameterIndex];
  const bool borrowedSelf = parameterIndex == 0 &&
                            !function.ownerTypeCodegenName.empty() &&
                            parameter->name == "self";
  if (function.isExternal || parameter->is_ref || parameter->is_variadic_pack ||
      borrowedSelf || !containsManagedValues(parameter->type)) {
    return ParameterOwnership::Borrow;
  }
  return parameter->is_sink ? ParameterOwnership::Sink
                            : ParameterOwnership::Transfer;
}

ParameterEscape parameterEscapeFor(const sema::FunctionSymbol &function,
                                   size_t parameterIndex) {
  if (parameterIndex >= function.parameters.size()) {
    return ParameterEscape::Unspecified;
  }
  return function.parameters[parameterIndex]->is_noescape
             ? ParameterEscape::NoEscape
             : ParameterEscape::Unspecified;
}
} // namespace

std::shared_ptr<Value> BoundIRGenerator::lowerConstantExpression(
    const sema::BoundExpression &expression) {
  if (!sema::ConstantEvaluator::isConstant(expression)) {
    return nullptr;
  }
  std::set<const sema::VariableSymbol *> resolvingConstants;
  return lowerConstantExpression(expression, resolvingConstants);
}

std::shared_ptr<Value> BoundIRGenerator::lowerConstantExpression(
    const sema::BoundExpression &expression,
    std::set<const sema::VariableSymbol *> &resolvingConstants) {
  if (auto unary =
          dynamic_cast<const sema::BoundUnaryExpression *>(&expression)) {
    if (unary->op != "&") {
      return nullptr;
    }

    if (auto variable = dynamic_cast<const sema::BoundVariableExpression *>(
            unary->expr.get())) {
      return std::make_shared<GlobalAddress>(variable->symbol->linkName,
                                             unary->type);
    }

    if (auto index =
            dynamic_cast<const sema::BoundIndexAccess *>(unary->expr.get())) {
      auto variable = dynamic_cast<const sema::BoundVariableExpression *>(
          index->left.get());
      auto indexValue =
          lowerConstantExpression(*index->index, resolvingConstants);
      auto constantIndex = std::dynamic_pointer_cast<Constant>(indexValue);
      if (!variable || !constantIndex) {
        return nullptr;
      }
      try {
        return std::make_shared<GlobalAddress>(
            variable->symbol->linkName, unary->type,
            static_cast<size_t>(std::stoull(constantIndex->getLiteral())));
      } catch (const std::exception &) {
        return nullptr;
      }
    }
    return nullptr;
  }

  if (auto literal = dynamic_cast<const sema::BoundLiteral *>(&expression)) {
    return std::make_shared<Constant>(literal->value, literal->type);
  }

  if (auto variable =
          dynamic_cast<const sema::BoundVariableExpression *>(&expression)) {
    auto symbol = variable->symbol;
    if (symbol && symbol->isCompileTimeConstant() && symbol->constant_value) {
      if (!resolvingConstants.insert(symbol.get()).second) {
        return nullptr;
      }
      auto value =
          lowerConstantExpression(*symbol->constant_value, resolvingConstants);
      resolvingConstants.erase(symbol.get());
      return value;
    }
    return nullptr;
  }

  if (auto binary =
          dynamic_cast<const sema::BoundBinaryExpression *>(&expression)) {
    auto left = lowerConstantExpression(*binary->left, resolvingConstants);
    auto right = lowerConstantExpression(*binary->right, resolvingConstants);
    auto leftConstant = std::dynamic_pointer_cast<Constant>(left);
    auto rightConstant = std::dynamic_pointer_cast<Constant>(right);
    if (!leftConstant || !rightConstant) {
      return nullptr;
    }

    try {
      if (binary->type->isFloatingPoint()) {
        const double lhs = std::stod(leftConstant->getLiteral());
        const double rhs = std::stod(rightConstant->getLiteral());
        std::optional<double> result;
        if (binary->op == "+")
          result = lhs + rhs;
        else if (binary->op == "-")
          result = lhs - rhs;
        else if (binary->op == "*")
          result = lhs * rhs;
        else if (binary->op == "/" && rhs != 0.0)
          result = lhs / rhs;
        if (result) {
          return std::make_shared<Constant>(std::to_string(*result),
                                            binary->type);
        }
        return nullptr;
      }

      if ((binary->type->getIntrinsicKind() == IntrinsicTypeKind::String ||
           binary->type->getIntrinsicKind() == IntrinsicTypeKind::StringView) &&
          binary->op == "+") {
        return std::make_shared<Constant>(leftConstant->getLiteral() +
                                              rightConstant->getLiteral(),
                                          binary->type);
      }

      if (!binary->type->isInteger()) {
        return nullptr;
      }
      const int64_t lhs = std::stoll(leftConstant->getLiteral(), nullptr, 0);
      const int64_t rhs = std::stoll(rightConstant->getLiteral(), nullptr, 0);
      std::optional<int64_t> result;
      if (binary->op == "+")
        result = lhs + rhs;
      else if (binary->op == "-")
        result = lhs - rhs;
      else if (binary->op == "*")
        result = lhs * rhs;
      else if (binary->op == "/" && rhs != 0)
        result = lhs / rhs;
      else if (binary->op == "%" && rhs != 0)
        result = lhs % rhs;
      else if (binary->op == "&")
        result = lhs & rhs;
      else if (binary->op == "|")
        result = lhs | rhs;
      else if (binary->op == "^")
        result = lhs ^ rhs;
      else if (binary->op == "<<" && rhs >= 0)
        result = lhs << rhs;
      else if (binary->op == ">>" && rhs >= 0)
        result = lhs >> rhs;
      if (result) {
        return std::make_shared<Constant>(std::to_string(*result),
                                          binary->type);
      }
    } catch (const std::exception &) {
    }
    return nullptr;
  }

  if (auto unary =
          dynamic_cast<const sema::BoundUnaryExpression *>(&expression)) {
    auto value = lowerConstantExpression(*unary->expr, resolvingConstants);
    auto constant = std::dynamic_pointer_cast<Constant>(value);
    if (!constant || !unary->type->isInteger()) {
      return nullptr;
    }
    try {
      const int64_t operand = std::stoll(constant->getLiteral(), nullptr, 0);
      if (unary->op == "-") {
        return std::make_shared<Constant>(std::to_string(-operand),
                                          unary->type);
      }
      if (unary->op == "~") {
        return std::make_shared<Constant>(std::to_string(~operand),
                                          unary->type);
      }
    } catch (const std::exception &) {
    }
    return nullptr;
  }

  if (auto array = dynamic_cast<const sema::BoundArrayLiteral *>(&expression)) {
    std::vector<std::shared_ptr<Value>> elements;
    elements.reserve(array->elements.size());
    for (const auto &element : array->elements) {
      auto value = lowerConstantExpression(*element, resolvingConstants);
      if (!value) {
        return nullptr;
      }
      elements.push_back(std::move(value));
    }
    return std::make_shared<ArrayConstant>(array->type, std::move(elements));
  }

  if (auto record =
          dynamic_cast<const sema::BoundStructLiteral *>(&expression)) {
    std::vector<AggregateConstant::FieldValue> fields;
    fields.reserve(record->fields.size());
    for (const auto &field : record->fields) {
      auto value = lowerConstantExpression(*field.second, resolvingConstants);
      if (!value) {
        return nullptr;
      }
      fields.push_back({field.first, std::move(value)});
    }
    return std::make_shared<AggregateConstant>(record->type, std::move(fields));
  }

  if (auto cast = dynamic_cast<const sema::BoundCast *>(&expression)) {
    auto value = lowerConstantExpression(*cast->expression, resolvingConstants);
    if (auto constant = std::dynamic_pointer_cast<Constant>(value)) {
      return std::make_shared<Constant>(constant->getLiteral(), cast->type);
    }
    return value;
  }

  return nullptr;
}

std::shared_ptr<Value> BoundIRGenerator::emitFailableFieldLoad(
    const std::shared_ptr<Value> &value, int fieldIndex,
    const std::shared_ptr<Type> &fieldType) {
  if (fieldType && fieldType->getKind() == TypeKind::Void) {
    return std::make_shared<Constant>("0", fieldType);
  }
  auto fieldAddr = createRegister(std::make_shared<PointerType>(fieldType));
  currentBlock_->addInstruction(
      std::make_unique<GetElementPtrInst>(fieldAddr, value, fieldIndex));
  auto loaded = createRegister(fieldType);
  currentBlock_->addInstruction(std::make_unique<LoadInst>(loaded, fieldAddr));
  if (!containsManagedValues(fieldType)) {
    return loaded;
  }

  // A failable aggregate owns its managed payload. The aggregate is released
  // after its selected field is read, so the extracted value needs its own
  // reference before that release.
  auto copied = createRegister(fieldType, ownedForType(fieldType));
  currentBlock_->addInstruction(std::make_unique<CopyInst>(copied, loaded));
  return copied;
}

std::shared_ptr<Value>
BoundIRGenerator::emitFailableOk(const std::shared_ptr<Value> &value) {
  return emitFailableFieldLoad(value, FailableTypeLayout::OkField,
                               std::make_shared<PrimitiveType>(TypeKind::Bool));
}

std::shared_ptr<Value>
BoundIRGenerator::emitFailableValue(const std::shared_ptr<Value> &value) {
  auto layout = getFailableTypeLayout(value->getType());
  return emitFailableFieldLoad(value, FailableTypeLayout::ValueField,
                               layout ? layout->valueType : nullptr);
}

std::shared_ptr<Value>
BoundIRGenerator::emitFailableError(const std::shared_ptr<Value> &value) {
  auto layout = getFailableTypeLayout(value->getType());
  return emitFailableFieldLoad(value, FailableTypeLayout::ErrorField,
                               layout ? layout->errorType : nullptr);
}

std::unique_ptr<Module> BoundIRGenerator::generate(sema::BoundRootNode &root) {
  module_ = std::make_unique<Module>("zap_module");
  globalSymbolMap_.clear();
  reachability_ = FunctionReachabilityAnalyzer{}.analyze(root);
  root.accept(*this);
  return std::move(module_);
}

void BoundIRGenerator::visit(sema::BoundRootNode &node) {
  for (const auto &record : node.records) {
    record->accept(*this);
  }
  for (const auto &en : node.enums) {
    en->accept(*this);
  }
  for (const auto &taggedUnion : node.taggedUnions) {
    taggedUnion->accept(*this);
  }
  for (const auto &global : node.globals) {
    global->accept(*this);
  }
  for (const auto &extFunc : node.externalFunctions) {
    const auto &symbol = extFunc->symbol;
    if (symbol && (reachability_.externalFunctions.count(symbol.get()) != 0 ||
                   reachability_.referencedFunctionLinkNames.count(
                       symbol->linkName) != 0 ||
                   symbol->hasNoMangle || symbol->hasEntry)) {
      extFunc->accept(*this);
    }
  }
  for (const auto &func : node.functions) {
    const auto &symbol = func->symbol;
    if (symbol &&
        (symbol->isEntryModule || symbol->hasNoMangle || symbol->hasEntry ||
         reachability_.functions.count(symbol.get()) != 0)) {
      func->accept(*this);
    }
  }
}

void BoundIRGenerator::visit(sema::BoundFunctionDeclaration &node) {
  auto symbol = node.symbol;
  auto func = std::make_unique<Function>(
      symbol->linkName, symbol->returnType, symbol->ownerTypeCodegenName,
      symbol->isDestructor, symbol->vtableSlot, symbol->isCVariadic);
  func->returnsRef = symbol->returnsRef;
  func->resultBorrow = symbol->resultBorrow;
  currentFunction_ = func.get();

  auto entryBlock = std::make_unique<BasicBlock>("entry");
  currentBlock_ = entryBlock.get();
  currentFunction_->addBlock(std::move(entryBlock));

  for (const auto &paramSymbol : symbol->parameters) {
    auto argType = paramSymbol->is_ref
                       ? std::static_pointer_cast<Type>(
                             std::make_shared<PointerType>(paramSymbol->type))
                       : paramSymbol->type;
    const size_t parameterIndex = currentFunction_->arguments.size();
    const auto parameterOwnership =
        parameterOwnershipFor(*symbol, parameterIndex);
    auto arg = std::make_shared<Argument>(
        paramSymbol->name, argType, paramSymbol->is_ref,
        paramSymbol->is_variadic_pack, paramSymbol->variadic_element_type,
        parameterOwnership, parameterEscapeFor(*symbol, parameterIndex));
    const bool borrowedSelf =
        !symbol->ownerTypeCodegenName.empty() && paramSymbol->name == "self";
    if (!paramSymbol->is_ref && !paramSymbol->is_variadic_pack &&
        !borrowedSelf && containsManagedValues(argType)) {
      arg->setOwnership(ownedForType(argType));
    }
    currentFunction_->arguments.push_back(arg);

    auto spillType = paramSymbol->is_ref
                         ? std::make_shared<PointerType>(paramSymbol->type)
                         : paramSymbol->type;
    auto allocaReg = createRegister(std::make_shared<PointerType>(spillType));
    currentBlock_->addInstruction(
        std::make_unique<AllocaInst>(allocaReg, spillType));
    currentBlock_->addInstruction(
        std::make_unique<StoreInst>(arg, allocaReg, StoreMode::Assign));

    symbolMap_[paramSymbol] = allocaReg;
  }

  if (node.body) {
    node.body->accept(*this);
  }

  if (currentBlock_ && !isTerminated(currentBlock_)) {
    if (symbol->returnType->getKind() == TypeKind::Void) {
      emitReturn();
    } else {
      auto dummy = std::make_shared<Constant>("0", symbol->returnType);
      emitReturn(dummy);
    }
  }

  module_->addFunction(std::move(func));
  currentFunction_ = nullptr;
  currentBlock_ = nullptr;
  symbolMap_.clear();
}

void BoundIRGenerator::visit(sema::BoundExternalFunctionDeclaration &node) {
  auto symbol = node.symbol;
  auto func = std::make_unique<Function>(
      symbol->linkName, symbol->returnType, symbol->ownerTypeCodegenName,
      symbol->isDestructor, symbol->vtableSlot, symbol->isCVariadic);
  func->returnsRef = symbol->returnsRef;
  func->resultBorrow = symbol->resultBorrow;

  for (size_t parameterIndex = 0; parameterIndex < symbol->parameters.size();
       ++parameterIndex) {
    const auto &paramSymbol = symbol->parameters[parameterIndex];
    auto argType = paramSymbol->is_ref
                       ? std::static_pointer_cast<Type>(
                             std::make_shared<PointerType>(paramSymbol->type))
                       : paramSymbol->type;
    const auto parameterOwnership =
        symbol->ownerTypeCodegenName.empty()
            ? ParameterOwnership::Borrow
            : parameterOwnershipFor(*symbol, parameterIndex);
    auto arg = std::make_shared<Argument>(
        paramSymbol->name, argType, paramSymbol->is_ref,
        paramSymbol->is_variadic_pack, paramSymbol->variadic_element_type,
        parameterOwnership, parameterEscapeFor(*symbol, parameterIndex));
    func->arguments.push_back(arg);
  }

  module_->addExternalFunction(std::move(func));
}

void BoundIRGenerator::visit(sema::BoundBlock &node) {
  std::vector<std::shared_ptr<sema::VariableSymbol>> blockClassLocals;
  for (const auto &stmt : node.statements) {
    if (isTerminated(currentBlock_)) {
      break;
    }
    stmt->accept(*this);
    if (currentFunction_) {
      if (auto *varDecl =
              dynamic_cast<sema::BoundVariableDeclaration *>(stmt.get())) {
        if (varDecl->symbol->type &&
            varDecl->symbol->type->getKind() == TypeKind::Class) {
          blockClassLocals.push_back(varDecl->symbol);
        }
      }
    }
  }
  if (node.result) {
    node.result->accept(*this);
  }

  if (currentFunction_ && currentBlock_ && !isTerminated(currentBlock_)) {
    for (auto it = blockClassLocals.rbegin(); it != blockClassLocals.rend();
         ++it) {
      auto symbolIt = symbolMap_.find(*it);
      if (symbolIt == symbolMap_.end()) {
        continue;
      }
      currentBlock_->addInstruction(std::make_unique<StoreInst>(
          std::make_shared<Constant>("null", (*it)->type), symbolIt->second,
          StoreMode::Assign));
    }
  }
}

void BoundIRGenerator::visit(sema::BoundVariableDeclaration &node) {
  auto type = node.symbol->type;

  if (!currentFunction_) {
    if (node.symbol->is_external) {
      auto global = std::make_shared<Global>(
          node.symbol->name, node.symbol->linkName, type, nullptr, false);
      module_->addExternalGlobal(global);
      globalSymbolMap_[node.symbol] = global;
      return;
    }
    std::shared_ptr<Value> initializer = nullptr;
    if (node.initializer) {
      initializer = lowerConstantExpression(*node.initializer);
      if (!initializer) {
        node.initializer->accept(*this);
        if (!valueStack_.empty()) {
          initializer = valueStack_.top();
          valueStack_.pop();
        }
      }
    }
    auto global = std::make_shared<Global>(
        node.symbol->name, node.symbol->linkName, type, initializer,
        node.symbol->isCompileTimeConstant());
    module_->addGlobal(global);
    globalSymbolMap_[node.symbol] = global;
    return;
  }

  if (node.symbol->is_ref) {
    // Ref var: alloca a pointer slot, store the address of the initializer
    auto ptrType = std::make_shared<PointerType>(type);
    auto refReg = createRegister(std::make_shared<PointerType>(ptrType));
    currentBlock_->addInstruction(
        std::make_unique<AllocaInst>(refReg, ptrType));
    symbolMap_[node.symbol] = refReg;
    if (node.initializer) {
      bool old = evaluateAsAddress_;
      evaluateAsAddress_ = true;
      node.initializer->accept(*this);
      evaluateAsAddress_ = old;
      auto addr = valueStack_.top();
      valueStack_.pop();
      emitInitializationStore(std::move(addr), refReg);
    }
    return;
  }

  auto reg = createRegister(std::make_shared<PointerType>(type));
  currentBlock_->addInstruction(std::make_unique<AllocaInst>(reg, type));
  symbolMap_[node.symbol] = reg;

  if (node.initializer) {
    node.initializer->accept(*this);
    auto val = valueStack_.top();
    valueStack_.pop();

    emitInitializationStore(std::move(val), reg);
  }
}

void BoundIRGenerator::visit(sema::BoundReturnStatement &node) {
  std::shared_ptr<Value> val = nullptr;
  if (node.expression) {
    bool oldEvaluateAsAddress = evaluateAsAddress_;
    if (node.returnsRef) {
      evaluateAsAddress_ = true;
    }
    node.expression->accept(*this);
    evaluateAsAddress_ = oldEvaluateAsAddress;
    val = valueStack_.top();
    valueStack_.pop();
  }
  emitReturn(val);
}

void BoundIRGenerator::visit(sema::BoundFailStatement &node) {
  if (!currentFunction_) {
    return;
  }

  std::shared_ptr<Value> errValue = nullptr;
  if (node.errorExpression) {
    node.errorExpression->accept(*this);
    errValue = valueStack_.top();
    valueStack_.pop();
  }

  auto failableType = node.propagatedType;
  auto failableLayout = getFailableTypeLayout(failableType);
  auto valueType = failableLayout ? failableLayout->valueType : nullptr;
  auto errorType = failableLayout ? failableLayout->errorType : nullptr;

  auto allocaReg = createRegister(std::make_shared<PointerType>(failableType));
  currentBlock_->addInstruction(
      std::make_unique<AllocaInst>(allocaReg, failableType));

  auto okAddr = createRegister(std::make_shared<PointerType>(
      std::make_shared<PrimitiveType>(TypeKind::Bool)));
  currentBlock_->addInstruction(std::make_unique<GetElementPtrInst>(
      okAddr, allocaReg, FailableTypeLayout::OkField));
  currentBlock_->addInstruction(std::make_unique<StoreInst>(
      std::make_shared<Constant>(
          "false", std::make_shared<PrimitiveType>(TypeKind::Bool)),
      okAddr, StoreMode::Assign));

  auto valueAddr = createRegister(std::make_shared<PointerType>(valueType));
  currentBlock_->addInstruction(std::make_unique<GetElementPtrInst>(
      valueAddr, allocaReg, FailableTypeLayout::ValueField));
  if (valueType && valueType->getKind() != TypeKind::Void) {
    // The value field is unused on the error path; zero it without ARC.
    currentBlock_->addInstruction(
        std::make_unique<StoreInst>(std::make_shared<Constant>("0", valueType),
                                    valueAddr, StoreMode::RawInitialize));
  }

  auto errAddr = createRegister(std::make_shared<PointerType>(errorType));
  currentBlock_->addInstruction(std::make_unique<GetElementPtrInst>(
      errAddr, allocaReg, FailableTypeLayout::ErrorField));
  if (errValue) {
    currentBlock_->addInstruction(
        std::make_unique<StoreInst>(errValue, errAddr, StoreMode::Assign));
  } else {
    currentBlock_->addInstruction(
        std::make_unique<StoreInst>(std::make_shared<Constant>("0", errorType),
                                    errAddr, StoreMode::Assign));
  }

  auto loaded = createRegister(failableType);
  currentBlock_->addInstruction(std::make_unique<LoadInst>(loaded, allocaReg));
  emitReturn(loaded);
}

void BoundIRGenerator::visit(sema::BoundAssignment &node) {
  bool oldEvaluateAsAddress = evaluateAsAddress_;
  evaluateAsAddress_ = true;
  node.target->accept(*this);
  evaluateAsAddress_ = oldEvaluateAsAddress;
  auto target = valueStack_.top();
  valueStack_.pop();

  auto oldCompoundTargetAddr = compoundTargetAddr_;
  if (node.isCompound)
    compoundTargetAddr_ = target;

  node.expression->accept(*this);
  auto val = valueStack_.top();
  valueStack_.pop();

  compoundTargetAddr_ = oldCompoundTargetAddr;
  if (val && containsManagedValues(val->getType())) {
    auto copied = createRegister(val->getType(), ownedForType(val->getType()));
    currentBlock_->addInstruction(std::make_unique<CopyInst>(copied, val));
    val = std::move(copied);
  }
  currentBlock_->addInstruction(
      std::make_unique<StoreInst>(val, target, StoreMode::Assign));
}

void BoundIRGenerator::visit(sema::BoundCompoundTargetLoad &node) {
  auto reg = createRegister(node.type);
  currentBlock_->addInstruction(
      std::make_unique<LoadInst>(reg, compoundTargetAddr_));
  valueStack_.push(reg);
}

void BoundIRGenerator::visit(sema::BoundExpressionStatement &node) {
  node.expression->accept(*this);
  if (!valueStack_.empty())
    valueStack_.pop();
}

void BoundIRGenerator::visit(sema::BoundLiteral &node) {
  valueStack_.push(std::make_shared<Constant>(node.value, node.type));
}

void BoundIRGenerator::visit(sema::BoundVariableExpression &node) {
  std::shared_ptr<Value> addr = nullptr;
  auto localIt = symbolMap_.find(node.symbol);
  if (localIt != symbolMap_.end()) {
    addr = localIt->second;
  } else {
    auto globalIt = globalSymbolMap_.find(node.symbol);
    if (globalIt != globalSymbolMap_.end()) {
      addr = globalIt->second;
    }
  }
  if (!addr) {
    std::cerr << "Error: Symbol " << node.symbol->name
              << " not found in IR symbol map\n";
    return;
  }
  if (node.symbol->is_ref) {
    auto ptrReg =
        createRegister(std::make_shared<PointerType>(node.symbol->type));
    currentBlock_->addInstruction(std::make_unique<LoadInst>(ptrReg, addr));
    if (evaluateAsAddress_) {
      valueStack_.push(ptrReg);
      return;
    }
    auto valueReg = createRegister(node.type);
    currentBlock_->addInstruction(std::make_unique<LoadInst>(valueReg, ptrReg));
    valueStack_.push(valueReg);
    return;
  }
  if (evaluateAsAddress_) {
    valueStack_.push(addr);
    return;
  }
  auto storedType =
      std::static_pointer_cast<PointerType>(addr->getType())->getBaseType();
  auto loaded = createRegister(storedType);
  currentBlock_->addInstruction(std::make_unique<LoadInst>(loaded, addr));
  if (storedType->toString() == node.type->toString()) {
    valueStack_.push(loaded);
    return;
  }
  auto reg = createRegister(node.type);
  currentBlock_->addInstruction(
      std::make_unique<CastInst>(reg, loaded, node.type));
  valueStack_.push(reg);
}

void BoundIRGenerator::visit(sema::BoundClassTypeTest &node) {
  node.expression->accept(*this);
  auto object = valueStack_.top();
  valueStack_.pop();
  auto result = createRegister(node.type);
  currentBlock_->addInstruction(
      std::make_unique<ClassIsInst>(result, object, node.targetType));
  valueStack_.push(result);
}

void BoundIRGenerator::visit(sema::BoundBinaryExpression &node) {
  if (node.op == "&&") {
    auto rhsLabel = createBlockLabel("and.rhs");
    auto mergeLabel = createBlockLabel("and.merge");

    node.left->accept(*this);
    auto leftVal = valueStack_.top();
    valueStack_.pop();
    std::string leftBlockLabel = currentBlock_->label;

    currentBlock_->addInstruction(
        std::make_unique<CondBranchInst>(leftVal, rhsLabel, mergeLabel));

    auto rhsBlock = std::make_unique<BasicBlock>(rhsLabel);
    auto *rhsBlockPtr = rhsBlock.get();
    currentFunction_->addBlock(std::move(rhsBlock));
    currentBlock_ = rhsBlockPtr;

    node.right->accept(*this);
    auto rightVal = valueStack_.top();
    valueStack_.pop();
    std::string actualRhsBlockLabel = currentBlock_->label;
    currentBlock_->addInstruction(std::make_unique<BranchInst>(mergeLabel));

    auto mergeBlock = std::make_unique<BasicBlock>(mergeLabel);
    auto *mergeBlockPtr = mergeBlock.get();
    currentFunction_->addBlock(std::move(mergeBlock));
    currentBlock_ = mergeBlockPtr;

    std::vector<std::pair<std::string, std::shared_ptr<Value>>> incoming;
    incoming.push_back(
        {leftBlockLabel, std::make_shared<Constant>("false", node.type)});
    incoming.push_back({actualRhsBlockLabel, rightVal});

    auto res = createRegister(node.type, ownershipForPhi(node.type, incoming));
    currentBlock_->addInstruction(std::make_unique<PhiInst>(res, incoming));
    valueStack_.push(res);
    return;
  }

  if (node.op == "||") {
    auto rhsLabel = createBlockLabel("or.rhs");
    auto mergeLabel = createBlockLabel("or.merge");

    node.left->accept(*this);
    auto leftVal = valueStack_.top();
    valueStack_.pop();
    std::string leftBlockLabel = currentBlock_->label;

    currentBlock_->addInstruction(
        std::make_unique<CondBranchInst>(leftVal, mergeLabel, rhsLabel));

    auto rhsBlock = std::make_unique<BasicBlock>(rhsLabel);
    auto *rhsBlockPtr = rhsBlock.get();
    currentFunction_->addBlock(std::move(rhsBlock));
    currentBlock_ = rhsBlockPtr;

    node.right->accept(*this);
    auto rightVal = valueStack_.top();
    valueStack_.pop();
    std::string actualRhsBlockLabel = currentBlock_->label;
    currentBlock_->addInstruction(std::make_unique<BranchInst>(mergeLabel));

    auto mergeBlock = std::make_unique<BasicBlock>(mergeLabel);
    auto *mergeBlockPtr = mergeBlock.get();
    currentFunction_->addBlock(std::move(mergeBlock));
    currentBlock_ = mergeBlockPtr;

    std::vector<std::pair<std::string, std::shared_ptr<Value>>> incoming;
    incoming.push_back(
        {leftBlockLabel, std::make_shared<Constant>("true", node.type)});
    incoming.push_back({actualRhsBlockLabel, rightVal});

    auto res = createRegister(node.type, ownershipForPhi(node.type, incoming));
    currentBlock_->addInstruction(std::make_unique<PhiInst>(res, incoming));
    valueStack_.push(res);
    return;
  }

  node.left->accept(*this);
  auto left = valueStack_.top();
  valueStack_.pop();

  node.right->accept(*this);
  auto right = valueStack_.top();
  valueStack_.pop();

  const bool ownsStringConcat =
      node.op == "+" && containsManagedValues(node.type) &&
      (node.left->type->getIntrinsicKind() == IntrinsicTypeKind::String ||
       node.right->type->getIntrinsicKind() == IntrinsicTypeKind::String ||
       node.left->type->getKind() == TypeKind::Char ||
       node.right->type->getKind() == TypeKind::Char);
  auto reg =
      createRegister(node.type, ownsStringConcat ? ValueOwnership::OwnedStrong
                                                 : ValueOwnership::Borrowed);
  bool isUnsigned = node.left->type->isUnsigned();
  if (node.op == "==" || node.op == "!=" || node.op == "<" ||
      (node.op == ">") || (node.op == "<=") || (node.op == ">=")) {
    std::string pred;
    if (node.op == "==")
      pred = "eq";
    else if (node.op == "!=")
      pred = "ne";
    else if (node.op == "<")
      pred = isUnsigned ? "ult" : "slt";
    else if (node.op == ">")
      pred = isUnsigned ? "ugt" : "sgt";
    else if (node.op == "<=")
      pred = isUnsigned ? "ule" : "sle";
    else if (node.op == ">=")
      pred = isUnsigned ? "uge" : "sge";

    currentBlock_->addInstruction(
        std::make_unique<CmpInst>(pred, reg, left, right));
  } else {
    OpCode op;
    if (node.op == "+")
      op = OpCode::Add;
    else if (node.op == "-")
      op = OpCode::Sub;
    else if (node.op == "*")
      op = OpCode::Mul;
    else if (node.op == "/")
      op = isUnsigned ? OpCode::UDiv : OpCode::SDiv;
    else if (node.op == "%")
      op = isUnsigned ? OpCode::URem : OpCode::SRem;
    else if (node.op == "<<")
      op = OpCode::Shl;
    else if (node.op == ">>")
      op = isUnsigned ? OpCode::LShr : OpCode::AShr;
    else if (node.op == "&")
      op = OpCode::BitAnd;
    else if (node.op == "|")
      op = OpCode::BitOr;
    else if (node.op == "^")
      op = OpCode::BitXor;
    else
      op = OpCode::Add;

    currentBlock_->addInstruction(
        std::make_unique<BinaryInst>(op, reg, left, right));
  }
  valueStack_.push(reg);
}

void BoundIRGenerator::visit(sema::BoundTernaryExpression &node) {
  auto thenLabel = createBlockLabel("ternary.then");
  auto elseLabel = createBlockLabel("ternary.else");
  auto mergeLabel = createBlockLabel("ternary.merge");

  node.condition->accept(*this);
  auto condVal = valueStack_.top();
  valueStack_.pop();

  currentBlock_->addInstruction(
      std::make_unique<CondBranchInst>(condVal, thenLabel, elseLabel));

  auto thenBlock = std::make_unique<BasicBlock>(thenLabel);
  auto *thenBlockPtr = thenBlock.get();
  currentFunction_->addBlock(std::move(thenBlock));
  currentBlock_ = thenBlockPtr;

  node.thenExpr->accept(*this);
  auto thenVal = valueStack_.top();
  valueStack_.pop();
  std::string actualThenLabel = currentBlock_->label;
  currentBlock_->addInstruction(std::make_unique<BranchInst>(mergeLabel));

  auto elseBlock = std::make_unique<BasicBlock>(elseLabel);
  auto *elseBlockPtr = elseBlock.get();
  currentFunction_->addBlock(std::move(elseBlock));
  currentBlock_ = elseBlockPtr;

  node.elseExpr->accept(*this);
  auto elseVal = valueStack_.top();
  valueStack_.pop();
  std::string actualElseLabel = currentBlock_->label;
  currentBlock_->addInstruction(std::make_unique<BranchInst>(mergeLabel));

  auto mergeBlock = std::make_unique<BasicBlock>(mergeLabel);
  auto *mergeBlockPtr = mergeBlock.get();
  currentFunction_->addBlock(std::move(mergeBlock));
  currentBlock_ = mergeBlockPtr;

  std::vector<std::pair<std::string, std::shared_ptr<Value>>> incoming;
  incoming.push_back({actualThenLabel, thenVal});
  incoming.push_back({actualElseLabel, elseVal});
  auto res = createRegister(node.type, ownershipForPhi(node.type, incoming));
  currentBlock_->addInstruction(std::make_unique<PhiInst>(res, incoming));
  valueStack_.push(res);
}

void BoundIRGenerator::visit(sema::BoundFunctionCall &node) {
  std::vector<std::shared_ptr<Value>> args;
  for (size_t i = 0; i < node.arguments.size(); ++i) {
    bool oldEvaluateAsAddress = evaluateAsAddress_;
    evaluateAsAddress_ =
        (i < node.argumentIsRef.size() && node.argumentIsRef[i]);
    node.arguments[i]->accept(*this);
    evaluateAsAddress_ = oldEvaluateAsAddress;
    auto argument = valueStack_.top();
    valueStack_.pop();
    const auto parameterOwnership = parameterOwnershipFor(*node.symbol, i);
    prepareCallArgument(argument, parameterOwnership);
    args.push_back(std::move(argument));
  }

  std::shared_ptr<Value> variadicPack = nullptr;
  if (node.variadicPack) {
    node.variadicPack->accept(*this);
    variadicPack = valueStack_.top();
    valueStack_.pop();
  }

  // ref-returning functions return a pointer in LLVM IR
  auto resultType = node.symbol->returnsRef
                        ? std::static_pointer_cast<zir::Type>(
                              std::make_shared<PointerType>(node.type))
                        : node.type;
  const bool ownsResult =
      !node.symbol->returnsRef && containsManagedValues(node.type);
  auto reg = createRegister(resultType, ownsResult ? ownedForType(node.type)
                                                   : ValueOwnership::Borrowed);
  currentBlock_->addInstruction(std::make_unique<CallInst>(
      reg, node.symbol->linkName, args, node.argumentIsRef, variadicPack));

  // If ref-returning function is used as value (not address), load it
  if (node.symbol->returnsRef && !evaluateAsAddress_) {
    auto loadReg = createRegister(node.type);
    currentBlock_->addInstruction(std::make_unique<LoadInst>(loadReg, reg));
    valueStack_.push(loadReg);
  } else {
    valueStack_.push(reg);
  }
}

void BoundIRGenerator::visit(sema::BoundFunctionReference &node) {
  auto functionType = std::static_pointer_cast<FunctionPointerType>(node.type);
  valueStack_.push(std::make_shared<FunctionReference>(
      node.symbol->linkName, std::move(functionType)));
}

void BoundIRGenerator::visit(sema::BoundIndirectCall &node) {
  node.callee->accept(*this);
  auto calleeVal = valueStack_.top();
  valueStack_.pop();

  std::vector<std::shared_ptr<Value>> args;
  const auto functionType =
      std::static_pointer_cast<FunctionPointerType>(calleeVal->getType());
  for (size_t i = 0; i < node.arguments.size(); ++i) {
    auto &arg = node.arguments[i];
    arg->accept(*this);
    auto argument = valueStack_.top();
    valueStack_.pop();
    const auto parameterOwnership =
        i < functionType->getParameterOwnership().size()
            ? functionType->getParameterOwnership()[i]
            : ParameterOwnership::Borrow;
    prepareCallArgument(argument, parameterOwnership);
    args.push_back(std::move(argument));
  }

  const bool returnsRef = functionType->returnsRef();
  auto resultType = returnsRef ? std::static_pointer_cast<zir::Type>(
                                     std::make_shared<PointerType>(node.type))
                               : node.type;
  const bool ownsResult = !returnsRef && containsManagedValues(node.type);
  auto reg = createRegister(resultType, ownsResult ? ownedForType(node.type)
                                                   : ValueOwnership::Borrowed);
  currentBlock_->addInstruction(
      std::make_unique<CallInst>(reg, calleeVal, std::move(args)));
  if (returnsRef && !evaluateAsAddress_) {
    auto loadReg = createRegister(node.type);
    currentBlock_->addInstruction(std::make_unique<LoadInst>(loadReg, reg));
    valueStack_.push(loadReg);
  } else {
    valueStack_.push(reg);
  }
}

std::shared_ptr<Value>
BoundIRGenerator::createRegister(std::shared_ptr<Type> type,
                                 ValueOwnership ownership) {
  auto result =
      std::make_shared<Register>(std::to_string(nextRegisterId_++), type);
  result->setOwnership(ownership);
  return result;
}

void BoundIRGenerator::emitInitializationStore(
    std::shared_ptr<Value> value, std::shared_ptr<Value> destination) {
  if (value && containsManagedValues(value->getType())) {
    const auto resultOwnership = isOwned(value->getOwnership())
                                     ? value->getOwnership()
                                     : ownedForType(value->getType());
    auto prepared = createRegister(value->getType(), resultOwnership);
    if (isOwned(value->getOwnership())) {
      currentBlock_->addInstruction(
          std::make_unique<MoveInst>(prepared, value));
    } else {
      currentBlock_->addInstruction(
          std::make_unique<CopyInst>(prepared, value));
    }
    value = std::move(prepared);
  }
  currentBlock_->addInstruction(std::make_unique<StoreInst>(
      std::move(value), std::move(destination), StoreMode::Initialize));
}

std::shared_ptr<Value>
BoundIRGenerator::materializeOwnedValue(std::shared_ptr<Value> value) {
  if (!value || !containsManagedValues(value->getType()) ||
      isOwned(value->getOwnership())) {
    return value;
  }

  auto copied =
      createRegister(value->getType(), ownedForType(value->getType()));
  currentBlock_->addInstruction(std::make_unique<CopyInst>(copied, value));
  return copied;
}

void BoundIRGenerator::prepareCallArgument(
    std::shared_ptr<Value> &value, ParameterOwnership parameterOwnership) {
  if (!transfersOwnership(parameterOwnership) || !value ||
      !containsManagedValues(value->getType())) {
    return;
  }

  const bool moveOwnedValue = parameterOwnership == ParameterOwnership::Sink &&
                              isOwned(value->getOwnership());
  const auto resultOwnership =
      moveOwnedValue ? value->getOwnership() : ownedForType(value->getType());
  auto prepared = createRegister(value->getType(), resultOwnership);
  if (moveOwnedValue) {
    currentBlock_->addInstruction(std::make_unique<MoveInst>(prepared, value));
  } else {
    currentBlock_->addInstruction(std::make_unique<CopyInst>(prepared, value));
  }
  value = std::move(prepared);
}

void BoundIRGenerator::emitReturn(std::shared_ptr<Value> value) {
  if (value && isOwned(value->getOwnership()) &&
      containsManagedValues(value->getType())) {
    auto moved = createRegister(value->getType(), value->getOwnership());
    currentBlock_->addInstruction(std::make_unique<MoveInst>(moved, value));
    value = std::move(moved);
  }
  currentBlock_->addInstruction(std::make_unique<ReturnInst>(std::move(value)));
}

std::string BoundIRGenerator::createBlockLabel(const std::string &prefix) {
  return prefix + "." + std::to_string(nextBlockId_++);
}

void BoundIRGenerator::visit(sema::BoundUnaryExpression &node) {
  if (node.op == "&") {
    bool oldEvaluateAsAddress = evaluateAsAddress_;
    evaluateAsAddress_ = true;
    node.expr->accept(*this);
    evaluateAsAddress_ = oldEvaluateAsAddress;
    return;
  }

  if (node.op == "*") {
    bool oldEvaluateAsAddress = evaluateAsAddress_;
    evaluateAsAddress_ = false;
    node.expr->accept(*this);
    evaluateAsAddress_ = oldEvaluateAsAddress;

    auto ptr = valueStack_.top();
    valueStack_.pop();
    if (evaluateAsAddress_) {
      valueStack_.push(ptr);
      return;
    }

    auto reg = createRegister(node.type);
    currentBlock_->addInstruction(std::make_unique<LoadInst>(reg, ptr));
    valueStack_.push(reg);
    return;
  }

  node.expr->accept(*this);
  auto expr = valueStack_.top();
  valueStack_.pop();

  // Global initializers are lowered as constants only. Fold unary operators on
  // constants instead of emitting runtime IR outside functions.
  if (!currentFunction_ || !currentBlock_) {
    if (auto c = std::dynamic_pointer_cast<Constant>(expr)) {
      const auto &lit = c->getLiteral();

      if (node.op == "+") {
        valueStack_.push(c);
        return;
      }

      if (node.op == "-") {
        if (!lit.empty() && lit != "true" && lit != "false" && lit != "null" &&
            lit[0] != '\'' && lit[0] != '\\') {
          try {
            if (c->getType() && c->getType()->isFloatingPoint()) {
              auto v = std::stod(lit);
              valueStack_.push(
                  std::make_shared<Constant>(std::to_string(-v), node.type));
              return;
            }
            if (c->getType() && c->getType()->isInteger()) {
              if (c->getType()->isUnsigned()) {
                auto v = std::stoull(lit);
                auto out = static_cast<int64_t>(-(static_cast<int64_t>(v)));
                valueStack_.push(
                    std::make_shared<Constant>(std::to_string(out), node.type));
              } else {
                auto v = std::stoll(lit);
                valueStack_.push(
                    std::make_shared<Constant>(std::to_string(-v), node.type));
              }
              return;
            }
          } catch (...) {
          }
        }
      }

      if (node.op == "!") {
        if (lit == "true") {
          valueStack_.push(std::make_shared<Constant>("false", node.type));
          return;
        }
        if (lit == "false") {
          valueStack_.push(std::make_shared<Constant>("true", node.type));
          return;
        }
      }

      if (node.op == "~") {
        if (!lit.empty() && lit != "true" && lit != "false" && lit != "null" &&
            lit[0] != '\'' && lit[0] != '\\') {
          try {
            auto v = std::stoll(lit);
            valueStack_.push(
                std::make_shared<Constant>(std::to_string(~v), node.type));
            return;
          } catch (...) {
          }
        }
      }
    }

    valueStack_.push(expr);
    return;
  }

  if (node.op == "+") {
    valueStack_.push(expr);
    return;
  }

  if (node.op == "-") {
    auto zero = std::make_shared<Constant>("0", node.type);
    auto reg = createRegister(node.type);
    currentBlock_->addInstruction(
        std::make_unique<BinaryInst>(OpCode::Sub, reg, zero, expr));
    valueStack_.push(reg);
    return;
  }

  if (node.op == "!") {
    auto zero = std::make_shared<Constant>(
        "false", std::make_shared<PrimitiveType>(TypeKind::Bool));
    auto reg = createRegister(node.type);
    currentBlock_->addInstruction(
        std::make_unique<CmpInst>("eq", reg, expr, zero));
    valueStack_.push(reg);
    return;
  }

  if (node.op == "~") {
    auto allOnes = std::make_shared<Constant>("-1", node.type);
    auto reg = createRegister(node.type);
    currentBlock_->addInstruction(
        std::make_unique<BinaryInst>(OpCode::BitXor, reg, expr, allOnes));
    valueStack_.push(reg);
    return;
  }

  valueStack_.push(expr);
}

void BoundIRGenerator::visit(sema::BoundArrayLiteral &node) {
  auto arrayType = std::static_pointer_cast<zir::ArrayType>(node.type);
  auto allocaReg = createRegister(std::make_shared<PointerType>(arrayType));
  currentBlock_->addInstruction(
      std::make_unique<AllocaInst>(allocaReg, arrayType));

  for (size_t i = 0; i < node.elements.size(); ++i) {
    node.elements[i]->accept(*this);
    auto value = std::move(valueStack_.top());
    valueStack_.pop();

    auto elementAddr =
        createRegister(std::make_shared<PointerType>(arrayType->getBaseType()));
    currentBlock_->addInstruction(std::make_unique<GetElementPtrInst>(
        elementAddr, allocaReg, static_cast<int>(i)));
    currentBlock_->addInstruction(
        std::make_unique<StoreInst>(value, elementAddr, StoreMode::Assign));
  }

  auto result = createRegister(arrayType);
  currentBlock_->addInstruction(std::make_unique<LoadInst>(result, allocaReg));
  valueStack_.push(result);
}

void BoundIRGenerator::visit(sema::BoundRecordDeclaration &node) {
  module_->addType(node.type);
}

void BoundIRGenerator::visit(sema::BoundEnumDeclaration &node) {
  module_->addType(node.type);
}

void BoundIRGenerator::visit(sema::BoundTaggedUnionDeclaration &node) {
  module_->addType(node.type);
}

void BoundIRGenerator::visit(sema::BoundMemberAccess &node) {
  if (node.left->type->getKind() == zir::TypeKind::Enum) {
    node.left->accept(*this);
    auto left = std::move(valueStack_.top());
    valueStack_.pop();

    auto enumType = std::static_pointer_cast<zir::EnumType>(left->getType());
    int64_t value = enumType->getVariantDiscriminant(node.member);
    if (value != -1) {
      valueStack_.push(std::make_shared<Constant>(
          std::to_string(value),
          std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int)));
      return;
    }
  }
  if (node.left->type->getKind() == zir::TypeKind::TaggedUnion &&
      node.member == "tag") {
    bool oldEvaluateAsAddress = evaluateAsAddress_;
    evaluateAsAddress_ = true;
    node.left->accept(*this);
    evaluateAsAddress_ = oldEvaluateAsAddress;

    auto left = std::move(valueStack_.top());
    valueStack_.pop();
    auto fieldAddr = createRegister(std::make_shared<PointerType>(
        std::make_shared<PrimitiveType>(TypeKind::Int32)));
    currentBlock_->addInstruction(
        std::make_unique<GetElementPtrInst>(fieldAddr, left, 0));
    if (evaluateAsAddress_) {
      valueStack_.push(fieldAddr);
    } else {
      auto result = createRegister(node.type);
      currentBlock_->addInstruction(
          std::make_unique<LoadInst>(result, fieldAddr));
      valueStack_.push(result);
    }
    return;
  }

  bool oldEvaluateAsAddress = evaluateAsAddress_;
  evaluateAsAddress_ = !(node.left->type->getKind() == zir::TypeKind::Class ||
                         node.left->type->getKind() == zir::TypeKind::Pointer);
  node.left->accept(*this);
  evaluateAsAddress_ = oldEvaluateAsAddress;

  auto left = std::move(valueStack_.top());
  valueStack_.pop();

  if (node.left->type->getKind() == zir::TypeKind::Class) {
    auto classType = std::static_pointer_cast<zir::ClassType>(node.left->type);
    int fieldIndex = -1;
    const auto &fields = classType->getFields();
    for (size_t i = 0; i < fields.size(); ++i) {
      if (fields[i].name == node.member) {
        fieldIndex = static_cast<int>(i);
        break;
      }
    }

    if (fieldIndex != -1) {
      auto fieldAddr = createRegister(
          std::make_shared<PointerType>(fields[fieldIndex].type));
      currentBlock_->addInstruction(
          std::make_unique<GetElementPtrInst>(fieldAddr, left, fieldIndex));
      if (evaluateAsAddress_) {
        valueStack_.push(fieldAddr);
      } else {
        auto result = createRegister(fields[fieldIndex].type);
        currentBlock_->addInstruction(
            std::make_unique<LoadInst>(result, fieldAddr));
        valueStack_.push(result);
      }
      return;
    }
  } else if (left->getType()->getKind() == zir::TypeKind::Pointer) {
    auto baseType = std::static_pointer_cast<zir::PointerType>(left->getType())
                        ->getBaseType();
    if (baseType->getKind() == zir::TypeKind::Class) {
      auto classType = std::static_pointer_cast<zir::ClassType>(baseType);
      int fieldIndex = -1;
      const auto &fields = classType->getFields();
      for (size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].name == node.member) {
          fieldIndex = static_cast<int>(i);
          break;
        }
      }

      if (fieldIndex != -1) {
        auto fieldAddr = createRegister(
            std::make_shared<PointerType>(fields[fieldIndex].type));
        currentBlock_->addInstruction(
            std::make_unique<GetElementPtrInst>(fieldAddr, left, fieldIndex));
        if (evaluateAsAddress_) {
          valueStack_.push(fieldAddr);
        } else {
          auto result = createRegister(fields[fieldIndex].type);
          currentBlock_->addInstruction(
              std::make_unique<LoadInst>(result, fieldAddr));
          valueStack_.push(result);
        }
        return;
      }
    } else if (baseType->getKind() == zir::TypeKind::Record) {
      auto recordType = std::static_pointer_cast<zir::RecordType>(baseType);
      int fieldIndex = -1;
      const auto &fields = recordType->getFields();
      for (size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].name == node.member) {
          fieldIndex = static_cast<int>(i);
          break;
        }
      }

      if (fieldIndex != -1) {
        auto fieldAddr = createRegister(
            std::make_shared<PointerType>(fields[fieldIndex].type));
        currentBlock_->addInstruction(
            std::make_unique<GetElementPtrInst>(fieldAddr, left, fieldIndex));
        if (evaluateAsAddress_) {
          valueStack_.push(fieldAddr);
        } else {
          auto result = createRegister(fields[fieldIndex].type);
          currentBlock_->addInstruction(
              std::make_unique<LoadInst>(result, fieldAddr));
          valueStack_.push(result);
        }
        return;
      }
    }
  } else if (left->getType()->getKind() == zir::TypeKind::Record) {
    auto recordType =
        std::static_pointer_cast<zir::RecordType>(left->getType());
    int fieldIndex = -1;
    const auto &fields = recordType->getFields();
    for (size_t i = 0; i < fields.size(); ++i) {
      if (fields[i].name == node.member) {
        fieldIndex = static_cast<int>(i);
        break;
      }
    }

    if (fieldIndex != -1) {
      auto fieldAddr = createRegister(
          std::make_shared<PointerType>(fields[fieldIndex].type));
      currentBlock_->addInstruction(
          std::make_unique<GetElementPtrInst>(fieldAddr, left, fieldIndex));
      if (evaluateAsAddress_) {
        valueStack_.push(fieldAddr);
      } else {
        auto result = createRegister(fields[fieldIndex].type);
        currentBlock_->addInstruction(
            std::make_unique<LoadInst>(result, fieldAddr));
        valueStack_.push(result);
      }
      return;
    }
  }

  throw std::runtime_error("IR member access failed: member '" + node.member +
                           "' not found in type '" +
                           renderTypeForUser(left->getType()) + "'");
}

void BoundIRGenerator::visit(sema::BoundStructLiteral &node) {
  auto recordType = std::static_pointer_cast<zir::RecordType>(node.type);

  // Global initializers are lowered as constants only.
  if (!currentFunction_ || !currentBlock_) {
    std::vector<AggregateConstant::FieldValue> aggregateFields;
    aggregateFields.reserve(node.fields.size());

    for (const auto &fieldInit : node.fields) {
      fieldInit.second->accept(*this);
      auto val = std::move(valueStack_.top());
      valueStack_.pop();
      aggregateFields.push_back({fieldInit.first, val});
    }

    valueStack_.push(std::make_shared<AggregateConstant>(
        node.type, std::move(aggregateFields)));
    return;
  }

  auto allocaReg = createRegister(std::make_shared<PointerType>(recordType));
  currentBlock_->addInstruction(
      std::make_unique<AllocaInst>(allocaReg, recordType));

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
    auto val = std::move(valueStack_.top());
    valueStack_.pop();

    if (fields[fieldIndex].type &&
        fields[fieldIndex].type->getKind() != TypeKind::Void) {
      auto fieldAddr = createRegister(
          std::make_shared<PointerType>(fields[fieldIndex].type));
      currentBlock_->addInstruction(std::make_unique<GetElementPtrInst>(
          fieldAddr, allocaReg, fieldIndex));
      emitInitializationStore(std::move(val), fieldAddr);
    }
  }

  auto result = createRegister(recordType);
  currentBlock_->addInstruction(std::make_unique<LoadInst>(result, allocaReg));
  valueStack_.push(result);
}

void BoundIRGenerator::visit(sema::BoundTaggedUnionLiteral &node) {
  auto taggedUnionType = std::static_pointer_cast<TaggedUnionType>(node.type);
  auto allocaReg =
      createRegister(std::make_shared<PointerType>(taggedUnionType));
  currentBlock_->addInstruction(
      std::make_unique<AllocaInst>(allocaReg, taggedUnionType));

  auto tagType = std::make_shared<PrimitiveType>(TypeKind::Int32);
  auto tagAddr = createRegister(std::make_shared<PointerType>(tagType));
  currentBlock_->addInstruction(
      std::make_unique<GetElementPtrInst>(tagAddr, allocaReg, 0));
  auto tagValue = std::make_shared<Constant>(std::to_string(node.tag), tagType);
  currentBlock_->addInstruction(
      std::make_unique<StoreInst>(tagValue, tagAddr, StoreMode::RawInitialize));

  if (node.payload) {
    node.payload->accept(*this);
    auto payload = std::move(valueStack_.top());
    valueStack_.pop();
    auto payloadAddr =
        createRegister(std::make_shared<PointerType>(node.payload->type));
    currentBlock_->addInstruction(
        std::make_unique<GetElementPtrInst>(payloadAddr, allocaReg, 1));
    emitInitializationStore(std::move(payload), payloadAddr);
  }

  auto result = createRegister(taggedUnionType);
  currentBlock_->addInstruction(std::make_unique<LoadInst>(result, allocaReg));
  valueStack_.push(result);
}

void BoundIRGenerator::visit(sema::BoundTryExpression &node) {
  node.expression->accept(*this);
  if (valueStack_.empty()) {
    return;
  }

  auto failableValue = valueStack_.top();
  valueStack_.pop();

  auto okValue = emitFailableOk(failableValue);
  auto successLabel = createBlockLabel("try.ok");
  auto failLabel = createBlockLabel("try.fail");
  auto mergeLabel = createBlockLabel("try.merge");

  currentBlock_->addInstruction(
      std::make_unique<CondBranchInst>(okValue, successLabel, failLabel));

  auto successBlock = std::make_unique<BasicBlock>(successLabel);
  auto *successBlockPtr = successBlock.get();
  currentFunction_->addBlock(std::move(successBlock));
  currentBlock_ = successBlockPtr;

  auto successValue = emitFailableValue(failableValue);
  std::string successFrom = currentBlock_->label;
  bool resultIsVoid = node.type && node.type->getKind() == TypeKind::Void;
  currentBlock_->addInstruction(std::make_unique<BranchInst>(mergeLabel));

  auto failBlock = std::make_unique<BasicBlock>(failLabel);
  auto *failBlockPtr = failBlock.get();
  currentFunction_->addBlock(std::move(failBlock));
  currentBlock_ = failBlockPtr;

  auto failErr = emitFailableError(failableValue);

  auto propagatedType = node.propagatedType;
  auto propagatedLayout = getFailableTypeLayout(propagatedType);
  auto propagatedValueType =
      propagatedLayout ? propagatedLayout->valueType : nullptr;
  auto propagatedErrorType =
      propagatedLayout ? propagatedLayout->errorType : nullptr;

  auto propagatedAlloca =
      createRegister(std::make_shared<PointerType>(propagatedType));
  currentBlock_->addInstruction(
      std::make_unique<AllocaInst>(propagatedAlloca, propagatedType));

  auto okAddr = createRegister(std::make_shared<PointerType>(
      std::make_shared<PrimitiveType>(TypeKind::Bool)));
  currentBlock_->addInstruction(std::make_unique<GetElementPtrInst>(
      okAddr, propagatedAlloca, FailableTypeLayout::OkField));
  currentBlock_->addInstruction(std::make_unique<StoreInst>(
      std::make_shared<Constant>(
          "false", std::make_shared<PrimitiveType>(TypeKind::Bool)),
      okAddr, StoreMode::Assign));

  auto valueAddr =
      createRegister(std::make_shared<PointerType>(propagatedValueType));
  currentBlock_->addInstruction(std::make_unique<GetElementPtrInst>(
      valueAddr, propagatedAlloca, FailableTypeLayout::ValueField));
  if (propagatedValueType && propagatedValueType->getKind() != TypeKind::Void) {
    currentBlock_->addInstruction(std::make_unique<StoreInst>(
        std::make_shared<Constant>("0", propagatedValueType), valueAddr,
        StoreMode::Assign));
  }

  auto errAddr =
      createRegister(std::make_shared<PointerType>(propagatedErrorType));
  currentBlock_->addInstruction(std::make_unique<GetElementPtrInst>(
      errAddr, propagatedAlloca, FailableTypeLayout::ErrorField));
  currentBlock_->addInstruction(
      std::make_unique<StoreInst>(failErr, errAddr, StoreMode::Assign));

  auto propagatedValue = createRegister(propagatedType);
  currentBlock_->addInstruction(
      std::make_unique<LoadInst>(propagatedValue, propagatedAlloca));
  emitReturn(propagatedValue);

  auto mergeBlock = std::make_unique<BasicBlock>(mergeLabel);
  auto *mergeBlockPtr = mergeBlock.get();
  currentFunction_->addBlock(std::move(mergeBlock));
  currentBlock_ = mergeBlockPtr;

  if (resultIsVoid) {
    valueStack_.push(std::make_shared<Constant>("0", node.type));
    return;
  }

  std::vector<std::pair<std::string, std::shared_ptr<Value>>> incoming;
  incoming.push_back({successFrom, successValue});
  auto result = createRegister(node.type, ownershipForPhi(node.type, incoming));
  currentBlock_->addInstruction(std::make_unique<PhiInst>(result, incoming));
  valueStack_.push(result);
}

void BoundIRGenerator::visit(sema::BoundFallbackExpression &node) {
  node.expression->accept(*this);
  if (valueStack_.empty()) {
    return;
  }

  auto failableValue = valueStack_.top();
  valueStack_.pop();

  auto okValue = emitFailableOk(failableValue);
  auto successLabel = createBlockLabel("or.ok");
  auto fallbackLabel = createBlockLabel("or.fallback");
  auto mergeLabel = createBlockLabel("or.merge");

  currentBlock_->addInstruction(
      std::make_unique<CondBranchInst>(okValue, successLabel, fallbackLabel));

  auto successBlock = std::make_unique<BasicBlock>(successLabel);
  auto *successBlockPtr = successBlock.get();
  currentFunction_->addBlock(std::move(successBlock));
  currentBlock_ = successBlockPtr;
  auto successValue = emitFailableValue(failableValue);
  successValue = materializeOwnedValue(successValue);
  std::string successFrom = currentBlock_->label;
  bool resultIsVoid = node.type && node.type->getKind() == TypeKind::Void;
  currentBlock_->addInstruction(std::make_unique<BranchInst>(mergeLabel));

  auto fallbackBlock = std::make_unique<BasicBlock>(fallbackLabel);
  auto *fallbackBlockPtr = fallbackBlock.get();
  currentFunction_->addBlock(std::move(fallbackBlock));
  currentBlock_ = fallbackBlockPtr;
  node.fallback->accept(*this);
  auto fallbackValue = valueStack_.top();
  valueStack_.pop();
  fallbackValue = materializeOwnedValue(fallbackValue);
  std::string fallbackFrom = currentBlock_->label;
  currentBlock_->addInstruction(std::make_unique<BranchInst>(mergeLabel));

  auto mergeBlock = std::make_unique<BasicBlock>(mergeLabel);
  auto *mergeBlockPtr = mergeBlock.get();
  currentFunction_->addBlock(std::move(mergeBlock));
  currentBlock_ = mergeBlockPtr;

  if (resultIsVoid) {
    valueStack_.push(std::make_shared<Constant>("0", node.type));
    return;
  }

  std::vector<std::pair<std::string, std::shared_ptr<Value>>> incoming;
  incoming.push_back({successFrom, successValue});
  incoming.push_back({fallbackFrom, fallbackValue});
  auto result = createRegister(node.type, ownershipForPhi(node.type, incoming));
  currentBlock_->addInstruction(std::make_unique<PhiInst>(result, incoming));
  valueStack_.push(result);
}

void BoundIRGenerator::visit(sema::BoundFailableHandleExpression &node) {
  node.expression->accept(*this);
  if (valueStack_.empty()) {
    return;
  }

  auto failableValue = valueStack_.top();
  valueStack_.pop();

  auto okValue = emitFailableOk(failableValue);
  auto successLabel = createBlockLabel("orerr.ok");
  auto handlerLabel = createBlockLabel("orerr.handler");
  auto mergeLabel = createBlockLabel("orerr.merge");

  currentBlock_->addInstruction(
      std::make_unique<CondBranchInst>(okValue, successLabel, handlerLabel));

  auto successBlock = std::make_unique<BasicBlock>(successLabel);
  auto *successBlockPtr = successBlock.get();
  currentFunction_->addBlock(std::move(successBlock));
  currentBlock_ = successBlockPtr;
  auto successValue = emitFailableValue(failableValue);
  successValue = materializeOwnedValue(successValue);
  std::string successFrom = currentBlock_->label;
  currentBlock_->addInstruction(std::make_unique<BranchInst>(mergeLabel));

  auto handlerBlock = std::make_unique<BasicBlock>(handlerLabel);
  auto *handlerBlockPtr = handlerBlock.get();
  currentFunction_->addBlock(std::move(handlerBlock));
  currentBlock_ = handlerBlockPtr;

  auto errValue = emitFailableError(failableValue);
  if (node.errorSymbol) {
    auto errAlloca =
        createRegister(std::make_shared<PointerType>(node.errorSymbol->type));
    currentBlock_->addInstruction(
        std::make_unique<AllocaInst>(errAlloca, node.errorSymbol->type));
    currentBlock_->addInstruction(
        std::make_unique<StoreInst>(errValue, errAlloca, StoreMode::Assign));
    symbolMap_[node.errorSymbol] = errAlloca;
  }

  if (node.handler) {
    node.handler->accept(*this);
  }

  bool resultIsVoid = node.type && node.type->getKind() == TypeKind::Void;
  std::shared_ptr<Value> handlerValue = nullptr;
  if (node.handler && node.handler->result) {
    handlerValue = valueStack_.top();
    valueStack_.pop();
  } else {
    handlerValue = std::make_shared<Constant>("0", node.type);
  }

  bool handlerReachesMerge = !isTerminated(currentBlock_);
  if (handlerReachesMerge) {
    handlerValue = materializeOwnedValue(handlerValue);
  }
  std::string handlerFrom = currentBlock_->label;
  if (handlerReachesMerge) {
    currentBlock_->addInstruction(std::make_unique<BranchInst>(mergeLabel));
  }

  auto mergeBlock = std::make_unique<BasicBlock>(mergeLabel);
  auto *mergeBlockPtr = mergeBlock.get();
  currentFunction_->addBlock(std::move(mergeBlock));
  currentBlock_ = mergeBlockPtr;

  if (!handlerReachesMerge) {
    valueStack_.push(resultIsVoid ? std::make_shared<Constant>("0", node.type)
                                  : successValue);
    return;
  }

  if (resultIsVoid) {
    valueStack_.push(std::make_shared<Constant>("0", node.type));
    return;
  }

  std::vector<std::pair<std::string, std::shared_ptr<Value>>> incoming;
  incoming.push_back({successFrom, successValue});
  incoming.push_back({handlerFrom, handlerValue});
  auto result = createRegister(node.type, ownershipForPhi(node.type, incoming));
  currentBlock_->addInstruction(std::make_unique<PhiInst>(result, incoming));
  valueStack_.push(result);
}

void BoundIRGenerator::visit(sema::BoundModuleReference &node) {
  (void)node;
  throw std::runtime_error("module reference reached ZIR generation");
}

void BoundIRGenerator::visit(sema::BoundIfStatement &node) {
  auto trueLabel = createBlockLabel("if.then");
  auto falseLabel = node.elseBody ? createBlockLabel("if.else") : "";
  auto mergeLabel = createBlockLabel("if.merge");

  node.condition->accept(*this);
  auto cond = valueStack_.top();
  valueStack_.pop();

  if (node.elseBody) {
    currentBlock_->addInstruction(
        std::make_unique<CondBranchInst>(cond, trueLabel, falseLabel));
  } else {
    currentBlock_->addInstruction(
        std::make_unique<CondBranchInst>(cond, trueLabel, mergeLabel));
  }

  auto thenBlock = std::make_unique<BasicBlock>(trueLabel);
  auto *thenBlockPtr = thenBlock.get();
  currentFunction_->addBlock(std::move(thenBlock));
  currentBlock_ = thenBlockPtr;

  std::shared_ptr<Value> narrowedAddress;
  if (node.narrowedSource && node.narrowedVariable) {
    auto sourceIt = symbolMap_.find(node.narrowedSource);
    if (sourceIt != symbolMap_.end()) {
      narrowedAddress = sourceIt->second;
      symbolMap_[node.narrowedVariable] = narrowedAddress;
    }
  }
  if (node.thenBody)
    node.thenBody->accept(*this);
  if (node.narrowedVariable && narrowedAddress) {
    symbolMap_.erase(node.narrowedVariable);
  }

  std::string actualThenLabel = currentBlock_->label;

  if (!isTerminated(currentBlock_)) {
    currentBlock_->addInstruction(std::make_unique<BranchInst>(mergeLabel));
  }

  std::string actualElseLabel = "";
  if (node.elseBody) {
    auto elseBlock = std::make_unique<BasicBlock>(falseLabel);
    auto *elseBlockPtr = elseBlock.get();
    currentFunction_->addBlock(std::move(elseBlock));
    currentBlock_ = elseBlockPtr;
    node.elseBody->accept(*this);

    actualElseLabel = currentBlock_->label;

    if (!isTerminated(currentBlock_)) {
      currentBlock_->addInstruction(std::make_unique<BranchInst>(mergeLabel));
    }
  }

  auto mergeBlock = std::make_unique<BasicBlock>(mergeLabel);
  auto *mergeBlockPtr = mergeBlock.get();
  currentFunction_->addBlock(std::move(mergeBlock));
  currentBlock_ = mergeBlockPtr;
}

void BoundIRGenerator::emitCaseRecordTest(const sema::BoundCasePattern &record,
                                          const std::shared_ptr<Value> &address,
                                          const std::string &successLabel,
                                          const std::string &failureLabel) {
  std::vector<const sema::BoundCaseRecordField *> constrainedFields;
  for (const auto &field : record.recordFields) {
    if (field.nested) {
      constrainedFields.push_back(&field);
    }
  }
  for (size_t fieldIndex = 0; fieldIndex < constrainedFields.size();
       ++fieldIndex) {
    const auto &field = *constrainedFields[fieldIndex];
    const auto nextLabel = fieldIndex + 1 == constrainedFields.size()
                               ? successLabel
                               : createBlockLabel("case.record.next");
    const auto fieldType = record.recordType->getFields()[field.index].type;
    auto fieldAddress =
        createRegister(std::make_shared<PointerType>(fieldType));
    currentBlock_->addInstruction(std::make_unique<GetElementPtrInst>(
        fieldAddress, address, field.index));
    if (field.nested->kind == sema::BoundCasePatternKind::Record) {
      emitCaseRecordTest(*field.nested, fieldAddress, nextLabel, failureLabel);
    } else if (field.nested->kind ==
               sema::BoundCasePatternKind::TaggedUnionVariant) {
      auto tagType = std::make_shared<PrimitiveType>(TypeKind::Int32);
      auto tagAddress = createRegister(std::make_shared<PointerType>(tagType));
      currentBlock_->addInstruction(
          std::make_unique<GetElementPtrInst>(tagAddress, fieldAddress, 0));
      auto tag = createRegister(tagType);
      currentBlock_->addInstruction(
          std::make_unique<LoadInst>(tag, tagAddress));
      auto expectedTag = std::make_shared<Constant>(
          std::to_string(field.nested->variantTag), tagType);
      auto tagMatches =
          createRegister(std::make_shared<PrimitiveType>(TypeKind::Bool));
      currentBlock_->addInstruction(
          std::make_unique<CmpInst>("eq", tagMatches, tag, expectedTag));
      if (!field.nested->payloadValue && !field.nested->payloadPattern) {
        currentBlock_->addInstruction(std::make_unique<CondBranchInst>(
            tagMatches, nextLabel, failureLabel));
      } else {
        const auto payloadLabel = createBlockLabel("case.payload");
        currentBlock_->addInstruction(std::make_unique<CondBranchInst>(
            tagMatches, payloadLabel, failureLabel));
        auto payloadBlock = std::make_unique<BasicBlock>(payloadLabel);
        auto *payloadBlockPtr = payloadBlock.get();
        currentFunction_->addBlock(std::move(payloadBlock));
        currentBlock_ = payloadBlockPtr;
        auto payloadAddress = createRegister(
            std::make_shared<PointerType>(field.nested->payloadType));
        currentBlock_->addInstruction(std::make_unique<GetElementPtrInst>(
            payloadAddress, fieldAddress, 1));
        if (field.nested->payloadPattern) {
          emitCaseRecordTest(*field.nested->payloadPattern, payloadAddress,
                             nextLabel, failureLabel);
        } else {
          auto payload = createRegister(field.nested->payloadType);
          currentBlock_->addInstruction(
              std::make_unique<LoadInst>(payload, payloadAddress));
          if (field.nested->payloadValue->type->getIntrinsicKind() ==
                  IntrinsicTypeKind::StringView &&
              payload->getType()->getIntrinsicKind() ==
                  IntrinsicTypeKind::String) {
            auto payloadView = createRegister(field.nested->payloadValue->type,
                                              ValueOwnership::Borrowed);
            currentBlock_->addInstruction(
                std::make_unique<BorrowInst>(payloadView, payload));
            payload = std::move(payloadView);
          }
          field.nested->payloadValue->accept(*this);
          auto expectedPayload = valueStack_.top();
          valueStack_.pop();
          auto payloadMatches =
              createRegister(std::make_shared<PrimitiveType>(TypeKind::Bool));
          currentBlock_->addInstruction(std::make_unique<CmpInst>(
              "eq", payloadMatches, payload, expectedPayload));
          currentBlock_->addInstruction(std::make_unique<CondBranchInst>(
              payloadMatches, nextLabel, failureLabel));
        }
      }
    } else {
      auto fieldValue = createRegister(fieldType);
      auto fieldMatches =
          createRegister(std::make_shared<PrimitiveType>(TypeKind::Bool));
      currentBlock_->addInstruction(
          std::make_unique<LoadInst>(fieldValue, fieldAddress));
      if (field.nested->kind == sema::BoundCasePatternKind::EnumVariant) {
        auto expected = std::make_shared<Constant>(
            std::to_string(field.nested->variantTag), fieldType);
        currentBlock_->addInstruction(std::make_unique<CmpInst>(
            "eq", fieldMatches, fieldValue, expected));
      } else {
        field.nested->value->accept(*this);
        auto expected = valueStack_.top();
        valueStack_.pop();
        currentBlock_->addInstruction(std::make_unique<CmpInst>(
            "eq", fieldMatches, fieldValue, expected));
      }
      currentBlock_->addInstruction(std::make_unique<CondBranchInst>(
          fieldMatches, nextLabel, failureLabel));
    }
    if (fieldIndex + 1 < constrainedFields.size()) {
      auto nextBlock = std::make_unique<BasicBlock>(nextLabel);
      auto *nextBlockPtr = nextBlock.get();
      currentFunction_->addBlock(std::move(nextBlock));
      currentBlock_ = nextBlockPtr;
    }
  }
  if (constrainedFields.empty()) {
    currentBlock_->addInstruction(std::make_unique<BranchInst>(successLabel));
  }
}

void BoundIRGenerator::materializeCaseRecordBindings(
    const sema::BoundCasePattern &pattern,
    const std::shared_ptr<Value> &address) {
  for (const auto &field : pattern.recordFields) {
    const auto fieldType = pattern.recordType->getFields()[field.index].type;
    auto fieldAddress =
        createRegister(std::make_shared<PointerType>(fieldType));
    currentBlock_->addInstruction(std::make_unique<GetElementPtrInst>(
        fieldAddress, address, field.index));
    if (field.nested &&
        field.nested->kind == sema::BoundCasePatternKind::Record) {
      materializeCaseRecordBindings(*field.nested, fieldAddress);
      continue;
    }
    if (field.nested &&
        field.nested->kind == sema::BoundCasePatternKind::TaggedUnionVariant &&
        field.nested->payloadBinding) {
      const auto &payloadPattern = *field.nested;
      auto payloadAddress = createRegister(
          std::make_shared<PointerType>(payloadPattern.payloadType));
      currentBlock_->addInstruction(
          std::make_unique<GetElementPtrInst>(payloadAddress, fieldAddress, 1));
      auto payload = createRegister(payloadPattern.payloadType);
      currentBlock_->addInstruction(
          std::make_unique<LoadInst>(payload, payloadAddress));
      auto bindingAddress = createRegister(
          std::make_shared<PointerType>(payloadPattern.payloadBinding->type));
      currentBlock_->addInstruction(std::make_unique<AllocaInst>(
          bindingAddress, payloadPattern.payloadBinding->type));
      emitInitializationStore(std::move(payload), bindingAddress);
      symbolMap_[payloadPattern.payloadBinding] = bindingAddress;
    }
    if (field.nested &&
        field.nested->kind == sema::BoundCasePatternKind::TaggedUnionVariant &&
        field.nested->payloadPattern) {
      const auto &payloadPattern = *field.nested;
      auto payloadAddress = createRegister(
          std::make_shared<PointerType>(payloadPattern.payloadType));
      currentBlock_->addInstruction(
          std::make_unique<GetElementPtrInst>(payloadAddress, fieldAddress, 1));
      materializeCaseRecordBindings(*payloadPattern.payloadPattern,
                                    payloadAddress);
    }
    if (!field.binding) {
      continue;
    }
    auto value = createRegister(field.binding->type);
    currentBlock_->addInstruction(
        std::make_unique<LoadInst>(value, fieldAddress));
    auto bindingAddress =
        createRegister(std::make_shared<PointerType>(field.binding->type));
    currentBlock_->addInstruction(
        std::make_unique<AllocaInst>(bindingAddress, field.binding->type));
    emitInitializationStore(std::move(value), bindingAddress);
    symbolMap_[field.binding] = bindingAddress;
  }
}

void BoundIRGenerator::materializeCasePayloadBinding(
    const sema::BoundCaseArm &arm,
    const std::shared_ptr<Value> &taggedUnionAddress) {
  if (!arm.payloadBinding) {
    return;
  }

  const auto &pattern = arm.patterns.front();
  auto payloadAddress =
      createRegister(std::make_shared<PointerType>(pattern.payloadType));
  currentBlock_->addInstruction(std::make_unique<GetElementPtrInst>(
      payloadAddress, taggedUnionAddress, 1));
  auto payload = createRegister(pattern.payloadType);
  currentBlock_->addInstruction(
      std::make_unique<LoadInst>(payload, payloadAddress));

  auto bindingAddress =
      createRegister(std::make_shared<PointerType>(arm.payloadBinding->type));
  currentBlock_->addInstruction(
      std::make_unique<AllocaInst>(bindingAddress, arm.payloadBinding->type));
  emitInitializationStore(std::move(payload), bindingAddress);
  symbolMap_[arm.payloadBinding] = bindingAddress;
}

void BoundIRGenerator::visit(sema::BoundCaseStatement &node) {
  node.scrutinee->accept(*this);
  auto scrutinee = valueStack_.top();
  valueStack_.pop();

  const bool isTaggedUnion =
      node.scrutinee->type->getKind() == TypeKind::TaggedUnion;
  const bool isRecord = node.scrutinee->type->getKind() == TypeKind::Record &&
                        !isIntrinsicStringType(node.scrutinee->type);
  std::shared_ptr<Value> taggedUnionAddress;
  std::shared_ptr<Value> recordAddress;
  if (isTaggedUnion) {
    taggedUnionAddress =
        createRegister(std::make_shared<PointerType>(node.scrutinee->type));
    currentBlock_->addInstruction(
        std::make_unique<AllocaInst>(taggedUnionAddress, node.scrutinee->type));
    emitInitializationStore(std::move(scrutinee), taggedUnionAddress);
  } else if (isRecord) {
    recordAddress =
        createRegister(std::make_shared<PointerType>(node.scrutinee->type));
    currentBlock_->addInstruction(
        std::make_unique<AllocaInst>(recordAddress, node.scrutinee->type));
    emitInitializationStore(std::move(scrutinee), recordAddress);
  }

  std::vector<std::string> bodyLabels;
  bodyLabels.reserve(node.arms.size());
  std::vector<std::string> testLabels;
  for (const auto &arm : node.arms) {
    bodyLabels.push_back(createBlockLabel("case.arm"));
    for (size_t i = 0; i < arm.patterns.size(); ++i) {
      testLabels.push_back(createBlockLabel("case.test"));
    }
  }
  const auto mergeLabel = createBlockLabel("case.merge");

  std::string fallbackLabel;
  size_t testCount = 0;
  for (size_t armIndex = 0; armIndex < node.arms.size(); ++armIndex) {
    const auto &arm = node.arms[armIndex];
    if (arm.isElse) {
      fallbackLabel = bodyLabels[armIndex];
      break;
    }
    testCount += arm.patterns.size();
  }

  if (fallbackLabel.empty()) {
    fallbackLabel = mergeLabel;
  }

  if (testCount == 0) {
    currentBlock_->addInstruction(std::make_unique<BranchInst>(fallbackLabel));
  } else {
    currentBlock_->addInstruction(std::make_unique<BranchInst>(testLabels[0]));
  }

  size_t nextTest = 0;
  for (size_t armIndex = 0; armIndex < node.arms.size(); ++armIndex) {
    const auto &arm = node.arms[armIndex];
    if (arm.isElse) {
      continue;
    }

    for (const auto &pattern : arm.patterns) {
      auto testBlock = std::make_unique<BasicBlock>(testLabels[nextTest]);
      auto *testBlockPtr = testBlock.get();
      currentFunction_->addBlock(std::move(testBlock));
      currentBlock_ = testBlockPtr;

      auto comparison =
          createRegister(std::make_shared<PrimitiveType>(TypeKind::Bool));
      bool recordEmitsBranch = false;
      if (pattern.kind == sema::BoundCasePatternKind::Literal) {
        pattern.value->accept(*this);
        auto patternValue = valueStack_.top();
        valueStack_.pop();
        currentBlock_->addInstruction(std::make_unique<CmpInst>(
            "eq", comparison, scrutinee, patternValue));
      } else if (pattern.kind == sema::BoundCasePatternKind::EnumVariant) {
        auto patternValue = std::make_shared<Constant>(
            std::to_string(pattern.variantTag), node.scrutinee->type);
        currentBlock_->addInstruction(std::make_unique<CmpInst>(
            "eq", comparison, scrutinee, patternValue));
      } else if (pattern.kind ==
                 sema::BoundCasePatternKind::TaggedUnionVariant) {
        auto tagType = std::make_shared<PrimitiveType>(TypeKind::Int32);
        auto tagAddress =
            createRegister(std::make_shared<PointerType>(tagType));
        currentBlock_->addInstruction(std::make_unique<GetElementPtrInst>(
            tagAddress, taggedUnionAddress, 0));
        auto tag = createRegister(tagType);
        currentBlock_->addInstruction(
            std::make_unique<LoadInst>(tag, tagAddress));
        auto patternTag = std::make_shared<Constant>(
            std::to_string(pattern.variantTag), tagType);
        currentBlock_->addInstruction(
            std::make_unique<CmpInst>("eq", comparison, tag, patternTag));
        if (pattern.payloadValue || pattern.payloadPattern) {
          const auto payloadTestLabel = createBlockLabel("case.payload");
          const auto followingTest = nextTest + 1;
          const auto tagMismatchLabel = followingTest < testCount
                                            ? testLabels[followingTest]
                                            : fallbackLabel;
          currentBlock_->addInstruction(std::make_unique<CondBranchInst>(
              comparison, payloadTestLabel, tagMismatchLabel));

          auto payloadTestBlock =
              std::make_unique<BasicBlock>(payloadTestLabel);
          auto *payloadTestBlockPtr = payloadTestBlock.get();
          currentFunction_->addBlock(std::move(payloadTestBlock));
          currentBlock_ = payloadTestBlockPtr;

          auto payloadAddress = createRegister(
              std::make_shared<PointerType>(pattern.payloadType));
          currentBlock_->addInstruction(std::make_unique<GetElementPtrInst>(
              payloadAddress, taggedUnionAddress, 1));
          if (pattern.payloadPattern) {
            emitCaseRecordTest(*pattern.payloadPattern, payloadAddress,
                               bodyLabels[armIndex], tagMismatchLabel);
            recordEmitsBranch = true;
          } else {
            auto payload = createRegister(pattern.payloadType);
            currentBlock_->addInstruction(
                std::make_unique<LoadInst>(payload, payloadAddress));
            if (pattern.payloadValue->type->getIntrinsicKind() ==
                    IntrinsicTypeKind::StringView &&
                payload->getType()->getIntrinsicKind() ==
                    IntrinsicTypeKind::String) {
              auto payloadView = createRegister(pattern.payloadValue->type,
                                                ValueOwnership::Borrowed);
              currentBlock_->addInstruction(
                  std::make_unique<BorrowInst>(payloadView, payload));
              payload = std::move(payloadView);
            }
            pattern.payloadValue->accept(*this);
            auto expectedPayload = valueStack_.top();
            valueStack_.pop();
            auto payloadMatches =
                createRegister(std::make_shared<PrimitiveType>(TypeKind::Bool));
            currentBlock_->addInstruction(std::make_unique<CmpInst>(
                "eq", payloadMatches, payload, expectedPayload));
            comparison = std::move(payloadMatches);
          }
        }
      } else {
        const auto followingTest = nextTest + 1;
        const auto failureLabel = followingTest < testCount
                                      ? testLabels[followingTest]
                                      : fallbackLabel;
        emitCaseRecordTest(pattern, recordAddress, bodyLabels[armIndex],
                           failureLabel);
        recordEmitsBranch = true;
      }

      ++nextTest;
      const auto nextLabel =
          nextTest < testCount ? testLabels[nextTest] : fallbackLabel;
      if (!recordEmitsBranch) {
        currentBlock_->addInstruction(std::make_unique<CondBranchInst>(
            comparison, bodyLabels[armIndex], nextLabel));
      }
    }
  }

  for (size_t armIndex = 0; armIndex < node.arms.size(); ++armIndex) {
    auto bodyBlock = std::make_unique<BasicBlock>(bodyLabels[armIndex]);
    auto *bodyBlockPtr = bodyBlock.get();
    currentFunction_->addBlock(std::move(bodyBlock));
    currentBlock_ = bodyBlockPtr;

    const auto &arm = node.arms[armIndex];
    if (!arm.patterns.empty() &&
        arm.patterns.front().kind == sema::BoundCasePatternKind::Record) {
      materializeCaseRecordBindings(arm.patterns.front(), recordAddress);
    }
    if (!arm.patterns.empty() &&
        arm.patterns.front().kind ==
            sema::BoundCasePatternKind::TaggedUnionVariant &&
        arm.patterns.front().payloadPattern) {
      const auto &pattern = arm.patterns.front();
      auto payloadAddress =
          createRegister(std::make_shared<PointerType>(pattern.payloadType));
      currentBlock_->addInstruction(std::make_unique<GetElementPtrInst>(
          payloadAddress, taggedUnionAddress, 1));
      materializeCaseRecordBindings(*pattern.payloadPattern, payloadAddress);
    }
    materializeCasePayloadBinding(arm, taggedUnionAddress);
    if (arm.body) {
      arm.body->accept(*this);
    }
    if (arm.payloadBinding) {
      symbolMap_.erase(arm.payloadBinding);
    }
    for (const auto &binding : arm.recordBindings) {
      symbolMap_.erase(binding);
    }
    if (!isTerminated(currentBlock_)) {
      currentBlock_->addInstruction(std::make_unique<BranchInst>(mergeLabel));
    }
  }

  auto mergeBlock = std::make_unique<BasicBlock>(mergeLabel);
  auto *mergeBlockPtr = mergeBlock.get();
  currentFunction_->addBlock(std::move(mergeBlock));
  currentBlock_ = mergeBlockPtr;
}

void BoundIRGenerator::visit(sema::BoundWhileStatement &node) {
  auto condLabel = createBlockLabel("while.cond");
  auto bodyLabel = createBlockLabel("while.body");
  auto endLabel = createBlockLabel("while.end");

  currentBlock_->addInstruction(std::make_unique<BranchInst>(condLabel));

  auto condBlock = std::make_unique<BasicBlock>(condLabel);
  auto *condBlockPtr = condBlock.get();
  currentFunction_->addBlock(std::move(condBlock));
  currentBlock_ = condBlockPtr;

  node.condition->accept(*this);
  auto cond = valueStack_.top();
  valueStack_.pop();
  currentBlock_->addInstruction(
      std::make_unique<CondBranchInst>(cond, bodyLabel, endLabel));

  auto bodyBlock = std::make_unique<BasicBlock>(bodyLabel);
  auto *bodyBlockPtr = bodyBlock.get();
  currentFunction_->addBlock(std::move(bodyBlock));
  currentBlock_ = bodyBlockPtr;
  loopLabelStack_.push_back({condLabel, endLabel});
  node.body->accept(*this);
  loopLabelStack_.pop_back();
  if (!isTerminated(currentBlock_)) {
    currentBlock_->addInstruction(std::make_unique<BranchInst>(condLabel));
  }

  auto endBlock = std::make_unique<BasicBlock>(endLabel);
  auto *endBlockPtr = endBlock.get();
  currentFunction_->addBlock(std::move(endBlock));
  currentBlock_ = endBlockPtr;
}

void BoundIRGenerator::visit(sema::BoundForStatement &node) {
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

  auto condLabel = createBlockLabel("for.cond");
  auto bodyLabel = createBlockLabel("for.body");
  auto stepLabel = createBlockLabel("for.step");
  auto endLabel = createBlockLabel("for.end");

  currentBlock_->addInstruction(std::make_unique<BranchInst>(condLabel));

  auto condBlock = std::make_unique<BasicBlock>(condLabel);
  auto *condBlockPtr = condBlock.get();
  currentFunction_->addBlock(std::move(condBlock));
  currentBlock_ = condBlockPtr;

  node.condition->accept(*this);
  auto cond = valueStack_.top();
  valueStack_.pop();
  currentBlock_->addInstruction(
      std::make_unique<CondBranchInst>(cond, bodyLabel, endLabel));

  auto bodyBlock = std::make_unique<BasicBlock>(bodyLabel);
  auto *bodyBlockPtr = bodyBlock.get();
  currentFunction_->addBlock(std::move(bodyBlock));
  currentBlock_ = bodyBlockPtr;
  loopLabelStack_.push_back({stepLabel, endLabel});
  if (node.body) {
    node.body->accept(*this);
  }
  loopLabelStack_.pop_back();
  if (!isTerminated(currentBlock_)) {
    currentBlock_->addInstruction(std::make_unique<BranchInst>(stepLabel));
  }

  auto stepBlock = std::make_unique<BasicBlock>(stepLabel);
  auto *stepBlockPtr = stepBlock.get();
  currentFunction_->addBlock(std::move(stepBlock));
  currentBlock_ = stepBlockPtr;
  if (node.increment) {
    node.increment->accept(*this);
  }
  if (!isTerminated(currentBlock_)) {
    currentBlock_->addInstruction(std::make_unique<BranchInst>(condLabel));
  }

  auto endBlock = std::make_unique<BasicBlock>(endLabel);
  auto *endBlockPtr = endBlock.get();
  currentFunction_->addBlock(std::move(endBlock));
  currentBlock_ = endBlockPtr;
}

void BoundIRGenerator::visit(sema::BoundAsmStatement &node) {
  std::vector<AsmOperand> outputs;
  for (auto &operand : node.outputs) {
    bool oldEvaluateAsAddress = evaluateAsAddress_;
    evaluateAsAddress_ = true;
    operand.expr->accept(*this);
    evaluateAsAddress_ = oldEvaluateAsAddress;
    auto addr = valueStack_.top();
    valueStack_.pop();
    outputs.push_back({operand.constraint, addr, operand.expr->type});
  }

  std::vector<AsmOperand> inputs;
  for (auto &operand : node.inputs) {
    operand.expr->accept(*this);
    auto value = valueStack_.top();
    valueStack_.pop();
    inputs.push_back({operand.constraint, value, operand.expr->type});
  }

  currentBlock_->addInstruction(std::make_unique<InlineAsmInst>(
      node.assembly, std::move(outputs), std::move(inputs), node.clobbers));
}

void BoundIRGenerator::visit(sema::BoundBreakStatement &node) {
  if (loopLabelStack_.empty()) {
    // Should have been diagnosed earlier in binder, but guard anyway
    return;
  }
  auto endLabel = loopLabelStack_.back().second;
  currentBlock_->addInstruction(std::make_unique<BranchInst>(endLabel));
}

void BoundIRGenerator::visit(sema::BoundContinueStatement &node) {
  if (loopLabelStack_.empty()) {
    return;
  }
  auto condLabel = loopLabelStack_.back().first;
  currentBlock_->addInstruction(std::make_unique<BranchInst>(condLabel));
}

void BoundIRGenerator::visit(sema::BoundWeakLockExpression &node) {
  node.weakExpression->accept(*this);
  auto weakValue = valueStack_.top();
  valueStack_.pop();
  auto result = createRegister(node.type, ValueOwnership::OwnedStrong);
  currentBlock_->addInstruction(
      std::make_unique<WeakLockInst>(result, weakValue));
  valueStack_.push(result);
}

void BoundIRGenerator::visit(sema::BoundWeakAliveExpression &node) {
  node.weakExpression->accept(*this);
  auto weakValue = valueStack_.top();
  valueStack_.pop();
  auto result = createRegister(node.type);
  currentBlock_->addInstruction(
      std::make_unique<WeakAliveInst>(result, weakValue));
  valueStack_.push(result);
}

void BoundIRGenerator::visit(sema::BoundIndexAccess &node) {
  if (node.left->type->getKind() == zir::TypeKind::Record) {
    auto recordType =
        std::static_pointer_cast<zir::RecordType>(node.left->type);
    if (recordType->getRole() == RecordRole::VariadicView) {
      bool oldEvaluateAsAddress = evaluateAsAddress_;
      evaluateAsAddress_ = true;
      node.left->accept(*this);
      evaluateAsAddress_ = oldEvaluateAsAddress;
      auto sliceAddr = valueStack_.top();
      valueStack_.pop();

      bool oldIndexEvaluateAsAddress = evaluateAsAddress_;
      evaluateAsAddress_ = false;
      node.index->accept(*this);
      evaluateAsAddress_ = oldIndexEvaluateAsAddress;
      auto indexValue = valueStack_.top();
      valueStack_.pop();

      auto elemType = std::static_pointer_cast<zir::PointerType>(
                          recordType->getFields()[0].type)
                          ->getBaseType();
      auto dataAddr = createRegister(
          std::make_shared<PointerType>(recordType->getFields()[0].type));
      currentBlock_->addInstruction(
          std::make_unique<GetElementPtrInst>(dataAddr, sliceAddr, 0));

      auto dataPtr = createRegister(recordType->getFields()[0].type);
      currentBlock_->addInstruction(
          std::make_unique<LoadInst>(dataPtr, dataAddr));

      auto elemAddr = createRegister(std::make_shared<PointerType>(elemType));
      currentBlock_->addInstruction(std::make_unique<BinaryInst>(
          OpCode::Add, elemAddr, dataPtr, indexValue));

      if (evaluateAsAddress_) {
        valueStack_.push(elemAddr);
      } else {
        auto res = createRegister(node.type);
        currentBlock_->addInstruction(
            std::make_unique<LoadInst>(res, elemAddr));
        valueStack_.push(res);
      }
      return;
    }
  }

  bool oldEvaluateAsAddress = evaluateAsAddress_;
  evaluateAsAddress_ = true;
  node.left->accept(*this);
  auto left = valueStack_.top();
  valueStack_.pop();

  evaluateAsAddress_ = false;
  node.index->accept(*this);
  auto indexVal = valueStack_.top();
  valueStack_.pop();
  evaluateAsAddress_ = oldEvaluateAsAddress;

  auto ptr = createRegister(std::make_shared<PointerType>(node.type));
  if (auto *c = dynamic_cast<Constant *>(indexVal.get())) {
    int idx = 0;
    try {
      idx = std::stoi(c->getName());
    } catch (...) {
    }
    currentBlock_->addInstruction(
        std::make_unique<GetElementPtrInst>(ptr, left, idx));
  } else {
    currentBlock_->addInstruction(
        std::make_unique<GetElementPtrInst>(ptr, left, indexVal));
  }

  if (evaluateAsAddress_) {
    valueStack_.push(ptr);
  } else {
    auto res = createRegister(node.type);
    currentBlock_->addInstruction(std::make_unique<LoadInst>(res, ptr));
    valueStack_.push(res);
  }
}

void BoundIRGenerator::visit(sema::BoundCast &node) {
  auto isArrayViewType = [](const std::shared_ptr<Type> &type) {
    return type && type->getKind() == TypeKind::Record &&
           std::static_pointer_cast<RecordType>(type)->getRole() ==
               RecordRole::VariadicView;
  };

  if (node.expression->type &&
      node.expression->type->getKind() == TypeKind::Array &&
      isArrayViewType(node.type)) {
    auto arrayType = std::static_pointer_cast<ArrayType>(node.expression->type);
    auto viewType = std::static_pointer_cast<RecordType>(node.type);

    bool oldEvaluateAsAddress = evaluateAsAddress_;
    evaluateAsAddress_ = true;
    node.expression->accept(*this);
    evaluateAsAddress_ = oldEvaluateAsAddress;

    auto arrayAddr = valueStack_.top();
    valueStack_.pop();

    auto data =
        createRegister(std::make_shared<PointerType>(arrayType->getBaseType()));
    currentBlock_->addInstruction(
        std::make_unique<GetElementPtrInst>(data, arrayAddr, 0));

    auto viewAddr = createRegister(std::make_shared<PointerType>(node.type));
    currentBlock_->addInstruction(
        std::make_unique<AllocaInst>(viewAddr, node.type));

    auto dataFieldAddr = createRegister(
        std::make_shared<PointerType>(viewType->getFields()[0].type));
    currentBlock_->addInstruction(
        std::make_unique<GetElementPtrInst>(dataFieldAddr, viewAddr, 0));
    currentBlock_->addInstruction(
        std::make_unique<StoreInst>(data, dataFieldAddr, StoreMode::Assign));

    auto lenType = viewType->getFields()[1].type;
    auto lenFieldAddr = createRegister(std::make_shared<PointerType>(lenType));
    currentBlock_->addInstruction(
        std::make_unique<GetElementPtrInst>(lenFieldAddr, viewAddr, 1));
    currentBlock_->addInstruction(std::make_unique<StoreInst>(
        std::make_shared<Constant>(std::to_string(arrayType->getSize()),
                                   lenType),
        lenFieldAddr, StoreMode::Assign));

    auto result = createRegister(node.type);
    currentBlock_->addInstruction(std::make_unique<LoadInst>(result, viewAddr));
    valueStack_.push(result);
    return;
  }

  node.expression->accept(*this);
  auto src = valueStack_.top();
  valueStack_.pop();

  // Global initializers are lowered as constants only. Fold casted constants
  // immediately to the target type so LLVM sees a correctly-typed initializer.
  if (!currentFunction_ || !currentBlock_) {
    if (auto c = std::dynamic_pointer_cast<Constant>(src)) {
      auto folded = c->getLiteral();
      auto srcTy = c->getType();
      auto dstTy = node.type;

      if (srcTy && dstTy && srcTy->isInteger() && dstTy->isInteger()) {
        try {
          if (dstTy->isUnsigned()) {
            auto v = static_cast<uint64_t>(std::stoull(folded, nullptr, 0));
            folded = std::to_string(v);
          } else {
            auto v = static_cast<int64_t>(std::stoll(folded, nullptr, 0));
            folded = std::to_string(v);
          }
        } catch (...) {
          // Keep original literal when parsing fails.
        }
      } else if (srcTy && dstTy && srcTy->isFloatingPoint() &&
                 dstTy->isFloatingPoint()) {
        try {
          auto v = std::stod(folded);
          folded = std::to_string(v);
        } catch (...) {
          // Keep original literal when parsing fails.
        }
      } else if (srcTy && dstTy && srcTy->isInteger() &&
                 dstTy->isFloatingPoint()) {
        try {
          if (srcTy->isUnsigned()) {
            auto v = static_cast<double>(std::stoull(folded, nullptr, 0));
            folded = std::to_string(v);
          } else {
            auto v = static_cast<double>(std::stoll(folded, nullptr, 0));
            folded = std::to_string(v);
          }
        } catch (...) {
          // Keep original literal when parsing fails.
        }
      } else if (srcTy && dstTy && srcTy->isFloatingPoint() &&
                 dstTy->isInteger()) {
        try {
          double v = std::stod(folded);
          if (dstTy->isUnsigned()) {
            auto out = static_cast<uint64_t>(v);
            folded = std::to_string(out);
          } else {
            auto out = static_cast<int64_t>(v);
            folded = std::to_string(out);
          }
        } catch (...) {
          // Keep original literal when parsing fails.
        }
      }

      valueStack_.push(std::make_shared<Constant>(folded, node.type));
      return;
    }

    valueStack_.push(src);
    return;
  }

  if (node.type->getIntrinsicKind() == IntrinsicTypeKind::StringView &&
      src->getType()->getIntrinsicKind() == IntrinsicTypeKind::String) {
    auto result = createRegister(node.type, ValueOwnership::Borrowed);
    currentBlock_->addInstruction(std::make_unique<BorrowInst>(result, src));
    valueStack_.push(result);
    return;
  }
  auto res = createRegister(node.type, ownershipForCast(src, node.type));
  currentBlock_->addInstruction(
      std::make_unique<CastInst>(res, src, node.type));
  valueStack_.push(res);
}

void BoundIRGenerator::visit(sema::BoundRangeExpression &node) {
  node.start->accept(*this);
}

void BoundIRGenerator::visit(sema::BoundNewExpression &node) {
  auto result = createRegister(node.type, ValueOwnership::OwnedStrong);
  currentBlock_->addInstruction(
      std::make_unique<AllocInst>(result, node.classType));

  if (node.constructor) {
    std::vector<std::shared_ptr<Value>> args;
    args.push_back(result);
    std::vector<bool> argumentIsRef{false};
    for (size_t i = 0; i < node.arguments.size(); ++i) {
      bool oldEvaluateAsAddress = evaluateAsAddress_;
      if (i < node.argumentIsRef.size() && node.argumentIsRef[i]) {
        evaluateAsAddress_ = true;
      }
      node.arguments[i]->accept(*this);
      evaluateAsAddress_ = oldEvaluateAsAddress;
      auto argument = valueStack_.top();
      valueStack_.pop();
      const size_t parameterIndex = i + 1;
      const auto parameterOwnership =
          parameterOwnershipFor(*node.constructor, parameterIndex);
      prepareCallArgument(argument, parameterOwnership);
      args.push_back(std::move(argument));
      argumentIsRef.push_back(i < node.argumentIsRef.size() &&
                              node.argumentIsRef[i]);
    }

    currentBlock_->addInstruction(std::make_unique<CallInst>(
        nullptr, node.constructor->linkName, std::move(args),
        std::move(argumentIsRef), nullptr));
  }

  valueStack_.push(result);
}

} // namespace zir
