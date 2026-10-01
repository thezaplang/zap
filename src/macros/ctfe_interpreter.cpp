#include "macros/ctfe_interpreter.hpp"

#include "ast/nodes.hpp"
#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#include "utils/diagnostics.hpp"

#include <charconv>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace zap::ctfe {
namespace {

struct Failure {
  const char *code;
  std::string message;
};

// Reserve parser/lexer working space before either can allocate from input.
// This is a conservative logical budget, not a process-wide RSS limit.
void chargeParseBudget(const CtfeLimits &limits, size_t &used, size_t count,
                       size_t bytesPerUnit) {
  if (used > limits.maxMemoryBytes ||
      count > (limits.maxMemoryBytes - used) / bytesPerUnit)
    throw Failure{"M3003", "CTFE memory limit exceeded."};
  used += count * bytesPerUnit;
}

using SyntaxHandle = std::shared_ptr<const SyntaxValue>;
using Value =
    std::variant<std::monostate, bool, int64_t, std::string, SyntaxHandle>;

struct Binding {
  Value value;
  bool mutableValue = false;
};

struct Frame {
  Frame *parent = nullptr;
  std::map<std::string, Binding> bindings;

  Binding *find(const std::string &name) {
    auto found = bindings.find(name);
    if (found != bindings.end())
      return &found->second;
    return parent ? parent->find(name) : nullptr;
  }
};

SyntaxDiagnostic diagnostic(const SyntaxMacroRequest &request, const char *code,
                            std::string message) {
  return {SyntaxSeverity::Error, code, std::move(message), request.invocation};
}

void checkSyntaxDepth(const std::vector<Token> &tokens, size_t maximum) {
  size_t depth = 0;
  for (const auto &token : tokens) {
    if (token.type == TokenType::LPAREN || token.type == TokenType::LBRACE ||
        token.type == TokenType::SQUARE_LBRACE) {
      if (depth >= maximum)
        throw Failure{"M3003", "CTFE syntax nesting limit exceeded."};
      ++depth;
    } else if (token.type == TokenType::RPAREN ||
               token.type == TokenType::RBRACE ||
               token.type == TokenType::SQUARE_RBRACE) {
      if (depth)
        --depth;
    }
  }
}

int64_t checkedArithmetic(const std::string &op, int64_t left, int64_t right) {
  int64_t result = 0;
  if (op == "+") {
    if ((right > 0 && left > std::numeric_limits<int64_t>::max() - right) ||
        (right < 0 && left < std::numeric_limits<int64_t>::min() - right))
      throw Failure{"M3001", "Compile-time integer overflow."};
    result = left + right;
  } else if (op == "-") {
    if ((right < 0 && left > std::numeric_limits<int64_t>::max() + right) ||
        (right > 0 && left < std::numeric_limits<int64_t>::min() + right))
      throw Failure{"M3001", "Compile-time integer overflow."};
    result = left - right;
  } else if (op == "*") {
    const auto minimum = std::numeric_limits<int64_t>::min();
    const auto maximum = std::numeric_limits<int64_t>::max();
    if ((left > 0 && right > 0 && left > maximum / right) ||
        (left > 0 && right < 0 && right < minimum / left) ||
        (left < 0 && right > 0 && left < minimum / right) ||
        (left < 0 && right < 0 && left < maximum / right))
      throw Failure{"M3001", "Compile-time integer overflow."};
    result = left * right;
  } else if (op == "/" || op == "%") {
    if (right == 0 ||
        (left == std::numeric_limits<int64_t>::min() && right == -1))
      throw Failure{"M3001", "Invalid compile-time division."};
    result = op == "/" ? left / right : left % right;
  } else {
    throw Failure{"M3001", "Unsupported compile-time arithmetic operator."};
  }
  return result;
}

class Evaluator {
public:
  Evaluator(const RootNode &root, const SyntaxMacroRequest &request,
            const CtfeLimits &limits, size_t initialMemory)
      : request_(request), limits_(limits), memoryUsed_(initialMemory) {
    for (const auto &node : root.children) {
      const auto *function = dynamic_cast<const FunDecl *>(node.get());
      if (!function || function->isExtern_ || function->isUnsafe_ ||
          function->isStatic_ || !function->body_ || function->lambdaExpr_ ||
          !function->genericParams_.empty() ||
          !function->genericConstraints_.empty())
        throw Failure{"M3002", "CTFE accepts only ordinary, safe functions."};
      if (!functions_.emplace(function->name_, function).second)
        throw Failure{"M3002", "Duplicate CTFE function name."};
      for (const auto &parameter : function->params_)
        if (parameter->isRef || parameter->isSink || parameter->isVariadic ||
            parameter->defaultValue)
          throw Failure{"M3002", "Unsupported CTFE function parameter."};
    }
  }

  Value run(const std::string &entry) {
    return call(entry, {std::make_shared<const SyntaxValue>(request_.input)});
  }

  void reserveParseMemory(size_t count, size_t bytesPerUnit) {
    chargeParseBudget(limits_, memoryUsed_, count, bytesPerUnit);
  }

private:
  const SyntaxMacroRequest &request_;
  const CtfeLimits &limits_;
  std::map<std::string, const FunDecl *> functions_;
  size_t steps_ = 0;
  size_t memoryUsed_ = 0;
  size_t depth_ = 0;

  void tick() {
    if (steps_ >= limits_.maxSteps)
      throw Failure{"M3003", "CTFE instruction limit exceeded."};
    ++steps_;
  }

  void charge(size_t bytes) {
    if (bytes > limits_.maxMemoryBytes - memoryUsed_)
      throw Failure{"M3003", "CTFE memory limit exceeded."};
    memoryUsed_ += bytes;
  }

  static int64_t integer(const Value &value) {
    if (const auto *number = std::get_if<int64_t>(&value))
      return *number;
    throw Failure{"M3001", "Expected a compile-time Int."};
  }

  static bool boolean(const Value &value) {
    if (const auto *flag = std::get_if<bool>(&value))
      return *flag;
    throw Failure{"M3001", "Expected a compile-time Bool."};
  }

  static const std::string &string(const Value &value) {
    if (const auto *text = std::get_if<std::string>(&value))
      return *text;
    throw Failure{"M3001", "Expected a compile-time String."};
  }

  Value call(const std::string &name, std::vector<Value> arguments) {
    tick();
    if (name == "panic") {
      if (arguments.size() != 1)
        throw Failure{"M3001", "panic expects one String."};
      throw Failure{"M3004", string(arguments.front())};
    }
    if (name == "syntaxExpr" || name == "syntaxItem" ||
        name == "syntaxTokens") {
      if (arguments.size() != 1)
        throw Failure{"M3001", "Syntax constructor expects one String."};
      return constructSyntax(name, string(arguments.front()));
    }
    if (name == "length") {
      if (arguments.size() != 1)
        throw Failure{"M3001", "length expects one String."};
      const auto size = string(arguments.front()).size();
      if (size > static_cast<size_t>(std::numeric_limits<int64_t>::max()))
        throw Failure{"M3003", "CTFE String is too large."};
      return static_cast<int64_t>(size);
    }
    if (name == "charAt") {
      if (arguments.size() != 2)
        throw Failure{"M3001", "charAt expects a String and index."};
      const auto &text = string(arguments[0]);
      const auto index = integer(arguments[1]);
      if (index < 0 || static_cast<uint64_t>(index) >= text.size())
        throw Failure{"M3001", "charAt index is out of range."};
      charge(1);
      return std::string(1, text[static_cast<size_t>(index)]);
    }
    if (name == "slice") {
      if (arguments.size() != 3)
        throw Failure{"M3001", "slice expects a String and two indices."};
      const auto &text = string(arguments[0]);
      const auto begin = integer(arguments[1]);
      const auto end = integer(arguments[2]);
      if (begin < 0 || end < begin || static_cast<uint64_t>(end) > text.size())
        throw Failure{"M3001", "slice indices are out of range."};
      charge(static_cast<size_t>(end - begin));
      return text.substr(static_cast<size_t>(begin),
                         static_cast<size_t>(end - begin));
    }

    const auto found = functions_.find(name);
    if (found == functions_.end())
      throw Failure{"M3002", "Forbidden or unknown CTFE call '" + name + "'."};
    const FunDecl &function = *found->second;
    if (arguments.size() != function.params_.size())
      throw Failure{"M3001", "Wrong CTFE function argument count."};
    if (depth_ >= limits_.maxCallDepth)
      throw Failure{"M3003", "CTFE recursion limit exceeded."};
    ++depth_;
    struct DepthGuard {
      size_t &depth;
      ~DepthGuard() { --depth; }
    } guard{depth_};
    Frame frame;
    for (size_t index = 0; index < arguments.size(); ++index) {
      const auto &parameter = *function.params_[index];
      if (!frame.bindings
               .emplace(parameter.name,
                        Binding{std::move(arguments[index]), false})
               .second)
        throw Failure{"M3002", "Duplicate CTFE parameter name."};
    }
    auto returned = body(*function.body_, frame);
    if (!returned)
      throw Failure{"M3001", "CTFE function did not return a value."};
    return std::move(*returned);
  }

  Value constructSyntax(const std::string &name, const std::string &source) {
    if (source.size() > limits_.maxDefinitionBytes)
      throw Failure{"M3003", "Generated CTFE syntax size limit exceeded."};
    reserveParseMemory(source.size(), 256);
    zap::DiagnosticEngine diagnostics(source, request_.invocation.sourceName);
    Lexer lexer(diagnostics);
    auto tokens = lexer.tokenize(source);
    if (diagnostics.hadErrors() || tokens.size() > limits_.maxDefinitionTokens)
      throw Failure{"M3001", "Invalid or oversized generated syntax."};
    reserveParseMemory(tokens.size(), 256);
    checkSyntaxDepth(tokens, limits_.maxSyntaxDepth);
    if (name != "syntaxTokens") {
      zap::Parser parser(tokens, diagnostics, nullptr, {},
                         MacroParseMode::ValidateFragmentSyntax);
      const auto kind =
          name == "syntaxExpr" ? FragmentKind::Expression : FragmentKind::Item;
      if (!parser.parseFragment(kind) || diagnostics.hadErrors())
        throw Failure{"M3001", "Invalid generated syntax fragment."};
    }
    SyntaxTokens result;
    result.tokens.reserve(tokens.size());
    for (const auto &token : tokens) {
      if (token.sourceFragment)
        throw Failure{"M3002", "Nested source groups in generated syntax are "
                               "not supported by CTFE yet."};
      result.tokens.push_back({static_cast<uint32_t>(token.type), token.value,
                               token.spelling, request_.invocation, 0,
                               nullptr});
      charge(token.value.size() + token.spelling.size() + sizeof(SyntaxToken));
    }
    if (name == "syntaxExpr")
      return std::make_shared<const SyntaxValue>(SyntaxExpr{std::move(result)});
    if (name == "syntaxItem")
      return std::make_shared<const SyntaxValue>(SyntaxItem{std::move(result)});
    return std::make_shared<const SyntaxValue>(std::move(result));
  }

  Value expression(const ExpressionNode &node, Frame &frame) {
    tick();
    if (const auto *literal = dynamic_cast<const ConstInt *>(&node)) {
      int64_t value = 0;
      const auto &text = literal->value_;
      const auto parsed =
          std::from_chars(text.data(), text.data() + text.size(), value);
      if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        throw Failure{"M3001", "Unsupported CTFE integer literal."};
      return value;
    }
    if (const auto *literal = dynamic_cast<const ConstString *>(&node)) {
      charge(literal->value_.size());
      return literal->value_;
    }
    if (const auto *literal = dynamic_cast<const ConstBool *>(&node))
      return literal->value_;
    if (const auto *identifier = dynamic_cast<const ConstId *>(&node)) {
      auto *binding = frame.find(identifier->value_);
      if (!binding)
        throw Failure{"M3002",
                      "Unknown CTFE binding '" + identifier->value_ + "'."};
      if (const auto *text = std::get_if<std::string>(&binding->value))
        charge(text->size());
      return binding->value;
    }
    if (const auto *unary = dynamic_cast<const UnaryExpr *>(&node)) {
      Value operand = expression(*unary->expr_, frame);
      if (unary->op_ == "!")
        return !boolean(operand);
      if (unary->op_ == "-")
        return checkedArithmetic("-", 0, integer(operand));
      if (unary->op_ == "+")
        return integer(operand);
      throw Failure{"M3002", "Unsupported CTFE unary operator."};
    }
    if (const auto *binary = dynamic_cast<const BinExpr *>(&node)) {
      Value left = expression(*binary->left_, frame);
      if (binary->op_ == "&&")
        return boolean(left) && boolean(expression(*binary->right_, frame));
      if (binary->op_ == "||")
        return boolean(left) || boolean(expression(*binary->right_, frame));
      Value right = expression(*binary->right_, frame);
      if (binary->op_ == "==" || binary->op_ == "!=") {
        bool equal = false;
        if (auto a = std::get_if<int64_t>(&left)) {
          if (auto b = std::get_if<int64_t>(&right))
            equal = *a == *b;
          else
            throw Failure{"M3001", "Incompatible CTFE comparison."};
        } else if (auto a = std::get_if<bool>(&left)) {
          if (auto b = std::get_if<bool>(&right))
            equal = *a == *b;
          else
            throw Failure{"M3001", "Incompatible CTFE comparison."};
        } else if (auto a = std::get_if<std::string>(&left)) {
          if (auto b = std::get_if<std::string>(&right))
            equal = *a == *b;
          else
            throw Failure{"M3001", "Incompatible CTFE comparison."};
        } else {
          throw Failure{"M3001", "Unsupported CTFE comparison."};
        }
        return binary->op_ == "==" ? equal : !equal;
      }
      if (auto a = std::get_if<int64_t>(&left)) {
        const auto b = integer(right);
        if (binary->op_ == "<")
          return *a < b;
        if (binary->op_ == "<=")
          return *a <= b;
        if (binary->op_ == ">")
          return *a > b;
        if (binary->op_ == ">=")
          return *a >= b;
        return checkedArithmetic(binary->op_, *a, b);
      }
      if (auto a = std::get_if<std::string>(&left)) {
        if (binary->op_ != "+")
          throw Failure{"M3002", "Unsupported CTFE String operator."};
        const auto &b = string(right);
        charge(a->size() + b.size());
        return *a + b;
      }
      throw Failure{"M3002", "Unsupported CTFE binary operation."};
    }
    if (const auto *ternary = dynamic_cast<const TernaryExpr *>(&node))
      return boolean(expression(*ternary->condition_, frame))
                 ? expression(*ternary->thenExpr_, frame)
                 : expression(*ternary->elseExpr_, frame);
    if (const auto *member = dynamic_cast<const MemberAccessNode *>(&node)) {
      Value base = expression(*member->left_, frame);
      if (const auto *syntax = std::get_if<SyntaxHandle>(&base)) {
        if (const auto *source = std::get_if<SyntaxSource>(syntax->get())) {
          if (member->member_ == "text") {
            charge(source->text.size());
            return source->text;
          }
          if (member->member_ == "interpolationCount")
            return static_cast<int64_t>(source->interpolations.size());
        }
      }
      if (const auto *text = std::get_if<std::string>(&base))
        if (member->member_ == "len")
          return static_cast<int64_t>(text->size());
      throw Failure{"M3002", "Unsupported CTFE member access."};
    }
    if (const auto *callNode = dynamic_cast<const FunCall *>(&node)) {
      const auto *callee =
          dynamic_cast<const ConstId *>(callNode->callee_.get());
      if (!callee || !callNode->genericArgs_.empty())
        throw Failure{"M3002", "Only direct CTFE function calls are allowed."};
      std::vector<Value> arguments;
      arguments.reserve(callNode->params_.size());
      for (const auto &argument : callNode->params_) {
        if (!argument->name.empty() || argument->isRef || argument->isSpread)
          throw Failure{"M3002", "Unsupported CTFE call argument."};
        arguments.push_back(expression(*argument->value, frame));
      }
      return call(callee->value_, std::move(arguments));
    }
    throw Failure{"M3002", "Unsupported CTFE expression."};
  }

  std::optional<Value> statement(const Node &node, Frame &frame) {
    tick();
    if (const auto *binding = dynamic_cast<const BindingDecl *>(&node)) {
      if (!binding->initializer_ || binding->isGlobal_ || binding->isExternal_)
        throw Failure{"M3002", "Unsupported CTFE binding."};
      Value value = expression(*binding->initializer_, frame);
      if (!frame.bindings
               .emplace(binding->name_,
                        Binding{std::move(value),
                                binding->kind_ == BindingKind::Mutable})
               .second)
        throw Failure{"M3001", "Duplicate CTFE binding."};
      return std::nullopt;
    }
    if (const auto *assignment = dynamic_cast<const AssignNode *>(&node)) {
      const auto *target =
          dynamic_cast<const ConstId *>(assignment->target_.get());
      if (!target || !assignment->op_.empty())
        throw Failure{"M3002", "Unsupported CTFE assignment."};
      auto *binding = frame.find(target->value_);
      if (!binding || !binding->mutableValue)
        throw Failure{"M3002", "CTFE assignment requires a mutable local."};
      binding->value = expression(*assignment->expr_, frame);
      return std::nullopt;
    }
    if (const auto *returned = dynamic_cast<const ReturnNode *>(&node)) {
      if (!returned->returnValue)
        return Value{std::monostate{}};
      return expression(*returned->returnValue, frame);
    }
    if (const auto *conditional = dynamic_cast<const IfNode *>(&node)) {
      if (boolean(expression(*conditional->condition_, frame)))
        return body(*conditional->thenBody_, frame);
      if (conditional->elseBody_)
        return body(*conditional->elseBody_, frame);
      return std::nullopt;
    }
    if (const auto *loop = dynamic_cast<const WhileNode *>(&node)) {
      while (true) {
        tick();
        if (!boolean(expression(*loop->condition_, frame)))
          break;
        if (auto returned = body(*loop->body_, frame))
          return returned;
      }
      return std::nullopt;
    }
    if (const auto *callNode = dynamic_cast<const FunCall *>(&node)) {
      (void)expression(*callNode, frame);
      return std::nullopt;
    }
    throw Failure{"M3002", "Unsupported CTFE statement."};
  }

  std::optional<Value> body(const BodyNode &block, Frame &parent) {
    Frame frame{&parent, {}};
    for (const auto &node : block.statements)
      if (auto returned = statement(*node, frame))
        return returned;
    if (block.result)
      return expression(*block.result, frame);
    return std::nullopt;
  }
};

} // namespace

SyntaxMacroResult CtfeInterpreter::execute(std::string_view definitionSource,
                                           std::string_view entryName,
                                           const SyntaxMacroRequest &request,
                                           CtfeLimits limits) {
  SyntaxMacroResult result;
  try {
    if (limits.maxMemoryBytes == 0 || limits.maxSteps == 0 ||
        limits.maxCallDepth == 0 || limits.maxSyntaxDepth == 0 ||
        definitionSource.size() > limits.maxDefinitionBytes)
      throw Failure{"M3003", "CTFE configuration or source limit exceeded."};
    auto encodedRequest = encodeRequest(request);
    if (!std::holds_alternative<std::string>(encodedRequest))
      throw Failure{"M3001", "Invalid CTFE syntax request."};
    const auto &requestBytes = std::get<std::string>(encodedRequest);
    if (definitionSource.size() > limits.maxMemoryBytes ||
        requestBytes.size() > limits.maxMemoryBytes - definitionSource.size())
      throw Failure{"M3003", "CTFE memory limit exceeded."};

    CacheKey key{std::string(definitionSource), std::string(entryName),
                 requestBytes, limits};
    auto cached = cache_.find(key);
    if (cached != cache_.end()) {
      auto decoded = decodeResult(cached->second);
      if (auto *value = std::get_if<SyntaxMacroResult>(&decoded)) {
        ++cacheHits_;
        return *value;
      }
      cacheBytes_ -= key.definitionSource.size() + key.entryName.size() +
                     key.request.size() + cached->second.size();
      cache_.erase(cached);
    }

    size_t memoryUsed = definitionSource.size() + requestBytes.size();
    chargeParseBudget(limits, memoryUsed, definitionSource.size(), 256);
    std::string source(definitionSource);
    zap::DiagnosticEngine diagnostics(source, "<ctfe-definition>");
    Lexer lexer(diagnostics);
    auto tokens = lexer.tokenize(source);
    if (diagnostics.hadErrors() || tokens.size() > limits.maxDefinitionTokens)
      throw Failure{"M3002", "Invalid or oversized CTFE function source."};
    chargeParseBudget(limits, memoryUsed, tokens.size(), 256);
    checkSyntaxDepth(tokens, limits.maxSyntaxDepth);
    zap::Parser parser(std::move(tokens), diagnostics, nullptr, {},
                       MacroParseMode::ValidateFragmentSyntax);
    auto root = parser.parse();
    if (!root || diagnostics.hadErrors() || !parser.macroDefinitions().empty())
      throw Failure{"M3002", "Invalid CTFE function source."};
    Evaluator evaluator(*root, request, limits, memoryUsed);
    Value value = evaluator.run(std::string(entryName));
    if (auto *syntax = std::get_if<SyntaxHandle>(&value))
      result.output = **syntax;
    else
      throw Failure{"M3001", "CTFE macro must return syntax."};

    auto encodedResult = encodeResult(result);
    if (!std::holds_alternative<std::string>(encodedResult))
      throw Failure{"M3001", "CTFE macro returned invalid syntax data."};
    const auto &resultBytes = std::get<std::string>(encodedResult);
    const size_t entryBytes = key.definitionSource.size() +
                              key.entryName.size() + key.request.size() +
                              resultBytes.size();
    if (limits.maxCacheEntries && entryBytes <= limits.maxCacheBytes) {
      if (cacheBytes_ > limits.maxCacheBytes - entryBytes ||
          cache_.size() >= limits.maxCacheEntries) {
        cache_.clear();
        cacheBytes_ = 0;
      }
      cache_.emplace(std::move(key), resultBytes);
      cacheBytes_ += entryBytes;
    }
  } catch (const Failure &failure) {
    result.output.reset();
    result.diagnostics.push_back(
        diagnostic(request, failure.code, failure.message));
  } catch (const std::exception &error) {
    result.output.reset();
    result.diagnostics.push_back(diagnostic(
        request, "M3004", "CTFE failed: " + std::string(error.what())));
  } catch (...) {
    result.output.reset();
    result.diagnostics.push_back(
        diagnostic(request, "M3004", "CTFE failed unexpectedly."));
  }
  return result;
}

} // namespace zap::ctfe
