#include "lsp/symbol_index.hpp"

#include "ast/nodes.hpp"

namespace zap::lsp {
namespace {

bool containsOffset(const SourceSpan &span, size_t offset) {
  return offset >= span.offset && offset <= span.offset + span.length;
}

bool startsBefore(const SourceSpan &span, size_t offset) {
  return span.offset <= offset;
}

void addSymbol(std::vector<LspSymbol> &symbols, const std::string &uri,
               const std::string &name, const SourceSpan &span, int64_t kind,
               Visibility visibility = Visibility::Private) {
  if (name.empty()) {
    return;
  }
  symbols.push_back(LspSymbol{name, uri, span, kind, visibility});
}

void collectFromBody(const BodyNode *body, size_t offset,
                     const std::string &uri, std::vector<LspSymbol> &symbols);

void collectFromExpression(const ExpressionNode *expr, size_t offset,
                           const std::string &uri,
                           std::vector<LspSymbol> &symbols) {
  if (!expr || !containsOffset(expr->span, offset)) {
    return;
  }

  if (auto body = dynamic_cast<const BodyNode *>(expr)) {
    collectFromBody(body, offset, uri, symbols);
    return;
  }
  if (auto fallback = dynamic_cast<const FallbackExpr *>(expr)) {
    collectFromExpression(fallback->expression_.get(), offset, uri, symbols);
    collectFromExpression(fallback->fallback_.get(), offset, uri, symbols);
    return;
  }
  if (auto handled = dynamic_cast<const FailableHandleExpr *>(expr)) {
    collectFromExpression(handled->expression_.get(), offset, uri, symbols);
    if (handled->handler_ && containsOffset(handled->handler_->span, offset)) {
      addSymbol(symbols, uri, handled->errorName_, handled->span, 6);
      collectFromBody(handled->handler_.get(), offset, uri, symbols);
    }
    return;
  }
  if (auto tryExpr = dynamic_cast<const TryExpr *>(expr)) {
    collectFromExpression(tryExpr->expression_.get(), offset, uri, symbols);
    return;
  }
  if (auto call = dynamic_cast<const FunCall *>(expr)) {
    collectFromExpression(call->callee_.get(), offset, uri, symbols);
    for (const auto &arg : call->params_) {
      if (arg) {
        collectFromExpression(arg->value.get(), offset, uri, symbols);
      }
    }
  }
}

void collectFromStatement(const Node *node, size_t offset,
                          const std::string &uri,
                          std::vector<LspSymbol> &symbols) {
  if (!node || !startsBefore(node->span, offset)) {
    return;
  }

  if (auto binding = dynamic_cast<const BindingDecl *>(node)) {
    if (binding->span.offset <= offset) {
      const int64_t symbolKind =
          binding->kind_ == BindingKind::CompileTimeConstant ? 21 : 6;
      addSymbol(symbols, uri, binding->name_, binding->span, symbolKind,
                binding->visibility_);
    }
    collectFromExpression(binding->initializer_.get(), offset, uri, symbols);
    return;
  }
  if (auto body = dynamic_cast<const BodyNode *>(node)) {
    collectFromBody(body, offset, uri, symbols);
    return;
  }
  if (auto ifNode = dynamic_cast<const IfNode *>(node)) {
    if (!containsOffset(ifNode->span, offset)) {
      return;
    }
    collectFromExpression(ifNode->condition_.get(), offset, uri, symbols);
    collectFromBody(ifNode->thenBody_.get(), offset, uri, symbols);
    collectFromBody(ifNode->elseBody_.get(), offset, uri, symbols);
    return;
  }
  if (auto ifType = dynamic_cast<const IfTypeNode *>(node)) {
    if (!containsOffset(ifType->span, offset)) {
      return;
    }
    if (ifType->matchType_ && ifType->matchType_->span.offset <= offset) {
      addSymbol(symbols, uri, ifType->parameterName_, ifType->span, 22);
    }
    collectFromBody(ifType->thenBody_.get(), offset, uri, symbols);
    collectFromBody(ifType->elseBody_.get(), offset, uri, symbols);
    return;
  }
  if (auto whileNode = dynamic_cast<const WhileNode *>(node)) {
    if (!containsOffset(whileNode->span, offset)) {
      return;
    }
    collectFromExpression(whileNode->condition_.get(), offset, uri, symbols);
    collectFromBody(whileNode->body_.get(), offset, uri, symbols);
    return;
  }
  if (auto forNode = dynamic_cast<const ForNode *>(node)) {
    if (!containsOffset(forNode->span, offset)) {
      return;
    }
    if (forNode->initializer_) {
      collectFromStatement(forNode->initializer_.get(), offset, uri, symbols);
    }
    collectFromExpression(forNode->condition_.get(), offset, uri, symbols);
    collectFromStatement(forNode->increment_.get(), offset, uri, symbols);
    collectFromBody(forNode->body_.get(), offset, uri, symbols);
    return;
  }
  if (auto forIn = dynamic_cast<const ForInNode *>(node)) {
    if (!containsOffset(forIn->span, offset)) {
      return;
    }
    collectFromExpression(forIn->iterable_.get(), offset, uri, symbols);
    if (forIn->body_ && startsBefore(forIn->body_->span, offset)) {
      addSymbol(symbols, uri, forIn->itemName_, forIn->span, 6);
      addSymbol(symbols, uri, forIn->indexName_, forIn->span, 6);
      collectFromBody(forIn->body_.get(), offset, uri, symbols);
    }
    return;
  }
  if (auto unsafeBlock = dynamic_cast<const UnsafeBlockNode *>(node)) {
    collectFromBody(unsafeBlock, offset, uri, symbols);
    return;
  }
  if (auto assign = dynamic_cast<const AssignNode *>(node)) {
    collectFromExpression(assign->target_.get(), offset, uri, symbols);
    collectFromExpression(assign->expr_.get(), offset, uri, symbols);
    return;
  }
  if (auto call = dynamic_cast<const FunCall *>(node)) {
    collectFromExpression(call, offset, uri, symbols);
    return;
  }
  if (auto ret = dynamic_cast<const ReturnNode *>(node)) {
    collectFromExpression(ret->returnValue.get(), offset, uri, symbols);
    return;
  }
  if (auto fail = dynamic_cast<const FailNode *>(node)) {
    collectFromExpression(fail->errorValue_.get(), offset, uri, symbols);
  }
}

void collectFromBody(const BodyNode *body, size_t offset,
                     const std::string &uri, std::vector<LspSymbol> &symbols) {
  if (!body) {
    return;
  }

  for (const auto &statement : body->statements) {
    if (!statement || statement->span.offset > offset) {
      break;
    }
    collectFromStatement(statement.get(), offset, uri, symbols);
  }
  collectFromExpression(body->result.get(), offset, uri, symbols);
}

void collectFunctionLocals(const FunDecl *fun, size_t offset,
                           const std::string &uri,
                           std::vector<LspSymbol> &symbols) {
  if (!fun || !containsOffset(fun->span, offset)) {
    return;
  }
  for (const auto &param : fun->params_) {
    if (param && param->span.offset <= offset) {
      addSymbol(symbols, uri, param->name, param->span, 6);
    }
  }
  collectFromBody(fun->body_.get(), offset, uri, symbols);
}

VisibleSymbolInfo findInfoInExpression(const ExpressionNode *expr,
                                       size_t offset, std::string_view name);

VisibleSymbolInfo findInfoInBody(const BodyNode *body, size_t offset,
                                 std::string_view name);

VisibleSymbolInfo findInfoInStatement(const Node *node, size_t offset,
                                      std::string_view name) {
  if (!node || !startsBefore(node->span, offset)) {
    return {};
  }

  if (auto binding = dynamic_cast<const BindingDecl *>(node)) {
    if (binding->name_ == name && binding->span.offset <= offset) {
      return {binding, binding->type_.get()};
    }
    return findInfoInExpression(binding->initializer_.get(), offset, name);
  }
  if (auto body = dynamic_cast<const BodyNode *>(node)) {
    return findInfoInBody(body, offset, name);
  }
  if (auto ifNode = dynamic_cast<const IfNode *>(node)) {
    if (!containsOffset(ifNode->span, offset)) {
      return {};
    }
    if (auto found =
            findInfoInExpression(ifNode->condition_.get(), offset, name);
        found.node || found.typeNode) {
      return found;
    }
    if (ifNode->thenBody_ && startsBefore(ifNode->thenBody_->span, offset)) {
      return findInfoInBody(ifNode->thenBody_.get(), offset, name);
    }
    if (ifNode->elseBody_ && startsBefore(ifNode->elseBody_->span, offset)) {
      return findInfoInBody(ifNode->elseBody_.get(), offset, name);
    }
    return {};
  }
  if (auto ifType = dynamic_cast<const IfTypeNode *>(node)) {
    if (!containsOffset(ifType->span, offset)) {
      return {};
    }
    if (ifType->parameterName_ == name && ifType->matchType_ &&
        ifType->matchType_->span.offset <= offset) {
      return {ifType, ifType->matchType_.get()};
    }
    if (ifType->thenBody_ && startsBefore(ifType->thenBody_->span, offset)) {
      return findInfoInBody(ifType->thenBody_.get(), offset, name);
    }
    if (ifType->elseBody_ && startsBefore(ifType->elseBody_->span, offset)) {
      return findInfoInBody(ifType->elseBody_.get(), offset, name);
    }
    return {};
  }
  if (auto whileNode = dynamic_cast<const WhileNode *>(node)) {
    if (!containsOffset(whileNode->span, offset)) {
      return {};
    }
    if (auto found =
            findInfoInExpression(whileNode->condition_.get(), offset, name);
        found.node || found.typeNode) {
      return found;
    }
    if (whileNode->body_ && startsBefore(whileNode->body_->span, offset)) {
      return findInfoInBody(whileNode->body_.get(), offset, name);
    }
    return {};
  }
  if (auto forNode = dynamic_cast<const ForNode *>(node)) {
    if (!containsOffset(forNode->span, offset)) {
      return {};
    }
    if (auto found =
            findInfoInStatement(forNode->initializer_.get(), offset, name);
        found.node || found.typeNode) {
      return found;
    }
    if (auto found =
            findInfoInExpression(forNode->condition_.get(), offset, name);
        found.node || found.typeNode) {
      return found;
    }
    if (auto found =
            findInfoInStatement(forNode->increment_.get(), offset, name);
        found.node || found.typeNode) {
      return found;
    }
    if (forNode->body_ && startsBefore(forNode->body_->span, offset)) {
      return findInfoInBody(forNode->body_.get(), offset, name);
    }
    return {};
  }
  if (auto forIn = dynamic_cast<const ForInNode *>(node)) {
    if (!containsOffset(forIn->span, offset)) {
      return {};
    }
    if (forIn->itemName_ == name && forIn->body_ &&
        startsBefore(forIn->body_->span, offset)) {
      return {forIn, nullptr};
    }
    return findInfoInExpression(forIn->iterable_.get(), offset, name);
  }
  if (auto unsafeBlock = dynamic_cast<const UnsafeBlockNode *>(node)) {
    return findInfoInBody(unsafeBlock, offset, name);
  }
  if (auto assign = dynamic_cast<const AssignNode *>(node)) {
    if (auto found = findInfoInExpression(assign->target_.get(), offset, name);
        found.node || found.typeNode) {
      return found;
    }
    return findInfoInExpression(assign->expr_.get(), offset, name);
  }
  if (auto call = dynamic_cast<const FunCall *>(node)) {
    return findInfoInExpression(call, offset, name);
  }
  if (auto ret = dynamic_cast<const ReturnNode *>(node)) {
    return findInfoInExpression(ret->returnValue.get(), offset, name);
  }
  if (auto fail = dynamic_cast<const FailNode *>(node)) {
    return findInfoInExpression(fail->errorValue_.get(), offset, name);
  }
  return {};
}

VisibleSymbolInfo findInfoInExpression(const ExpressionNode *expr,
                                       size_t offset, std::string_view name) {
  if (!expr || !containsOffset(expr->span, offset)) {
    return {};
  }
  if (auto body = dynamic_cast<const BodyNode *>(expr)) {
    return findInfoInBody(body, offset, name);
  }
  if (auto fallback = dynamic_cast<const FallbackExpr *>(expr)) {
    if (auto found =
            findInfoInExpression(fallback->expression_.get(), offset, name);
        found.node || found.typeNode) {
      return found;
    }
    return findInfoInExpression(fallback->fallback_.get(), offset, name);
  }
  if (auto handled = dynamic_cast<const FailableHandleExpr *>(expr)) {
    if (auto found =
            findInfoInExpression(handled->expression_.get(), offset, name);
        found.node || found.typeNode) {
      return found;
    }
    if (handled->errorName_ == name && handled->handler_ &&
        startsBefore(handled->handler_->span, offset)) {
      return {handled, nullptr};
    }
    if (handled->handler_ && startsBefore(handled->handler_->span, offset)) {
      return findInfoInBody(handled->handler_.get(), offset, name);
    }
    return {};
  }
  if (auto tryExpr = dynamic_cast<const TryExpr *>(expr)) {
    return findInfoInExpression(tryExpr->expression_.get(), offset, name);
  }
  if (auto call = dynamic_cast<const FunCall *>(expr)) {
    if (auto found = findInfoInExpression(call->callee_.get(), offset, name);
        found.node || found.typeNode) {
      return found;
    }
    for (const auto &arg : call->params_) {
      if (arg) {
        if (auto found = findInfoInExpression(arg->value.get(), offset, name);
            found.node || found.typeNode) {
          return found;
        }
      }
    }
  }
  return {};
}

VisibleSymbolInfo findInfoInBody(const BodyNode *body, size_t offset,
                                 std::string_view name) {
  if (!body) {
    return {};
  }

  VisibleSymbolInfo found;
  for (const auto &statement : body->statements) {
    if (!statement || statement->span.offset > offset) {
      break;
    }
    if (auto current = findInfoInStatement(statement.get(), offset, name);
        current.node || current.typeNode) {
      found = current;
    }
  }
  if (body->result && body->result->span.offset <= offset) {
    if (auto current = findInfoInExpression(body->result.get(), offset, name);
        current.node || current.typeNode) {
      found = current;
    }
  }
  return found;
}

} // namespace

std::vector<LspSymbol> collectLocalSymbols(const RootNode &root, size_t offset,
                                           const std::string &uri) {
  std::vector<LspSymbol> symbols;
  for (const auto &child : root.children) {
    if (!child || !containsOffset(child->span, offset)) {
      continue;
    }
    if (auto fun = dynamic_cast<const FunDecl *>(child.get())) {
      collectFunctionLocals(fun, offset, uri, symbols);
      break;
    }
    if (auto cls = dynamic_cast<const ClassDecl *>(child.get())) {
      for (const auto &field : cls->fields_) {
        if (field && field->span.offset <= offset) {
          addSymbol(symbols, uri, field->name, field->span, 8,
                    field->visibility_);
        }
      }
      for (const auto &method : cls->methods_) {
        collectFunctionLocals(method.get(), offset, uri, symbols);
      }
      break;
    }
  }
  return symbols;
}

VisibleSymbolInfo findVisibleSymbolInfo(const RootNode &root, size_t offset,
                                        std::string_view name) {
  for (const auto &child : root.children) {
    if (!child) {
      continue;
    }
    if (auto binding = dynamic_cast<const BindingDecl *>(child.get())) {
      if (binding->name_ == name && binding->span.offset <= offset) {
        return {binding, binding->type_.get()};
      }
      continue;
    }
    if (!containsOffset(child->span, offset)) {
      continue;
    }
    if (auto fun = dynamic_cast<const FunDecl *>(child.get())) {
      for (const auto &param : fun->params_) {
        if (param && param->name == name && param->span.offset <= offset) {
          return {param.get(), param->type.get()};
        }
      }
      return findInfoInBody(fun->body_.get(), offset, name);
    }
    if (auto cls = dynamic_cast<const ClassDecl *>(child.get())) {
      for (const auto &field : cls->fields_) {
        if (field && field->name == name && field->span.offset <= offset) {
          return {field.get(), field->type.get()};
        }
      }
      for (const auto &method : cls->methods_) {
        if (!method || !containsOffset(method->span, offset)) {
          continue;
        }
        for (const auto &param : method->params_) {
          if (param && param->name == name && param->span.offset <= offset) {
            return {param.get(), param->type.get()};
          }
        }
        return findInfoInBody(method->body_.get(), offset, name);
      }
    }
  }
  return {};
}

const TypeNode *findVisibleTypeNode(const RootNode &root, size_t offset,
                                    std::string_view name) {
  return findVisibleSymbolInfo(root, offset, name).typeNode;
}

} // namespace zap::lsp
