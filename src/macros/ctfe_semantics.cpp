#include "macros/ctfe_semantics.hpp"

#include "ast/nodes.hpp"
#include "utils/diagnostics.hpp"

#include <charconv>
#include <limits>
#include <stdexcept>
#include <utility>

namespace zap::ctfe {

std::optional<int64_t> integerLiteral(std::string_view text, bool negated) {
  if (negated && text == "9223372036854775808")
    return std::numeric_limits<int64_t>::min();
  int64_t value = 0;
  auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      (negated && value == std::numeric_limits<int64_t>::min()))
    return std::nullopt;
  return negated ? -value : value;
}

const FunctionTypes &builtinTypes() {
  static const FunctionTypes functions{
      {"panic", {{ValueType::String}, ValueType::Never}},
      {"syntaxExpr", {{ValueType::String}, ValueType::SyntaxExpr}},
      {"syntaxItem", {{ValueType::String}, ValueType::SyntaxItem}},
      {"syntaxTokens", {{ValueType::String}, ValueType::SyntaxTokens}},
      {"syntaxCount", {{ValueType::SyntaxTokens}, ValueType::Int}},
      {"syntaxTokenText",
       {{ValueType::SyntaxTokens, ValueType::Int}, ValueType::String}},
      {"syntaxTokenSpelling",
       {{ValueType::SyntaxTokens, ValueType::Int}, ValueType::String}},
      {"syntaxTokenKind",
       {{ValueType::SyntaxTokens, ValueType::Int}, ValueType::String}},
      {"syntaxTokenSourceName",
       {{ValueType::SyntaxTokens, ValueType::Int}, ValueType::String}},
      {"syntaxTokenLine",
       {{ValueType::SyntaxTokens, ValueType::Int}, ValueType::Int}},
      {"syntaxTokenColumn",
       {{ValueType::SyntaxTokens, ValueType::Int}, ValueType::Int}},
      {"syntaxTokenOffset",
       {{ValueType::SyntaxTokens, ValueType::Int}, ValueType::Int}},
      {"syntaxTokenLength",
       {{ValueType::SyntaxTokens, ValueType::Int}, ValueType::Int}},
      {"sourceInterpolation",
       {{ValueType::SyntaxSource, ValueType::Int}, ValueType::SyntaxExpr}},
      {"syntaxExprTokens", {{ValueType::SyntaxExpr}, ValueType::SyntaxTokens}},
      {"syntaxItemTokens", {{ValueType::SyntaxItem}, ValueType::SyntaxTokens}},
      {"syntaxConcat",
       {{ValueType::SyntaxTokens, ValueType::SyntaxTokens},
        ValueType::SyntaxTokens}},
      {"syntaxSlice",
       {{ValueType::SyntaxTokens, ValueType::Int, ValueType::Int},
        ValueType::SyntaxTokens}},
      {"syntaxExprFromTokens",
       {{ValueType::SyntaxTokens}, ValueType::SyntaxExpr}},
      {"syntaxItemFromTokens",
       {{ValueType::SyntaxTokens}, ValueType::SyntaxItem}},
      {"length", {{ValueType::String}, ValueType::Int}},
      {"charAt", {{ValueType::String, ValueType::Int}, ValueType::String}},
      {"slice",
       {{ValueType::String, ValueType::Int, ValueType::Int},
        ValueType::String}}};
  return functions;
}

ValueType syntaxValueType(const SyntaxValue &value) {
  if (std::holds_alternative<SyntaxTokens>(value))
    return ValueType::SyntaxTokens;
  if (std::holds_alternative<SyntaxSource>(value))
    return ValueType::SyntaxSource;
  if (std::holds_alternative<SyntaxExpr>(value))
    return ValueType::SyntaxExpr;
  return ValueType::SyntaxItem;
}

std::string typeName(ValueType type) {
  switch (type) {
  case ValueType::Void:
    return "Void";
  case ValueType::Bool:
    return "Bool";
  case ValueType::Int:
    return "Int";
  case ValueType::String:
    return "String";
  case ValueType::SyntaxTokens:
    return "SyntaxTokens";
  case ValueType::SyntaxSource:
    return "SyntaxSource";
  case ValueType::SyntaxExpr:
    return "SyntaxExpr";
  case ValueType::SyntaxItem:
    return "SyntaxItem";
  case ValueType::Never:
    return "Never";
  }
  throw std::logic_error("invalid CTFE type");
}

namespace {

struct InvalidDefinition {};
struct Local {
  ValueType type;
  bool mutableValue;
};
struct Scope {
  Scope *parent;
  std::map<std::string, Local> locals;
  const Local *find(const std::string &name) const {
    auto found = locals.find(name);
    if (found != locals.end())
      return &found->second;
    return parent ? parent->find(name) : nullptr;
  }
};

class Validator {
public:
  Validator(zap::DiagnosticEngine &diagnostics, FunctionTypes &functions)
      : diagnostics_(diagnostics), functions_(functions) {}

  void validate(const RootNode &root) {
    for (const auto &node : root.children) {
      const auto *function = dynamic_cast<const FunDecl *>(node.get());
      if (!function || function->isExtern_ || function->isUnsafe_ ||
          function->isStatic_ || !function->body_ || function->lambdaExpr_ ||
          function->returnsRef_ || function->resultBorrowSource_ ||
          !function->attributes_.empty() || !function->genericParams_.empty() ||
          !function->genericConstraints_.empty())
        fail(*node, "CTFE accepts only ordinary, safe functions.");
      FunctionType signature{{}, declaredType(function->returnType_.get())};
      for (const auto &parameter : function->params_) {
        if (parameter->isRef || parameter->isSink || parameter->isNoEscape ||
            parameter->isVariadic || parameter->defaultValue)
          fail(*parameter, "Unsupported CTFE function parameter.");
        auto type = declaredType(parameter->type.get());
        requireValue(*parameter, type);
        signature.parameters.push_back(type);
      }
      if (builtinTypes().count(function->name_) ||
          !functions_.emplace(function->name_, std::move(signature)).second)
        fail(*function, "Duplicate or reserved CTFE function name.");
    }
    for (const auto &node : root.children) {
      const auto &function = dynamic_cast<const FunDecl &>(*node);
      const auto &signature = functions_.at(function.name_);
      Scope parameters{nullptr, {}};
      for (size_t i = 0; i < function.params_.size(); ++i)
        declare(*function.params_[i], parameters, function.params_[i]->name,
                signature.parameters[i], false);
      bool terminates = body(*function.body_, parameters, signature.result);
      if (!terminates && signature.result != ValueType::Void)
        fail(function, "CTFE function '" + function.name_ +
                           "' does not return a value on every path.");
    }
  }

private:
  zap::DiagnosticEngine &diagnostics_;
  FunctionTypes &functions_;

  [[noreturn]] void fail(const Node &node, const std::string &message) {
    diagnostics_.report(node.span, zap::DiagnosticLevel::Error, "M3002",
                        message);
    throw InvalidDefinition();
  }

  ValueType declaredType(const TypeNode *node) {
    if (!node)
      return ValueType::Void;
    if (node->isReference || node->isPointer || node->isArray ||
        node->isVarArgs || node->isWeak || node->isFailable || node->isFunPtr ||
        node->baseType || !node->qualifiers.empty() ||
        !node->genericArgs.empty() || node->defaultType)
      fail(*node, "Unsupported CTFE type.");
    for (auto type :
         {ValueType::Void, ValueType::Bool, ValueType::Int, ValueType::String,
          ValueType::SyntaxTokens, ValueType::SyntaxSource,
          ValueType::SyntaxExpr, ValueType::SyntaxItem})
      if (node->typeName == typeName(type))
        return type;
    fail(*node, "Unsupported CTFE type '" + node->typeName + "'.");
  }

  void expect(const Node &node, ValueType actual, ValueType expected) {
    if (actual != expected && actual != ValueType::Never)
      fail(node, "CTFE type mismatch: expected " + typeName(expected) +
                     ", got " + typeName(actual) + ".");
  }

  void requireValue(const Node &node, ValueType type) {
    if (type == ValueType::Void || type == ValueType::Never)
      fail(node, "CTFE binding requires a value type.");
  }

  void declare(const Node &node, Scope &scope, const std::string &name,
               ValueType type, bool mutableValue) {
    if (!scope.locals.emplace(name, Local{type, mutableValue}).second)
      fail(node, "Duplicate CTFE binding '" + name + "'.");
  }

  ValueType expression(const ExpressionNode &node, Scope &scope) {
    if (const auto *literal = dynamic_cast<const ConstInt *>(&node)) {
      if (!integerLiteral(literal->value_))
        fail(node, "Unsupported CTFE integer literal.");
      return ValueType::Int;
    }
    if (dynamic_cast<const ConstBool *>(&node))
      return ValueType::Bool;
    if (dynamic_cast<const ConstString *>(&node))
      return ValueType::String;
    if (const auto *id = dynamic_cast<const ConstId *>(&node)) {
      const auto *local = scope.find(id->value_);
      if (!local)
        fail(node, "Unknown CTFE binding '" + id->value_ + "'.");
      return local->type;
    }
    if (const auto *unary = dynamic_cast<const UnaryExpr *>(&node)) {
      if (unary->op_ == "-") {
        if (const auto *literal =
                dynamic_cast<const ConstInt *>(unary->expr_.get())) {
          if (!integerLiteral(literal->value_, true))
            fail(node, "Unsupported CTFE integer literal.");
          return ValueType::Int;
        }
      }
      auto operand = expression(*unary->expr_, scope);
      if (unary->op_ == "!") {
        expect(node, operand, ValueType::Bool);
        return operand == ValueType::Never ? operand : ValueType::Bool;
      }
      if (unary->op_ == "+" || unary->op_ == "-") {
        expect(node, operand, ValueType::Int);
        return operand == ValueType::Never ? operand : ValueType::Int;
      }
      fail(node, "Unsupported CTFE unary operator.");
    }
    if (const auto *binary = dynamic_cast<const BinExpr *>(&node)) {
      auto left = expression(*binary->left_, scope);
      auto right = expression(*binary->right_, scope);
      const auto &op = binary->op_;
      if (op == "&&" || op == "||") {
        expect(*binary->left_, left, ValueType::Bool);
        expect(*binary->right_, right, ValueType::Bool);
        return left == ValueType::Never ? left : ValueType::Bool;
      }
      if (op == "==" || op == "!=") {
        auto type = left == ValueType::Never ? right : left;
        if (type != ValueType::Int && type != ValueType::String &&
            type != ValueType::Bool && type != ValueType::Never)
          fail(node, "Unsupported CTFE comparison.");
        expect(node, right, type);
        return left == ValueType::Never || right == ValueType::Never
                   ? ValueType::Never
                   : ValueType::Bool;
      }
      if (op == "+" &&
          (left == ValueType::String || right == ValueType::String)) {
        expect(node, left, ValueType::String);
        expect(node, right, ValueType::String);
        return left == ValueType::Never || right == ValueType::Never
                   ? ValueType::Never
                   : ValueType::String;
      }
      if (op != "+" && op != "-" && op != "*" && op != "/" && op != "%" &&
          op != "<" && op != "<=" && op != ">" && op != ">=")
        fail(node, "Unsupported CTFE binary operator.");
      expect(*binary->left_, left, ValueType::Int);
      expect(*binary->right_, right, ValueType::Int);
      if (left == ValueType::Never || right == ValueType::Never)
        return ValueType::Never;
      return op == "<" || op == "<=" || op == ">" || op == ">="
                 ? ValueType::Bool
                 : ValueType::Int;
    }
    if (const auto *ternary = dynamic_cast<const TernaryExpr *>(&node)) {
      auto condition = expression(*ternary->condition_, scope);
      expect(node, condition, ValueType::Bool);
      auto thenType = expression(*ternary->thenExpr_, scope);
      auto elseType = expression(*ternary->elseExpr_, scope);
      auto result = thenType == ValueType::Never ? elseType : thenType;
      expect(node, elseType, result);
      return condition == ValueType::Never ? condition : result;
    }
    if (const auto *member = dynamic_cast<const MemberAccessNode *>(&node)) {
      auto base = expression(*member->left_, scope);
      if (base == ValueType::SyntaxSource && member->member_ == "text")
        return ValueType::String;
      if ((base == ValueType::SyntaxSource &&
           member->member_ == "interpolationCount") ||
          (base == ValueType::String && member->member_ == "len"))
        return ValueType::Int;
      fail(node, "Unsupported CTFE member access.");
    }
    if (const auto *call = dynamic_cast<const FunCall *>(&node)) {
      const auto *callee = dynamic_cast<const ConstId *>(call->callee_.get());
      if (!callee || !call->genericArgs_.empty())
        fail(node, "Only direct CTFE function calls are allowed.");
      if (scope.find(callee->value_))
        fail(node, "CTFE function call is shadowed by a local binding.");
      const FunctionType *signature = nullptr;
      auto builtin = builtinTypes().find(callee->value_);
      auto function = functions_.find(callee->value_);
      if (builtin != builtinTypes().end())
        signature = &builtin->second;
      else if (function != functions_.end())
        signature = &function->second;
      if (!signature)
        fail(node, "Forbidden or unknown CTFE call '" + callee->value_ + "'.");
      if (call->params_.size() != signature->parameters.size())
        fail(node, "Wrong CTFE function argument count.");
      bool never = false;
      for (size_t i = 0; i < call->params_.size(); ++i) {
        const auto &argument = *call->params_[i];
        if (!argument.name.empty() || argument.isRef || argument.isSpread)
          fail(node, "Unsupported CTFE call argument.");
        auto actual = expression(*argument.value, scope);
        expect(*argument.value, actual, signature->parameters[i]);
        never |= actual == ValueType::Never;
      }
      return never ? ValueType::Never : signature->result;
    }
    fail(node, "Unsupported CTFE expression.");
  }

  bool statement(const Node &node, Scope &scope, ValueType result) {
    if (const auto *binding = dynamic_cast<const BindingDecl *>(&node)) {
      if (!binding->initializer_ || binding->isGlobal_ ||
          binding->isExternal_ || !binding->attributes_.empty())
        fail(node, "Unsupported CTFE binding.");
      auto actual = expression(*binding->initializer_, scope);
      auto type = binding->type_ ? declaredType(binding->type_.get()) : actual;
      requireValue(node, type);
      expect(node, actual, type);
      declare(node, scope, binding->name_, type,
              binding->kind_ == BindingKind::Mutable);
      return actual == ValueType::Never;
    }
    if (const auto *assignment = dynamic_cast<const AssignNode *>(&node)) {
      const auto *target =
          dynamic_cast<const ConstId *>(assignment->target_.get());
      if (!target || !assignment->op_.empty())
        fail(node, "Unsupported CTFE assignment.");
      const auto *local = scope.find(target->value_);
      if (!local || !local->mutableValue)
        fail(node, "CTFE assignment requires a mutable local.");
      auto actual = expression(*assignment->expr_, scope);
      expect(node, actual, local->type);
      return actual == ValueType::Never;
    }
    if (const auto *returned = dynamic_cast<const ReturnNode *>(&node)) {
      expect(node,
             returned->returnValue ? expression(*returned->returnValue, scope)
                                   : ValueType::Void,
             result);
      return true;
    }
    if (const auto *conditional = dynamic_cast<const IfNode *>(&node)) {
      auto condition = expression(*conditional->condition_, scope);
      expect(node, condition, ValueType::Bool);
      bool thenReturns = body(*conditional->thenBody_, scope, result);
      bool elseReturns = conditional->elseBody_ &&
                         body(*conditional->elseBody_, scope, result);
      if (condition == ValueType::Never)
        return true;
      if (const auto *constant =
              dynamic_cast<const ConstBool *>(conditional->condition_.get()))
        return constant->value_ ? thenReturns : elseReturns;
      return thenReturns && elseReturns;
    }
    if (const auto *loop = dynamic_cast<const WhileNode *>(&node)) {
      auto condition = expression(*loop->condition_, scope);
      expect(node, condition, ValueType::Bool);
      (void)body(*loop->body_, scope, result);
      const auto *constant =
          dynamic_cast<const ConstBool *>(loop->condition_.get());
      return condition == ValueType::Never || (constant && constant->value_);
    }
    if (const auto *call = dynamic_cast<const FunCall *>(&node))
      return expression(*call, scope) == ValueType::Never;
    fail(node, "Unsupported CTFE statement.");
  }

  bool body(const BodyNode &block, Scope &parent, ValueType result) {
    Scope scope{&parent, {}};
    bool terminates = false;
    for (const auto &node : block.statements)
      terminates = statement(*node, scope, result) || terminates;
    if (block.result)
      terminates =
          expression(*block.result, scope) == ValueType::Never || terminates;
    return terminates;
  }
};

} // namespace

bool validateDefinition(const RootNode &root,
                        zap::DiagnosticEngine &diagnostics,
                        FunctionTypes &functions) {
  functions.clear();
  try {
    Validator(diagnostics, functions).validate(root);
    return true;
  } catch (const InvalidDefinition &) {
    functions.clear();
    return false;
  }
}

} // namespace zap::ctfe
