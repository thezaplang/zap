#include "ir_generator.hpp"
#include <cstdint>
#include <iostream>

namespace zir {

std::unique_ptr<Module> BoundIRGenerator::generate(sema::BoundRootNode &root) {
  module_ = std::make_unique<Module>("zap_module");
  globalSymbolMap_.clear();
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
  for (const auto &global : node.globals) {
    global->accept(*this);
  }
  for (const auto &extFunc : node.externalFunctions) {
    extFunc->accept(*this);
  }
  for (const auto &func : node.functions) {
    func->accept(*this);
  }
}

void BoundIRGenerator::visit(sema::BoundFunctionDeclaration &node) {
  auto symbol = node.symbol;
  auto func = std::make_unique<Function>(symbol->linkName, symbol->returnType,
                                         symbol->ownerTypeName,
                                         symbol->isDestructor,
                                         symbol->vtableSlot,
                                         symbol->isCVariadic);
  currentFunction_ = func.get();

  auto entryBlock = std::make_unique<BasicBlock>("entry");
  currentBlock_ = entryBlock.get();
  currentFunction_->addBlock(std::move(entryBlock));

  for (const auto &paramSymbol : symbol->parameters) {
    auto argType = paramSymbol->is_ref
                       ? std::static_pointer_cast<Type>(
                             std::make_shared<PointerType>(paramSymbol->type))
                       : paramSymbol->type;
    auto arg = std::make_shared<Argument>(
        paramSymbol->name, argType, paramSymbol->is_ref,
        paramSymbol->is_variadic_pack, paramSymbol->variadic_element_type);
    currentFunction_->arguments.push_back(arg);

    auto spillType = paramSymbol->is_ref
                         ? std::make_shared<PointerType>(paramSymbol->type)
                         : paramSymbol->type;
    auto allocaReg = createRegister(std::make_shared<PointerType>(spillType));
    currentBlock_->addInstruction(
        std::make_unique<AllocaInst>(allocaReg, spillType));
    currentBlock_->addInstruction(std::make_unique<StoreInst>(arg, allocaReg));

    symbolMap_[paramSymbol] = allocaReg;
  }

  if (node.body) {
    node.body->accept(*this);
  }

  if (currentBlock_ &&
      (currentBlock_->instructions.empty() ||
       currentBlock_->instructions.back()->getOpCode() != OpCode::Ret)) {
    if (symbol->returnType->getKind() == TypeKind::Void) {
      currentBlock_->addInstruction(std::make_unique<ReturnInst>());
    } else {
      auto dummy = std::make_shared<Constant>("0", symbol->returnType);
      currentBlock_->addInstruction(std::make_unique<ReturnInst>(dummy));
    }
  }

  module_->addFunction(std::move(func));
  currentFunction_ = nullptr;
  currentBlock_ = nullptr;
  symbolMap_.clear();
}

void BoundIRGenerator::visit(sema::BoundExternalFunctionDeclaration &node) {
  auto symbol = node.symbol;
  auto func = std::make_unique<Function>(symbol->linkName, symbol->returnType,
                                         symbol->ownerTypeName,
                                         symbol->isDestructor,
                                         symbol->vtableSlot,
                                         symbol->isCVariadic);

  for (const auto &paramSymbol : symbol->parameters) {
    auto argType = paramSymbol->is_ref
                       ? std::static_pointer_cast<Type>(
                             std::make_shared<PointerType>(paramSymbol->type))
                       : paramSymbol->type;
    auto arg = std::make_shared<Argument>(
        paramSymbol->name, argType, paramSymbol->is_ref,
        paramSymbol->is_variadic_pack, paramSymbol->variadic_element_type);
    func->arguments.push_back(arg);
  }

  module_->addExternalFunction(std::move(func));
}

void BoundIRGenerator::visit(sema::BoundBlock &node) {
  std::vector<std::shared_ptr<sema::VariableSymbol>> blockClassLocals;
  for (const auto &stmt : node.statements) {
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

  if (currentFunction_ && currentBlock_ &&
      (currentBlock_->instructions.empty() ||
       (currentBlock_->instructions.back()->getOpCode() != OpCode::Ret &&
        currentBlock_->instructions.back()->getOpCode() != OpCode::Br &&
        currentBlock_->instructions.back()->getOpCode() != OpCode::CondBr))) {
    for (auto it = blockClassLocals.rbegin(); it != blockClassLocals.rend();
         ++it) {
      auto symbolIt = symbolMap_.find(*it);
      if (symbolIt == symbolMap_.end()) {
        continue;
      }
      currentBlock_->addInstruction(std::make_unique<StoreInst>(
          std::make_shared<Constant>("null", (*it)->type), symbolIt->second));
    }
  }
}

void BoundIRGenerator::visit(sema::BoundVariableDeclaration &node) {
  auto type = node.symbol->type;

  if (!currentFunction_) {
    std::shared_ptr<Value> initializer = nullptr;
    if (node.initializer) {
      node.initializer->accept(*this);
      initializer = valueStack_.top();
      valueStack_.pop();
    }
    auto global = std::make_shared<Global>(node.symbol->name,
                                           node.symbol->linkName, type,
                                           initializer, node.symbol->is_const);
    module_->addGlobal(global);
    globalSymbolMap_[node.symbol] = global;
    return;
  }

  auto reg = createRegister(std::make_shared<PointerType>(type));
  currentBlock_->addInstruction(std::make_unique<AllocaInst>(reg, type));
  symbolMap_[node.symbol] = reg;

  if (node.initializer) {
    node.initializer->accept(*this);
    auto val = valueStack_.top();
    valueStack_.pop();

    currentBlock_->addInstruction(std::make_unique<StoreInst>(val, reg));
  }
}

void BoundIRGenerator::visit(sema::BoundReturnStatement &node) {
  std::shared_ptr<Value> val = nullptr;
  if (node.expression) {
    node.expression->accept(*this);
    val = valueStack_.top();
    valueStack_.pop();
  }
  currentBlock_->addInstruction(std::make_unique<ReturnInst>(val));
}

void BoundIRGenerator::visit(sema::BoundAssignment &node) {
  bool oldEvaluateAsAddress = evaluateAsAddress_;
  evaluateAsAddress_ = true;
  node.target->accept(*this);
  evaluateAsAddress_ = oldEvaluateAsAddress;
  auto target = valueStack_.top();
  valueStack_.pop();

  node.expression->accept(*this);
  auto val = valueStack_.top();
  valueStack_.pop();
  currentBlock_->addInstruction(std::make_unique<StoreInst>(val, target));
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
  auto reg = createRegister(node.type);
  currentBlock_->addInstruction(std::make_unique<LoadInst>(reg, addr));
  valueStack_.push(reg);
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

    auto res = createRegister(node.type);
    std::vector<std::pair<std::string, std::shared_ptr<Value>>> incoming;
    incoming.push_back(
        {leftBlockLabel, std::make_shared<Constant>("false", node.type)});
    incoming.push_back({actualRhsBlockLabel, rightVal});

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

    auto res = createRegister(node.type);
    std::vector<std::pair<std::string, std::shared_ptr<Value>>> incoming;
    incoming.push_back(
        {leftBlockLabel, std::make_shared<Constant>("true", node.type)});
    incoming.push_back({actualRhsBlockLabel, rightVal});

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

  auto reg = createRegister(node.type);
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

  auto res = createRegister(node.type);
  std::vector<std::pair<std::string, std::shared_ptr<Value>>> incoming;
  incoming.push_back({actualThenLabel, thenVal});
  incoming.push_back({actualElseLabel, elseVal});
  currentBlock_->addInstruction(std::make_unique<PhiInst>(res, incoming));
  valueStack_.push(res);
}

void BoundIRGenerator::visit(sema::BoundFunctionCall &node) {
  std::vector<std::shared_ptr<Value>> args;
  for (size_t i = 0; i < node.arguments.size(); ++i) {
    bool oldEvaluateAsAddress = evaluateAsAddress_;
    if (i < node.argumentIsRef.size() && node.argumentIsRef[i]) {
      evaluateAsAddress_ = true;
    }
    node.arguments[i]->accept(*this);
    evaluateAsAddress_ = oldEvaluateAsAddress;
    args.push_back(valueStack_.top());
    valueStack_.pop();
  }

  std::shared_ptr<Value> variadicPack = nullptr;
  if (node.variadicPack) {
    node.variadicPack->accept(*this);
    variadicPack = valueStack_.top();
    valueStack_.pop();
  }

  auto reg = createRegister(node.type);
  currentBlock_->addInstruction(
      std::make_unique<CallInst>(reg, node.symbol->linkName, args,
                                 node.argumentIsRef, variadicPack));
  valueStack_.push(reg);
}

std::shared_ptr<Value>
BoundIRGenerator::createRegister(std::shared_ptr<Type> type) {
  return std::make_shared<Register>(std::to_string(nextRegisterId_++), type);
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
        if (!lit.empty() && lit != "true" && lit != "false" &&
            lit != "null" && lit[0] != '\'' && lit[0] != '\\') {
          try {
            if (c->getType() && c->getType()->isFloatingPoint()) {
              auto v = std::stod(lit);
              valueStack_.push(std::make_shared<Constant>(
                  std::to_string(-v), node.type));
              return;
            }
            if (c->getType() && c->getType()->isInteger()) {
              if (c->getType()->isUnsigned()) {
                auto v = std::stoull(lit);
                auto out = static_cast<int64_t>(-(static_cast<int64_t>(v)));
                valueStack_.push(std::make_shared<Constant>(
                    std::to_string(out), node.type));
              } else {
                auto v = std::stoll(lit);
                valueStack_.push(std::make_shared<Constant>(
                    std::to_string(-v), node.type));
              }
              return;
            }
          } catch (...) {
          }
        }
      }

      if (node.op == "!") {
        if (lit == "true") {
          valueStack_.push(
              std::make_shared<Constant>("false", node.type));
          return;
        }
        if (lit == "false") {
          valueStack_.push(
              std::make_shared<Constant>("true", node.type));
          return;
        }
      }

      if (node.op == "~") {
        if (!lit.empty() && lit != "true" && lit != "false" &&
            lit != "null" && lit[0] != '\'' && lit[0] != '\\') {
          try {
            auto v = std::stoll(lit);
            valueStack_.push(std::make_shared<Constant>(
                std::to_string(~v), node.type));
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

    auto elementAddr = createRegister(
        std::make_shared<PointerType>(arrayType->getBaseType()));
    currentBlock_->addInstruction(std::make_unique<GetElementPtrInst>(
        elementAddr, allocaReg, static_cast<int>(i)));
    currentBlock_->addInstruction(
        std::make_unique<StoreInst>(value, elementAddr));
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

void BoundIRGenerator::visit(sema::BoundMemberAccess &node) {
  auto isStringRecord = [](const std::shared_ptr<zir::Type> &type) {
    return type && type->getKind() == zir::TypeKind::Record &&
           std::static_pointer_cast<zir::RecordType>(type)->getName() ==
               "String";
  };

  if (node.left->type->getKind() == zir::TypeKind::Enum) {
    node.left->accept(*this);
    auto left = std::move(valueStack_.top());
    valueStack_.pop();

    auto enumType = std::static_pointer_cast<zir::EnumType>(left->getType());
    int value = enumType->getVariantIndex(node.member);
    if (value != -1) {
      valueStack_.push(std::make_shared<Constant>(
          std::to_string(value),
          std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int)));
      return;
    }
  }

  bool oldEvaluateAsAddress = evaluateAsAddress_;
  evaluateAsAddress_ =
      !(node.left->type->getKind() == zir::TypeKind::Class ||
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
    auto baseType =
        std::static_pointer_cast<zir::PointerType>(left->getType())->getBaseType();
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
    } else if (baseType->getKind() == zir::TypeKind::Record &&
               !isStringRecord(baseType)) {
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
  } else if (left->getType()->getKind() == zir::TypeKind::Record &&
             !isStringRecord(left->getType())) {
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

  throw std::runtime_error("Member '" + node.member + "' not found in type '" +
                           left->getTypeName() + "'");
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

    valueStack_.push(
        std::make_shared<AggregateConstant>(node.type, std::move(aggregateFields)));
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

    auto fieldAddr =
        createRegister(std::make_shared<PointerType>(fields[fieldIndex].type));
    currentBlock_->addInstruction(
        std::make_unique<GetElementPtrInst>(fieldAddr, allocaReg, fieldIndex));
    currentBlock_->addInstruction(std::make_unique<StoreInst>(val, fieldAddr));
  }

  auto result = createRegister(recordType);
  currentBlock_->addInstruction(std::make_unique<LoadInst>(result, allocaReg));
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

  if (node.thenBody)
    node.thenBody->accept(*this);

  std::string actualThenLabel = currentBlock_->label;

  if (currentBlock_->instructions.empty() ||
      currentBlock_->instructions.back()->getOpCode() != OpCode::Ret) {
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

    if (currentBlock_->instructions.empty() ||
        currentBlock_->instructions.back()->getOpCode() != OpCode::Ret) {
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
  if (currentBlock_->instructions.empty() ||
      currentBlock_->instructions.back()->getOpCode() != OpCode::Ret) {
    currentBlock_->addInstruction(std::make_unique<BranchInst>(condLabel));
  }

  auto endBlock = std::make_unique<BasicBlock>(endLabel);
  auto *endBlockPtr = endBlock.get();
  currentFunction_->addBlock(std::move(endBlock));
  currentBlock_ = endBlockPtr;
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
  auto result = createRegister(node.type);
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
    if (recordType->getName() == "String") {
      if (evaluateAsAddress_) {
        throw std::runtime_error("String index access is not assignable.");
      }

      node.left->accept(*this);
      auto stringValue = valueStack_.top();
      valueStack_.pop();

      node.index->accept(*this);
      auto indexValue = valueStack_.top();
      valueStack_.pop();

      auto result = createRegister(node.type);
      currentBlock_->addInstruction(std::make_unique<CallInst>(
          result, "at",
          std::vector<std::shared_ptr<Value>>{stringValue, indexValue}));
      valueStack_.push(result);
      return;
    }

    if (recordType->getName().rfind("__zap_varargs_", 0) == 0) {
      bool oldEvaluateAsAddress = evaluateAsAddress_;
      evaluateAsAddress_ = true;
      node.left->accept(*this);
      evaluateAsAddress_ = oldEvaluateAsAddress;
      auto sliceAddr = valueStack_.top();
      valueStack_.pop();

      node.index->accept(*this);
      auto indexValue = valueStack_.top();
      valueStack_.pop();

      auto elemType =
          std::static_pointer_cast<zir::PointerType>(recordType->getFields()[0].type)
              ->getBaseType();
      auto dataAddr = createRegister(recordType->getFields()[0].type);
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
  evaluateAsAddress_ = oldEvaluateAsAddress;
  auto left = valueStack_.top();
  valueStack_.pop();

  node.index->accept(*this);
  auto indexVal = valueStack_.top();
  valueStack_.pop();

  int idx = 0;
  if (auto *c = dynamic_cast<Constant *>(indexVal.get())) {
    try {
      idx = std::stoi(c->getName());
    } catch (...) {
    }
  }

  auto ptr = createRegister(std::make_shared<PointerType>(node.type));
  currentBlock_->addInstruction(
      std::make_unique<GetElementPtrInst>(ptr, left, idx));

  if (evaluateAsAddress_) {
    valueStack_.push(ptr);
  } else {
    auto res = createRegister(node.type);
    currentBlock_->addInstruction(std::make_unique<LoadInst>(res, ptr));
    valueStack_.push(res);
  }
}

void BoundIRGenerator::visit(sema::BoundCast &node) {
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
            auto v = static_cast<uint64_t>(std::stoull(folded));
            folded = std::to_string(v);
          } else {
            auto v = static_cast<int64_t>(std::stoll(folded));
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
            auto v = static_cast<double>(std::stoull(folded));
            folded = std::to_string(v);
          } else {
            auto v = static_cast<double>(std::stoll(folded));
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

  auto res = createRegister(node.type);
  currentBlock_->addInstruction(
      std::make_unique<CastInst>(res, src, node.type));
  valueStack_.push(res);
}

void BoundIRGenerator::visit(sema::BoundNewExpression &node) {
  auto result = createRegister(node.type);
  currentBlock_->addInstruction(
      std::make_unique<AllocInst>(result, node.classType));

  if (node.constructor) {
    std::vector<std::shared_ptr<Value>> args;
    args.push_back(result);
    for (size_t i = 0; i < node.arguments.size(); ++i) {
      bool oldEvaluateAsAddress = evaluateAsAddress_;
      if (i < node.argumentIsRef.size() && node.argumentIsRef[i]) {
        evaluateAsAddress_ = true;
      }
      node.arguments[i]->accept(*this);
      evaluateAsAddress_ = oldEvaluateAsAddress;
      args.push_back(valueStack_.top());
      valueStack_.pop();
    }

    currentBlock_->addInstruction(
        std::make_unique<CallInst>(nullptr, node.constructor->linkName, args));
  }

  valueStack_.push(result);
}

} // namespace zir
