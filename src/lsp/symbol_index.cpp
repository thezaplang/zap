#include "lsp/symbol_index.hpp"

#include "ast/nodes.hpp"
#include <optional>

namespace zap::lsp {
namespace {

bool containsOffset(const SourceSpan &span, size_t offset) {
  return offset >= span.offset && offset <= span.offset + span.length;
}

bool startsBefore(const SourceSpan &span, size_t offset) {
  return span.offset <= offset;
}

bool bodyContainsOffset(const BodyNode *body, size_t offset) {
  if (!body) {
    return false;
  }
  return containsOffset(body->span, offset) || startsBefore(body->span, offset);
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

  if (auto var = dynamic_cast<const VarDecl *>(node)) {
    if (var->span.offset <= offset) {
      addSymbol(symbols, uri, var->name_, var->span, 6, var->visibility_);
    }
    collectFromExpression(var->initializer_.get(), offset, uri, symbols);
    return;
  }
  if (auto cnst = dynamic_cast<const ConstDecl *>(node)) {
    if (cnst->span.offset <= offset) {
      addSymbol(symbols, uri, cnst->name_, cnst->span, 21, cnst->visibility_);
    }
    collectFromExpression(cnst->initializer_.get(), offset, uri, symbols);
    return;
  }
  if (auto body = dynamic_cast<const BodyNode *>(node)) {
    collectFromBody(body, offset, uri, symbols);
    return;
  }
  if (auto ifNode = dynamic_cast<const IfNode *>(node)) {
    collectFromExpression(ifNode->condition_.get(), offset, uri, symbols);
    collectFromBody(ifNode->thenBody_.get(), offset, uri, symbols);
    collectFromBody(ifNode->elseBody_.get(), offset, uri, symbols);
    return;
  }
  if (auto ifType = dynamic_cast<const IfTypeNode *>(node)) {
    if (ifType->matchType_ && ifType->matchType_->span.offset <= offset) {
      addSymbol(symbols, uri, ifType->parameterName_, ifType->span, 22);
    }
    collectFromBody(ifType->thenBody_.get(), offset, uri, symbols);
    collectFromBody(ifType->elseBody_.get(), offset, uri, symbols);
    return;
  }
  if (auto whileNode = dynamic_cast<const WhileNode *>(node)) {
    collectFromExpression(whileNode->condition_.get(), offset, uri, symbols);
    collectFromBody(whileNode->body_.get(), offset, uri, symbols);
    return;
  }
  if (auto forNode = dynamic_cast<const ForNode *>(node)) {
    if (forNode->initializer_) {
      collectFromStatement(forNode->initializer_.get(), offset, uri, symbols);
    }
    collectFromExpression(forNode->condition_.get(), offset, uri, symbols);
    collectFromStatement(forNode->increment_.get(), offset, uri, symbols);
    collectFromBody(forNode->body_.get(), offset, uri, symbols);
    return;
  }
  if (auto forIn = dynamic_cast<const ForInNode *>(node)) {
    collectFromExpression(forIn->iterable_.get(), offset, uri, symbols);
    if (bodyContainsOffset(forIn->body_.get(), offset)) {
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
  if (!bodyContainsOffset(body, offset)) {
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

} // namespace zap::lsp
