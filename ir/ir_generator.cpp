#include "ir_generator.hpp"
#include <iostream>

namespace zir {

std::unique_ptr<Module> BoundIRGenerator::generate(sema::BoundRootNode &root) {
  module_ = std::make_unique<Module>("zap_module");
  root.accept(*this);
  return std::move(module_);
}

void BoundIRGenerator::visit(sema::BoundRootNode &node) {
  for (const auto &func : node.functions) {
    func->accept(*this);
  }
}

void BoundIRGenerator::visit(sema::BoundFunctionDeclaration &node) {
  auto symbol = node.symbol;
  auto func =
      std::make_unique<Function>(symbol->name, symbol->returnType->toString());
  currentFunction_ = func.get();

  auto entryBlock = std::make_unique<BasicBlock>("entry");
  currentBlock_ = entryBlock.get();
  currentFunction_->addBlock(std::move(entryBlock));

  for (const auto &paramSymbol : symbol->parameters) {
    auto arg = std::make_shared<Argument>(paramSymbol->name, paramSymbol->type);
    currentFunction_->arguments.push_back(arg);

    // Create an alloca for the parameter to make it mutable (standard LLVM
    // practice)
    auto allocaReg =
        createRegister(std::make_shared<PointerType>(paramSymbol->type));
    currentBlock_->addInstruction(
        std::make_unique<AllocaInst>(allocaReg, paramSymbol->type));
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

void BoundIRGenerator::visit(sema::BoundBlock &node) {
  for (const auto &stmt : node.statements) {
    stmt->accept(*this);
  }
}

void BoundIRGenerator::visit(sema::BoundVariableDeclaration &node) {
  auto type = node.symbol->type;
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
  auto reg = symbolMap_[node.symbol];
  node.expression->accept(*this);
  auto val = valueStack_.top();
  valueStack_.pop();
  currentBlock_->addInstruction(std::make_unique<StoreInst>(val, reg));
}

void BoundIRGenerator::visit(sema::BoundLiteral &node) {
  valueStack_.push(std::make_shared<Constant>(node.value, node.type));
}

void BoundIRGenerator::visit(sema::BoundVariableExpression &node) {
  auto it = symbolMap_.find(node.symbol);
  if (it == symbolMap_.end()) {
    std::cerr << "Error: Symbol " << node.symbol->name
              << " not found in IR symbol map\n";
    return;
  }
  auto addr = it->second;
  auto reg = createRegister(node.type);
  currentBlock_->addInstruction(std::make_unique<LoadInst>(reg, addr));
  valueStack_.push(reg);
}

void BoundIRGenerator::visit(sema::BoundBinaryExpression &node) {
  node.left->accept(*this);
  auto left = valueStack_.top();
  valueStack_.pop();

  node.right->accept(*this);
  auto right = valueStack_.top();
  valueStack_.pop();

  auto reg = createRegister(node.type);
  OpCode op;
  if (node.op == "+")
    op = OpCode::Add;
  else if (node.op == "-")
    op = OpCode::Sub;
  else if (node.op == "*")
    op = OpCode::Mul;
  else if (node.op == "/")
    op = OpCode::Div;
  else
    op = OpCode::Add;

  currentBlock_->addInstruction(
      std::make_unique<BinaryInst>(op, reg, left, right));
  valueStack_.push(reg);
}

void BoundIRGenerator::visit(sema::BoundFunctionCall &node) {
  std::vector<std::shared_ptr<Value>> args;
  for (const auto &arg : node.arguments) {
    arg->accept(*this);
    args.push_back(valueStack_.top());
    valueStack_.pop();
  }

  auto reg = createRegister(node.type);
  currentBlock_->addInstruction(
      std::make_unique<CallInst>(reg, node.symbol->name, args));
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
  node.expr->accept(*this);
  auto expr = valueStack_.top();
  valueStack_.pop();

  auto reg = createRegister(node.type);
  // Placeholder: just propagate for now
  valueStack_.push(expr);
}

void BoundIRGenerator::visit(sema::BoundArrayLiteral &node) {
  // Placeholder: array literal generation is complex
  valueStack_.push(std::make_shared<Constant>("0", node.type));
}

} // namespace zir
