#include "binder.hpp"
#include <iostream>

namespace sema {

std::unique_ptr<BoundRootNode> Binder::bind(RootNode& root) {
  boundRoot_ = std::make_unique<BoundRootNode>();
  currentScope_ = std::make_shared<SymbolTable>();
  hadError_ = false;
  
  // First pass: Declare all functions (to support recursion and forward calls)
  for (const auto& child : root.children) {
    if (auto funDecl = dynamic_cast<FunDecl*>(child.get())) {
      std::vector<std::shared_ptr<VariableSymbol>> params;
      for (const auto& p : funDecl->params_) {
        params.push_back(std::make_shared<VariableSymbol>(p->name, mapType(p->type->typeName)));
      }
      auto retType = funDecl->returnType_ ? mapType(funDecl->returnType_->typeName) : std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
      auto symbol = std::make_shared<FunctionSymbol>(funDecl->name_, std::move(params), std::move(retType));
      
      if (!currentScope_->declare(funDecl->name_, symbol)) {
        error("Function '" + funDecl->name_ + "' already declared.");
      }
    }
  }

  // Second pass: Bind function bodies
  root.accept(*this);
  return hadError_ ? nullptr : std::move(boundRoot_);
}

void Binder::visit(RootNode& node) {
  for (const auto& child : node.children) {
    child->accept(*this);
  }
}

void Binder::visit(FunDecl& node) {
  auto symbol = std::dynamic_pointer_cast<FunctionSymbol>(currentScope_->lookup(node.name_));
  if (!symbol) {
    error("Internal error: Function symbol not found for " + node.name_);
    return;
  }

  pushScope();
  
  // Declare parameters in the function scope
  for (const auto& param : symbol->parameters) {
    if (!currentScope_->declare(param->name, param)) {
      error("Parameter '" + param->name + "' already declared.");
    }
  }

  if (node.body_) {
    node.body_->accept(*this);
    // BodyNode visitor will push a BoundBlock to somewhere? 
    // Wait, I need a way to get the BoundBlock.
    // Let's assume visit(BodyNode) results in a BoundBlock being available.
    // For now, I'll manually handle BodyNode here or adjust visit(BodyNode).
  }
  
  auto boundBody = std::make_unique<BoundBlock>();
  if (currentBlock_) {
    boundBody = std::move(currentBlock_);
  }
  
  popScope();
  
  boundRoot_->functions.push_back(std::make_unique<BoundFunctionDeclaration>(symbol, std::move(boundBody)));
}

void Binder::visit(BodyNode& node) {
  auto oldBlock = std::move(currentBlock_);
  currentBlock_ = std::make_unique<BoundBlock>();
  
  for (const auto& stmt : node.statements) {
    stmt->accept(*this);
    if (!statementStack_.empty()) {
      currentBlock_->statements.push_back(std::move(statementStack_.top()));
      statementStack_.pop();
    }
  }
  
  if (node.result) {
    node.result->accept(*this);
    // If it's a BodyNode as an expression, we might need to handle the result.
    // For now, Zap seems to treat it as an expression.
  }
  
  // We don't pop currentBlock_ yet, because the caller (like visit(FunDecl)) expects it.
  // Actually, this is a bit messy. Let's use a stack for blocks if needed.
}

void Binder::visit(VarDecl& node) {
  auto type = mapType(node.type_->typeName);
  std::unique_ptr<BoundExpression> initializer = nullptr;
  
  if (node.initializer_) {
    node.initializer_->accept(*this);
    if (!expressionStack_.empty()) {
      initializer = std::move(expressionStack_.top());
      expressionStack_.pop();
    }
    
    // TODO: Type checking (initializer->type vs type)
  }
  
  auto symbol = std::make_shared<VariableSymbol>(node.name_, type);
  if (!currentScope_->declare(node.name_, symbol)) {
    error("Variable '" + node.name_ + "' already declared.");
  }
  
  statementStack_.push(std::make_unique<BoundVariableDeclaration>(symbol, std::move(initializer)));
}

void Binder::visit(ReturnNode& node) {
  std::unique_ptr<BoundExpression> expr = nullptr;
  if (node.returnValue) {
    node.returnValue->accept(*this);
    expr = std::move(expressionStack_.top());
    expressionStack_.pop();
  }
  statementStack_.push(std::make_unique<BoundReturnStatement>(std::move(expr)));
}

void Binder::visit(BinExpr& node) {
  node.left_->accept(*this);
  if (expressionStack_.empty()) return;
  auto left = std::move(expressionStack_.top());
  expressionStack_.pop();
  
  node.right_->accept(*this);
  if (expressionStack_.empty()) return;
  auto right = std::move(expressionStack_.top());
  expressionStack_.pop();
  
  // Simple type propagation (should be more robust)
  auto type = left->type;
  
  expressionStack_.push(std::make_unique<BoundBinaryExpression>(std::move(left), node.op_, std::move(right), type));
}

void Binder::visit(ConstInt& node) {
  expressionStack_.push(std::make_unique<BoundLiteral>(std::to_string(node.value_), std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int)));
}

void Binder::visit(ConstFloat& node) {
  expressionStack_.push(std::make_unique<BoundLiteral>(std::to_string(node.value_), std::make_shared<zir::PrimitiveType>(zir::TypeKind::Float)));
}

void Binder::visit(ConstString& node) {
  expressionStack_.push(std::make_unique<BoundLiteral>(node.value_, std::make_shared<zir::RecordType>("String")));
}

void Binder::visit(ConstId& node) {
  auto symbol = currentScope_->lookup(node.value_);
  if (!symbol) {
    error("Undefined identifier: " + node.value_);
    return;
  }
  
  if (auto varSymbol = std::dynamic_pointer_cast<VariableSymbol>(symbol)) {
    expressionStack_.push(std::make_unique<BoundVariableExpression>(varSymbol));
  } else {
    error("'" + node.value_ + "' is not a variable.");
  }
}

void Binder::visit(AssignNode& node) {
  auto symbol = currentScope_->lookup(node.target_);
  if (!symbol) {
    error("Undefined identifier: " + node.target_);
    return;
  }
  
  auto varSymbol = std::dynamic_pointer_cast<VariableSymbol>(symbol);
  if (!varSymbol) {
    error("Cannot assign to '" + node.target_ + "' (not a variable).");
    return;
  }
  
  node.expr_->accept(*this);
  if (expressionStack_.empty()) return;
  auto expr = std::move(expressionStack_.top());
  expressionStack_.pop();
  
  statementStack_.push(std::make_unique<BoundAssignment>(varSymbol, std::move(expr)));
}

void Binder::visit(FunCall& node) {
  auto symbol = currentScope_->lookup(node.funcName_);
  if (!symbol) {
    error("Undefined function: " + node.funcName_);
    return;
  }
  
  auto funcSymbol = std::dynamic_pointer_cast<FunctionSymbol>(symbol);
  if (!funcSymbol) {
    error("'" + node.funcName_ + "' is not a function.");
    return;
  }
  
  std::vector<std::unique_ptr<BoundExpression>> boundArgs;
  for (const auto& arg : node.params_) {
    arg->value->accept(*this);
    if (expressionStack_.empty()) return;
    boundArgs.push_back(std::move(expressionStack_.top()));
    expressionStack_.pop();
  }
  
  expressionStack_.push(std::make_unique<BoundFunctionCall>(funcSymbol, std::move(boundArgs)));
}

void Binder::visit(IfNode& node) {
  // TODO: Implement BoundIfNode and visitor
  // For now, just visit children to not lose them
  if (node.condition_) node.condition_->accept(*this);
  if (node.thenBody_) node.thenBody_->accept(*this);
  if (node.elseBody_) node.elseBody_->accept(*this);
  // expressionStack_.push(something); // if it's an expression
}

void Binder::visit(WhileNode& node) {
  // TODO: Implement BoundWhileNode and visitor
  if (node.condition_) node.condition_->accept(*this);
  if (node.body_) node.body_->accept(*this);
}

void Binder::pushScope() {
  currentScope_ = std::make_shared<SymbolTable>(currentScope_);
}

void Binder::popScope() {
  if (currentScope_) {
    currentScope_ = currentScope_->getParent();
  }
}

std::shared_ptr<zir::Type> Binder::mapType(const std::string& typeName) {
  if (typeName == "Int") return std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int);
  if (typeName == "Float") return std::make_shared<zir::PrimitiveType>(zir::TypeKind::Float);
  if (typeName == "Bool") return std::make_shared<zir::PrimitiveType>(zir::TypeKind::Bool);
  if (typeName == "String") return std::make_shared<zir::RecordType>("String");
  return std::make_shared<zir::RecordType>(typeName);
}

void Binder::error(const std::string& message) {
  std::cerr << "Semantic Error: " << message << std::endl;
  hadError_ = true;
}

} // namespace sema
