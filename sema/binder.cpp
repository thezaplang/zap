#include "binder.hpp"
#include "../ast/enum_decl.hpp"
#include "../ast/record_decl.hpp"
#include <iostream>

namespace sema {

Binder::Binder(zap::DiagnosticEngine &diag) : _diag(diag), hadError_(false) {}

std::unique_ptr<BoundRootNode> Binder::bind(RootNode &root) {
  boundRoot_ = std::make_unique<BoundRootNode>();
  currentScope_ = std::make_shared<SymbolTable>();
  currentScope_->declare("Int", std::make_shared<TypeSymbol>(
                                    "Int", std::make_shared<zir::PrimitiveType>(
                                               zir::TypeKind::Int)));
  currentScope_->declare(
      "Float",
      std::make_shared<TypeSymbol>(
          "Float", std::make_shared<zir::PrimitiveType>(zir::TypeKind::Float)));
  currentScope_->declare(
      "Bool",
      std::make_shared<TypeSymbol>(
          "Bool", std::make_shared<zir::PrimitiveType>(zir::TypeKind::Bool)));
  currentScope_->declare(
      "Void",
      std::make_shared<TypeSymbol>(
          "Void", std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void)));
  currentScope_->declare(
      "String", std::make_shared<TypeSymbol>(
                    "String", std::make_shared<zir::RecordType>("String")));

  // Pass 1: Declare all types
  for (const auto &child : root.children) {
    if (auto recordDecl = dynamic_cast<RecordDecl *>(child.get())) {
      auto type = std::make_shared<zir::RecordType>(recordDecl->name_);
      if (!currentScope_->declare(recordDecl->name_,
                                  std::make_shared<TypeSymbol>(
                                      recordDecl->name_, std::move(type)))) {
        error(recordDecl->span,
              "Type '" + recordDecl->name_ + "' already declared.");
      }
    } else if (auto enumDecl = dynamic_cast<EnumDecl *>(child.get())) {
      auto type =
          std::make_shared<zir::EnumType>(enumDecl->name_, enumDecl->entries_);
      if (!currentScope_->declare(
              enumDecl->name_,
              std::make_shared<TypeSymbol>(enumDecl->name_, std::move(type)))) {
        error(enumDecl->span,
              "Type '" + enumDecl->name_ + "' already declared.");
      }
    }
  }

  // Pass 2: Declare all functions
  for (const auto &child : root.children) {
    if (auto funDecl = dynamic_cast<FunDecl *>(child.get())) {
      std::vector<std::shared_ptr<VariableSymbol>> params;
      for (const auto &p : funDecl->params_) {
        params.push_back(
            std::make_shared<VariableSymbol>(p->name, mapType(*p->type)));
      }
      auto retType =
          funDecl->returnType_
              ? mapType(*funDecl->returnType_)
              : std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
      auto symbol = std::make_shared<FunctionSymbol>(
          funDecl->name_, std::move(params), std::move(retType));

      if (!currentScope_->declare(funDecl->name_, symbol)) {
        error(funDecl->span,
              "Function '" + funDecl->name_ + "' already declared.");
      }
    }
  }

  root.accept(*this);
  return (hadError_ || _diag.hadErrors()) ? nullptr : std::move(boundRoot_);
}

void Binder::visit(RootNode &node) {
  for (const auto &child : node.children) {
    child->accept(*this);
  }
}

void Binder::visit(FunDecl &node) {
  auto symbol = std::dynamic_pointer_cast<FunctionSymbol>(
      currentScope_->lookup(node.name_));
  if (!symbol) {
    error(node.span,
          "Internal error: Function symbol not found for " + node.name_);
    return;
  }

  pushScope();
  auto oldFunction = currentFunction_;
  currentFunction_ = symbol;

  for (const auto &param : symbol->parameters) {
    if (!currentScope_->declare(param->name, param)) {
      error(node.span, "Parameter '" + param->name + "' already declared.");
    }
  }

  if (node.body_) {
    node.body_->accept(*this);
  }

  auto boundBody = std::make_unique<BoundBlock>();
  if (currentBlock_) {
    boundBody = std::move(currentBlock_);
  }

  popScope();
  currentFunction_ = oldFunction;

  boundRoot_->functions.push_back(
      std::make_unique<BoundFunctionDeclaration>(symbol, std::move(boundBody)));
}

void Binder::visit(BodyNode &node) {
  auto savedBlock = std::move(currentBlock_);
  currentBlock_ = std::make_unique<BoundBlock>();

  for (const auto &stmt : node.statements) {
    stmt->accept(*this);
    if (!statementStack_.empty()) {
      currentBlock_->statements.push_back(std::move(statementStack_.top()));
      statementStack_.pop();
    }
  }

  if (node.result) {
    node.result->accept(*this);
  }
}

void Binder::visit(VarDecl &node) {
  auto type = mapType(*node.type_);
  std::unique_ptr<BoundExpression> initializer = nullptr;

  if (node.initializer_) {
    node.initializer_->accept(*this);
    if (!expressionStack_.empty()) {
      initializer = std::move(expressionStack_.top());
      expressionStack_.pop();

      if (!canConvert(initializer->type, type)) {
        error(node.span, "Cannot assign expression of type '" +
                             initializer->type->toString() +
                             "' to variable of type '" + type->toString() +
                             "'");
      }
    }
  }

  auto symbol = std::make_shared<VariableSymbol>(node.name_, type);
  if (!currentScope_->declare(node.name_, symbol)) {
    error(node.span, "Variable '" + node.name_ + "' already declared.");
  }

  statementStack_.push(std::make_unique<BoundVariableDeclaration>(
      symbol, std::move(initializer)));
}

void Binder::visit(ReturnNode &node) {
  std::unique_ptr<BoundExpression> expr = nullptr;
  if (node.returnValue) {
    node.returnValue->accept(*this);
    if (!expressionStack_.empty()) {
      expr = std::move(expressionStack_.top());
      expressionStack_.pop();
    }
  }

  if (currentFunction_) {
    auto expectedType = currentFunction_->returnType;
    auto actualType =
        expr ? expr->type
             : std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
    if (!canConvert(actualType, expectedType)) {
      error(node.span, "Function '" + currentFunction_->name +
                           "' expects return type '" +
                           expectedType->toString() + "', but received '" +
                           actualType->toString() + "'");
    }
  }

  statementStack_.push(std::make_unique<BoundReturnStatement>(std::move(expr)));
}

void Binder::visit(BinExpr &node) {
  node.left_->accept(*this);
  if (expressionStack_.empty())
    return;
  auto left = std::move(expressionStack_.top());
  expressionStack_.pop();

  node.right_->accept(*this);
  if (expressionStack_.empty())
    return;
  auto right = std::move(expressionStack_.top());
  expressionStack_.pop();

  auto type = left->type;
  if (node.op_ == "+" || node.op_ == "-" || node.op_ == "*" ||
      node.op_ == "/") {
    if (!isNumeric(left->type) || !isNumeric(right->type)) {
      error(node.span, "Operator '" + node.op_ +
                           "' cannot be applied to types '" +
                           left->type->toString() + "' and '" +
                           right->type->toString() + "'");
    }
    type = getPromotedType(left->type, right->type);
  } else if (node.op_ == "==" || node.op_ == "!=" || node.op_ == "<" ||
             node.op_ == ">" || node.op_ == "<=" || node.op_ == ">=") {
    if (!canConvert(left->type, right->type) &&
        !canConvert(right->type, left->type)) {
      error(node.span, "Incompatible types for comparison: '" +
                           left->type->toString() + "' and '" +
                           right->type->toString() + "'");
    }
    type = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Bool);
  }
  // TODO: Handle other operators (comparison, logical)

  expressionStack_.push(std::make_unique<BoundBinaryExpression>(
      std::move(left), node.op_, std::move(right), type));
}

void Binder::visit(ConstInt &node) {
  expressionStack_.push(std::make_unique<BoundLiteral>(
      std::to_string(node.value_),
      std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int)));
}

void Binder::visit(ConstFloat &node) {
  expressionStack_.push(std::make_unique<BoundLiteral>(
      std::to_string(node.value_),
      std::make_shared<zir::PrimitiveType>(zir::TypeKind::Float)));
}

void Binder::visit(ConstString &node) {
  expressionStack_.push(std::make_unique<BoundLiteral>(
      node.value_, std::make_shared<zir::RecordType>("String")));
}

void Binder::visit(ConstId &node) {
  auto symbol = currentScope_->lookup(node.value_);
  if (!symbol) {
    error(node.span, "Undefined identifier: " + node.value_);
    return;
  }

  if (auto varSymbol = std::dynamic_pointer_cast<VariableSymbol>(symbol)) {
    expressionStack_.push(std::make_unique<BoundVariableExpression>(varSymbol));
  } else {
    error(node.span, "'" + node.value_ + "' is not a variable.");
  }
}

void Binder::visit(AssignNode &node) {
  auto symbol = currentScope_->lookup(node.target_);
  if (!symbol) {
    error(node.span, "Undefined identifier: " + node.target_);
    return;
  }

  auto varSymbol = std::dynamic_pointer_cast<VariableSymbol>(symbol);
  if (!varSymbol) {
    error(node.span,
          "Cannot assign to '" + node.target_ + "' (not a variable).");
    return;
  }

  node.expr_->accept(*this);
  if (expressionStack_.empty())
    return;
  auto expr = std::move(expressionStack_.top());
  expressionStack_.pop();

  if (!canConvert(expr->type, varSymbol->type)) {
    error(node.span, "Cannot assign expression of type '" +
                         expr->type->toString() + "' to variable of type '" +
                         varSymbol->type->toString() + "'");
  }

  statementStack_.push(
      std::make_unique<BoundAssignment>(varSymbol, std::move(expr)));
}

void Binder::visit(FunCall &node) {
  auto symbol = currentScope_->lookup(node.funcName_);
  if (!symbol) {
    error(node.span, "Undefined function: " + node.funcName_);
    return;
  }

  auto funcSymbol = std::dynamic_pointer_cast<FunctionSymbol>(symbol);
  if (!funcSymbol) {
    error(node.span, "'" + node.funcName_ + "' is not a function.");
    return;
  }

  if (node.params_.size() != funcSymbol->parameters.size()) {
    error(node.span, "Function '" + node.funcName_ + "' expects " +
                         std::to_string(funcSymbol->parameters.size()) +
                         " arguments, but received " +
                         std::to_string(node.params_.size()));
  }

  std::vector<std::unique_ptr<BoundExpression>> boundArgs;
  for (size_t i = 0; i < node.params_.size(); ++i) {
    node.params_[i]->value->accept(*this);
    if (expressionStack_.empty())
      return;
    auto arg = std::move(expressionStack_.top());
    expressionStack_.pop();

    if (i < funcSymbol->parameters.size()) {
      auto expectedType = funcSymbol->parameters[i]->type;
      if (!canConvert(arg->type, expectedType)) {
        error(node.span, "Argument " + std::to_string(i + 1) +
                             " of function '" + node.funcName_ +
                             "' expected type '" + expectedType->toString() +
                             "', but received type '" + arg->type->toString() +
                             "'");
      }
    }
    boundArgs.push_back(std::move(arg));
  }

  expressionStack_.push(
      std::make_unique<BoundFunctionCall>(funcSymbol, std::move(boundArgs)));
}

void Binder::visit(IfNode &node) {
  if (!node.condition_) {
    error(node.span, "If condition is missing.");
    return;
  }

  node.condition_->accept(*this);
  if (expressionStack_.empty())
    return;
  auto cond = std::move(expressionStack_.top());
  expressionStack_.pop();

  if (cond->type->getKind() != zir::TypeKind::Bool) {
    error(node.span, "If condition must be of type 'Bool', but received '" +
                         cond->type->toString() + "'");
  }
  pushScope();
  auto savedMainBlockThen = std::move(currentBlock_);
  node.thenBody_->accept(*this);
  auto thenBound = std::move(currentBlock_);
  currentBlock_ = std::move(savedMainBlockThen);
  popScope();

  std::unique_ptr<BoundBlock> elseBound = nullptr;
  if (node.elseBody_) {
    pushScope();
    auto savedMainBlockElse = std::move(currentBlock_);
    node.elseBody_->accept(*this);
    elseBound = std::move(currentBlock_);
    currentBlock_ = std::move(savedMainBlockElse);
    popScope();
  }

  std::shared_ptr<zir::Type> resultType =
      std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);

  if (node.thenBody_->result) {
    if (!node.elseBody_ || !node.elseBody_->result) {
      error(node.span, "If expression with a result must have an 'else' block "
                       "with a result.");
    }
  }

  std::unique_ptr<BoundIfExpression> boundIf =
      std::make_unique<BoundIfExpression>(std::move(cond), std::move(thenBound),
                                          std::move(elseBound), resultType);

  statementStack_.push(std::move(boundIf));
}

void Binder::visit(WhileNode &node) {
  if (!node.condition_) {
    error(node.span, "While condition is missing.");
    return;
  }

  node.condition_->accept(*this);
  if (expressionStack_.empty())
    return;
  auto cond = std::move(expressionStack_.top());
  expressionStack_.pop();

  if (cond->type->getKind() != zir::TypeKind::Bool) {
    error(node.span, "While condition must be of type 'Bool', but received '" +
                         cond->type->toString() + "'");
  }

  pushScope();
  auto savedMainBlockWhile = std::move(currentBlock_);
  node.body_->accept(*this);
  auto boundBody = std::move(currentBlock_);
  currentBlock_ = std::move(savedMainBlockWhile);
  popScope();

  std::unique_ptr<BoundWhileStatement> boundWhile =
      std::make_unique<BoundWhileStatement>(std::move(cond),
                                            std::move(boundBody));

  statementStack_.push(std::move(boundWhile));
}

void Binder::pushScope() {
  currentScope_ = std::make_shared<SymbolTable>(currentScope_);
}

void Binder::popScope() {
  if (currentScope_) {
    currentScope_ = currentScope_->getParent();
  }
}

std::shared_ptr<zir::Type> Binder::mapType(const TypeNode &typeNode) {
  auto symbol = currentScope_->lookup(typeNode.typeName);
  std::shared_ptr<zir::Type> type = nullptr;

  if (symbol && symbol->getKind() == SymbolKind::Type) {
    type = symbol->type;
  } else {
    if (typeNode.typeName == "Int")
      type = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int);
    else if (typeNode.typeName == "Float")
      type = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Float);
    else if (typeNode.typeName == "Bool")
      type = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Bool);
    else if (typeNode.typeName == "String")
      type = std::make_shared<zir::RecordType>("String");
    else
      type = std::make_shared<zir::RecordType>(typeNode.typeName);
  }

  if (typeNode.isArray) {
    size_t size = 0;
    if (auto constInt = dynamic_cast<ConstInt *>(typeNode.arraySize.get())) {
      size = constInt->value_;
    }
    type = std::make_shared<zir::ArrayType>(std::move(type), size);
  }

  if (typeNode.isPointer) {
    type = std::make_shared<zir::PointerType>(std::move(type));
  }

  return type;
}

void Binder::visit(ConstBool &node) {
  expressionStack_.push(std::make_unique<BoundLiteral>(
      node.value_ ? "true" : "false",
      std::make_shared<zir::PrimitiveType>(zir::TypeKind::Bool)));
}

void Binder::visit(UnaryExpr &node) {
  node.expr_->accept(*this);
  if (expressionStack_.empty())
    return;
  auto expr = std::move(expressionStack_.top());
  expressionStack_.pop();

  auto type = expr->type;
  if (node.op_ == "-" || node.op_ == "+") {
    if (!isNumeric(type)) {
      error(node.span, "Operator '" + node.op_ +
                           "' cannot be applied to type '" + type->toString() +
                           "'");
    }
  } else if (node.op_ == "!") {
    if (type->getKind() != zir::TypeKind::Bool) {
      error(node.span, "Operator '!' cannot be applied to type '" +
                           type->toString() + "'");
    }
  }

  expressionStack_.push(
      std::make_unique<BoundUnaryExpression>(node.op_, std::move(expr), type));
}

void Binder::visit(ArrayLiteralNode &node) {
  std::vector<std::unique_ptr<BoundExpression>> elements;
  std::shared_ptr<zir::Type> elementType = nullptr;

  for (const auto &el : node.elements_) {
    el->accept(*this);
    if (!expressionStack_.empty()) {
      auto boundEl = std::move(expressionStack_.top());
      expressionStack_.pop();

      if (!elementType) {
        elementType = boundEl->type;
      } else if (!canConvert(boundEl->type, elementType)) {
        error(node.span, "Array elements must have the same type. Expected '" +
                             elementType->toString() + "', but got '" +
                             boundEl->type->toString() + "'");
      }
      elements.push_back(std::move(boundEl));
    }
  }

  auto arrayType = std::make_shared<zir::ArrayType>(
      elementType ? elementType
                  : std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void),
      elements.size());
  expressionStack_.push(
      std::make_unique<BoundArrayLiteral>(std::move(elements), arrayType));
}

void Binder::visit(RecordDecl &node) {
  auto symbol = currentScope_->lookup(node.name_);
  auto recordType = std::static_pointer_cast<zir::RecordType>(symbol->type);

  for (const auto &field : node.fields_) {
    recordType->addField(field->name, mapType(*field->type));
  }

  auto boundRecord = std::make_unique<BoundRecordDeclaration>();
  boundRecord->type = recordType;
  boundRoot_->records.push_back(std::move(boundRecord));
}

void Binder::visit(EnumDecl &node) {
  auto symbol = currentScope_->lookup(node.name_);
  auto enumType = std::static_pointer_cast<zir::EnumType>(symbol->type);

  auto boundEnum = std::make_unique<BoundEnumDeclaration>();
  boundEnum->type = enumType;
  boundRoot_->enums.push_back(std::move(boundEnum));
}

bool Binder::isNumeric(std::shared_ptr<zir::Type> type) {
  return type->getKind() == zir::TypeKind::Int ||
         type->getKind() == zir::TypeKind::Float;
}

bool Binder::canConvert(std::shared_ptr<zir::Type> from,
                        std::shared_ptr<zir::Type> to) {
  if (from->getKind() == to->getKind()) {
    if (from->getKind() == zir::TypeKind::Record) {
      return from->toString() == to->toString();
    }
    return true;
  }

  if (from->getKind() == zir::TypeKind::Int &&
      to->getKind() == zir::TypeKind::Float) {
    return true;
  }

  return false;
}

std::shared_ptr<zir::Type>
Binder::getPromotedType(std::shared_ptr<zir::Type> t1,
                        std::shared_ptr<zir::Type> t2) {
  if (t1->getKind() == zir::TypeKind::Float ||
      t2->getKind() == zir::TypeKind::Float) {
    return std::make_shared<zir::PrimitiveType>(zir::TypeKind::Float);
  }
  return t1;
}

void Binder::error(SourceSpan span, const std::string &message) {
  _diag.report(span, zap::DiagnosticLevel::Error, message);
  hadError_ = true;
}

} // namespace sema
