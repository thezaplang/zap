#include "ir_generator.hpp"
#include "../ast/nodes.hpp"
#include <iostream>

namespace zir {

std::unique_ptr<Module> IRGenerator::generate(RootNode& root) {
    module_ = std::make_unique<Module>("zap_module");
    root.accept(*this);
    return std::move(module_);
}

void IRGenerator::visit(RootNode& node) {
    for (const auto& child : node.children) {
        child->accept(*this);
    }
}

void IRGenerator::visit(FunDecl& node) {
    auto retType = node.returnType_ ? mapType(node.returnType_->typeName) : std::make_shared<PrimitiveType>(TypeKind::Void);
    auto func = std::make_unique<Function>(node.name_, retType->toString());
    currentFunction_ = func.get();
    
    auto entryBlock = std::make_unique<BasicBlock>("entry");
    currentBlock_ = entryBlock.get();
    currentFunction_->addBlock(std::move(entryBlock));
    
    pushScope();
    
    for (const auto& param : node.params_) {
        auto paramType = mapType(param->type->typeName);
        auto arg = std::make_shared<Argument>(param->name, paramType);
        currentFunction_->arguments.push_back(arg);
        
        if (paramType->isReferenceType()) {
            currentBlock_->addInstruction(std::make_unique<RetainInst>(arg));
        }
        addSymbol(param->name, arg);
    }
    
    if (node.body_) {
        node.body_->accept(*this);
    }
    
    if (currentBlock_ && (currentBlock_->instructions.empty() || 
        currentBlock_->instructions.back()->getOpCode() != OpCode::Ret)) {
        popScope(); // Ensure release before void return
        if (retType->getKind() == TypeKind::Void) {
            currentBlock_->addInstruction(std::make_unique<ReturnInst>());
        } else {
            auto dummy = std::make_shared<Constant>("0", retType);
            currentBlock_->addInstruction(std::make_unique<ReturnInst>(dummy));
        }
    }
    
    module_->addFunction(std::move(func));
    currentFunction_ = nullptr;
    currentBlock_ = nullptr;
}

void IRGenerator::visit(BodyNode& node) {
    for (const auto& stmt : node.statements) {
        stmt->accept(*this);
    }
    if (node.result) {
        node.result->accept(*this);
    }
}

void IRGenerator::visit(VarDecl& node) {
    auto type = mapType(node.type_->typeName);
    auto reg = createRegister(std::make_shared<PointerType>(type));
    currentBlock_->addInstruction(std::make_unique<AllocaInst>(reg, type));
    addSymbol(node.name_, reg);
    
    if (node.initializer_) {
        node.initializer_->accept(*this);
        auto val = valueStack_.top();
        valueStack_.pop();
        
        if (type->isReferenceType()) {
            currentBlock_->addInstruction(std::make_unique<RetainInst>(val));
        }
        currentBlock_->addInstruction(std::make_unique<StoreInst>(val, reg));
    }
}

void IRGenerator::visit(ReturnNode& node) {
    std::shared_ptr<Value> val = nullptr;
    if (node.returnValue) {
        node.returnValue->accept(*this);
        val = valueStack_.top();
        valueStack_.pop();
    }
    
    // ARC release before return
    // Note: We need to handle nested scopes if return is deep
    popScope();
    
    currentBlock_->addInstruction(std::make_unique<ReturnInst>(val));
}

void IRGenerator::visit(BinExpr& node) {
    node.left_->accept(*this);
    if (valueStack_.empty()) return;
    auto lhs = valueStack_.top();
    valueStack_.pop();
    
    node.right_->accept(*this);
    if (valueStack_.empty()) {
        valueStack_.push(lhs); // Put it back
        return;
    }
    auto rhs = valueStack_.top();
    valueStack_.pop();
    
    if (node.op_ == "+" || node.op_ == "-" || node.op_ == "*" || node.op_ == "/") {
        auto res = createRegister(lhs->getType());
        OpCode op;
        if (node.op_ == "+") op = OpCode::Add;
        else if (node.op_ == "-") op = OpCode::Sub;
        else if (node.op_ == "*" ) op = OpCode::Mul;
        else op = OpCode::Div;
        currentBlock_->addInstruction(std::make_unique<BinaryInst>(op, res, lhs, rhs));
        valueStack_.push(res);
    } else {
        auto res = createRegister(std::make_shared<PrimitiveType>(TypeKind::Bool));
        std::string pred;
        if (node.op_ == "==") pred = "eq";
        else if (node.op_ == "!=") pred = "ne";
        else if (node.op_ == ">") pred = "sgt";
        else if (node.op_ == ">=") pred = "sge";
        else if (node.op_ == "<") pred = "slt";
        else if (node.op_ == "<=") pred = "sle";
        else pred = "eq";
        
        currentBlock_->addInstruction(std::make_unique<CmpInst>(pred, res, lhs, rhs));
        valueStack_.push(res);
    }
}

void IRGenerator::visit(ConstInt& node) {
    auto val = std::make_shared<Constant>(std::to_string(node.value_), std::make_shared<PrimitiveType>(TypeKind::Int));
    valueStack_.push(val);
}

void IRGenerator::visit(ConstFloat& node) {
    auto val = std::make_shared<Constant>(std::to_string(node.value_), std::make_shared<PrimitiveType>(TypeKind::Float));
    valueStack_.push(val);
}

void IRGenerator::visit(ConstString& node) {
    auto val = std::make_shared<Constant>("\"" + node.value_ + "\"", std::make_shared<RecordType>("String"));
    valueStack_.push(val);
}

void IRGenerator::visit(ConstId& node) {
    auto sym = findSymbol(node.value_);
    if (!sym) {
        std::cerr << "Error: symbol not found " << node.value_ << std::endl;
        // Instead of exit, push a dummy to avoid crash, but real compiler should error earlier
        valueStack_.push(std::make_shared<Constant>("0", std::make_shared<PrimitiveType>(TypeKind::Int)));
        return;
    }
    
    if (sym->getType()->getKind() == TypeKind::Pointer) {
        auto ptrType = std::static_pointer_cast<PointerType>(sym->getType());
        auto baseType = ptrType->getBaseType();
        // Load the value from pointer
        auto reg = createRegister(baseType);
        currentBlock_->addInstruction(std::make_unique<LoadInst>(reg, sym));
        valueStack_.push(reg);
    } else {
        valueStack_.push(sym);
    }
}

void IRGenerator::visit(AssignNode& node) {
    auto sym = findSymbol(node.target_);
    if (!sym) exit(1);

    node.expr_->accept(*this);
    auto val = valueStack_.top();
    valueStack_.pop();

    if (val->getType()->isReferenceType()) {
        currentBlock_->addInstruction(std::make_unique<RetainInst>(val));
        
        // Load old value and release it
        auto oldVal = createRegister(val->getType());
        currentBlock_->addInstruction(std::make_unique<LoadInst>(oldVal, sym));
        currentBlock_->addInstruction(std::make_unique<ReleaseInst>(oldVal));
    }

    currentBlock_->addInstruction(std::make_unique<StoreInst>(val, sym));
}

void IRGenerator::visit(IfNode& node) {
    auto thenL = createBlockLabel("then");
    auto elseL = createBlockLabel("else");
    auto mergeL = createBlockLabel("merge");

    node.condition_->accept(*this);
    if (valueStack_.empty()) return; // Safety check
    auto cond = valueStack_.top();
    valueStack_.pop();

    // We need to handle if-as-expression by using a temporary storage (alloca)
    // To know the type, we might need to visit thenBody first or pre-calculate it.
    // Simplified: we'll peek into thenBody to see if it has a result.
    std::shared_ptr<Value> resultAlloca = nullptr;
    if (node.thenBody_->result) {
        // Temporary visit to get type (not ideal but works for this simplified IR gen)
        // A better way would be a separate type-checking pass.
        auto savedBlock = currentBlock_;
        auto savedFunc = currentFunction_;
        // We don't actually visit, we just map the type if possible or use a default
        auto type = std::make_shared<PrimitiveType>(TypeKind::Int); // Default
        if (node.elseBody_ == nullptr) {
             // if-as-expression usually has else, but if not...
        }
        resultAlloca = createRegister(std::make_shared<PointerType>(type));
        currentFunction_->blocks.front()->addInstruction(std::make_unique<AllocaInst>(resultAlloca, type));
    }

    currentBlock_->addInstruction(std::make_unique<CondBranchInst>(cond, thenL, elseL));

    // Then
    auto thenB = std::make_unique<BasicBlock>(thenL);
    currentBlock_ = thenB.get();
    currentFunction_->addBlock(std::move(thenB));
    pushScope();
    node.thenBody_->accept(*this);
    
    if (node.thenBody_->result && !valueStack_.empty() && resultAlloca) {
        auto thenVal = valueStack_.top();
        valueStack_.pop();
        currentBlock_->addInstruction(std::make_unique<StoreInst>(thenVal, resultAlloca));
    }
    popScope();
    currentBlock_->addInstruction(std::make_unique<BranchInst>(mergeL));

    // Else
    auto elseB = std::make_unique<BasicBlock>(elseL);
    currentBlock_ = elseB.get();
    currentFunction_->addBlock(std::move(elseB));
    if (node.elseBody_) {
        pushScope();
        node.elseBody_->accept(*this);
        
        if (node.elseBody_->result && !valueStack_.empty() && resultAlloca) {
            auto elseVal = valueStack_.top();
            valueStack_.pop();
            currentBlock_->addInstruction(std::make_unique<StoreInst>(elseVal, resultAlloca));
        }
        popScope();
    }
    currentBlock_->addInstruction(std::make_unique<BranchInst>(mergeL));

    // Merge
    auto mergeB = std::make_unique<BasicBlock>(mergeL);
    currentBlock_ = mergeB.get();
    currentFunction_->addBlock(std::move(mergeB));
    
    if (resultAlloca) {
        auto res = createRegister(std::static_pointer_cast<PointerType>(resultAlloca->getType())->getBaseType());
        currentBlock_->addInstruction(std::make_unique<LoadInst>(res, resultAlloca));
        valueStack_.push(res);
    }
}

void IRGenerator::visit(WhileNode& node) {
    auto condL = createBlockLabel("while.cond");
    auto bodyL = createBlockLabel("while.body");
    auto endL = createBlockLabel("while.end");

    currentBlock_->addInstruction(std::make_unique<BranchInst>(condL));

    // Cond
    auto condB = std::make_unique<BasicBlock>(condL);
    currentBlock_ = condB.get();
    currentFunction_->addBlock(std::move(condB));
    node.condition_->accept(*this);
    auto cond = valueStack_.top();
    valueStack_.pop();
    currentBlock_->addInstruction(std::make_unique<CondBranchInst>(cond, bodyL, endL));

    // Body
    auto bodyB = std::make_unique<BasicBlock>(bodyL);
    currentBlock_ = bodyB.get();
    currentFunction_->addBlock(std::move(bodyB));
    pushScope();
    node.body_->accept(*this);
    popScope();
    currentBlock_->addInstruction(std::make_unique<BranchInst>(condL));

    // End
    auto endB = std::make_unique<BasicBlock>(endL);
    currentBlock_ = endB.get();
    currentFunction_->addBlock(std::move(endB));
}

void IRGenerator::visit(FunCall& node) {
    std::vector<std::shared_ptr<Value>> args;
    for (const auto& arg : node.params_) {
        arg->value->accept(*this);
        args.push_back(valueStack_.top());
        valueStack_.pop();
    }
    
    auto reg = createRegister(std::make_shared<PrimitiveType>(TypeKind::Int)); // Default i64 for now
    currentBlock_->addInstruction(std::make_unique<CallInst>(reg, node.funcName_, args));
    valueStack_.push(reg);
}

void IRGenerator::pushScope() {
    scopeStack_.push_back({});
}

void IRGenerator::popScope() {
    auto& currentScope = scopeStack_.back();
    for (auto& var : currentScope.refVariables) {
        currentBlock_->addInstruction(std::make_unique<ReleaseInst>(var));
    }
    scopeStack_.pop_back();
}

void IRGenerator::addSymbol(const std::string& name, std::shared_ptr<Value> val) {
    scopeStack_.back().symbols[name] = val;
    if (val->getType()->isReferenceType()) {
        scopeStack_.back().refVariables.push_back(val);
    }
}

std::shared_ptr<Value> IRGenerator::findSymbol(const std::string& name) {
    for (auto it = scopeStack_.rbegin(); it != scopeStack_.rend(); ++it) {
        if (it->symbols.count(name)) return it->symbols[name];
    }
    return nullptr;
}

std::shared_ptr<Type> IRGenerator::mapType(const std::string& typeName) {
    if (typeName == "Int") return std::make_shared<PrimitiveType>(TypeKind::Int);
    if (typeName == "Float") return std::make_shared<PrimitiveType>(TypeKind::Float);
    if (typeName == "Bool") return std::make_shared<PrimitiveType>(TypeKind::Bool);
    if (typeName == "String") return std::make_shared<RecordType>("String");
    return std::make_shared<RecordType>(typeName);
}

std::shared_ptr<Value> IRGenerator::createRegister(std::shared_ptr<Type> type) {
    return std::make_shared<Register>(std::to_string(nextRegisterId_++), type);
}

std::string IRGenerator::createBlockLabel(const std::string& prefix) {
    return prefix + "." + std::to_string(nextBlockId_++);
}

} // namespace zir
