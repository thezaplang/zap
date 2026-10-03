#include "macros/ctfe_interpreter.hpp"
#include "macros/ctfe_provenance.hpp"
#include "macros/ctfe_runtime.hpp"
#include "macros/ctfe_semantics.hpp"

#include "ast/nodes.hpp"
#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#include "utils/diagnostics.hpp"

#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace zap::ctfe {
namespace {

using Failure = CtfeFailure;
using Value = CtfeValue;

struct Binding {
  Value value;
  bool mutableValue = false;
  ValueType type = ValueType::Void;
};

ValueType valueType(const Value &value) {
  if (std::holds_alternative<std::monostate>(value))
    return ValueType::Void;
  if (std::holds_alternative<bool>(value))
    return ValueType::Bool;
  if (std::holds_alternative<int64_t>(value))
    return ValueType::Int;
  if (std::holds_alternative<std::string>(value))
    return ValueType::String;
  return syntaxValueType(*std::get<SyntaxHandle>(value));
}

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

template <typename TokenEntry>
void checkSyntaxDepth(const std::vector<TokenEntry> &tokens, size_t maximum) {
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

std::unique_ptr<RootNode>
parseDefinition(const std::string &source, zap::DiagnosticEngine &diagnostics,
                const CtfeLimits &limits, size_t &memoryUsed,
                FunctionTypes &types, const SourceSpan *bodySpan = nullptr) {
  if (limits.maxMemoryBytes == 0 || limits.maxSteps == 0 ||
      limits.maxCallDepth == 0 || limits.maxSyntaxDepth == 0 ||
      source.size() > limits.maxDefinitionBytes)
    throw Failure{"M3003", "CTFE configuration or source limit exceeded."};
  chargeParseBudget(limits, memoryUsed, source.size(), 256);
  zap::DiagnosticEngine lexical(source, diagnostics.sourceName());
  Lexer lexer(lexical);
  auto tokens = lexer.tokenize(source);
  const size_t bodyBegin = bodySpan ? source.find('{') : 0;
  auto rebase = [&](SourceSpan span) {
    if (!bodySpan)
      return span;
    if (span.offset < bodyBegin)
      return *bodySpan;
    span.offset = bodySpan->offset + span.offset - bodyBegin;
    if (span.line == 1)
      span.column = bodySpan->column + span.column - bodyBegin - 1;
    span.line += bodySpan->line - 1;
    span.sourceName = bodySpan->sourceName;
    return span;
  };
  for (const auto &diagnostic : lexical.diagnostics())
    diagnostics.report(rebase(diagnostic.span), diagnostic.level,
                       diagnostic.code, diagnostic.message);
  if (lexical.hadErrors())
    return nullptr;
  if (tokens.size() > limits.maxDefinitionTokens)
    throw Failure{"M3003", "CTFE definition token limit exceeded."};
  chargeParseBudget(limits, memoryUsed, tokens.size(), 256);
  checkSyntaxDepth(tokens, limits.maxSyntaxDepth);
  for (auto &token : tokens)
    token.span = rebase(token.span);
  zap::Parser parser(std::move(tokens), diagnostics, nullptr, {},
                     MacroParseMode::ValidateFragmentSyntax);
  auto root = parser.parse();
  if (!parser.macroDefinitions().empty())
    diagnostics.report(root->span, zap::DiagnosticLevel::Error, "M3002",
                       "CTFE does not accept macro declarations.");
  if (!root || diagnostics.hadErrors() ||
      !zap::ctfe::validateDefinition(*root, diagnostics, types))
    return nullptr;
  return root;
}

class Evaluator {
public:
  Evaluator(const RootNode &root, const SyntaxMacroRequest &request,
            const CtfeLimits &limits, size_t initialMemory,
            const FunctionTypes &types)
      : request_(request), limits_(limits), types_(types),
        memoryUsed_(initialMemory) {
    for (const auto &node : root.children) {
      const auto *function = dynamic_cast<const FunDecl *>(node.get());
      functions_.emplace(function->name_, function);
    }
  }

  Value run(const std::string &entry) {
    input_ = std::make_shared<const SyntaxValue>(request_.input);
    return call(entry, {input_});
  }

  bool returnsInput(const SyntaxHandle &value) const { return value == input_; }
  bool observesLocations() const { return observesLocations_; }
  CtfeSyntaxBudget syntaxBudget() {
    return {limits_, memoryUsed_, observesLocations_};
  }
  size_t remainingMemory() const {
    return limits_.maxMemoryBytes - memoryUsed_;
  }

  void reserveParseMemory(size_t count, size_t bytesPerUnit) {
    chargeParseBudget(limits_, memoryUsed_, count, bytesPerUnit);
  }

private:
  SyntaxHandle input_;
  bool observesLocations_ = false;
  const SyntaxMacroRequest &request_;
  const CtfeLimits &limits_;
  const FunctionTypes &types_;
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
    auto builtin = builtinTypes().find(name);
    auto declared = types_.find(name);
    const FunctionType *signature =
        builtin != builtinTypes().end() ? &builtin->second
        : declared != types_.end()      ? &declared->second
                                        : nullptr;
    if (!signature)
      throw Failure{"M3002", "Forbidden or unknown CTFE call '" + name + "'."};
    if (arguments.size() != signature->parameters.size())
      throw Failure{"M3001", "Wrong CTFE function argument count."};
    for (size_t i = 0; i < arguments.size(); ++i)
      if (valueType(arguments[i]) != signature->parameters[i])
        throw Failure{"M3001", "CTFE argument type mismatch: expected " +
                                   typeName(signature->parameters[i]) + "."};
    if (name == "panic") {
      throw Failure{"M3004", string(arguments.front())};
    }
    CtfeSyntaxBudget syntaxBudget{limits_, memoryUsed_, observesLocations_};
    if (auto result =
            evaluateSyntaxBuiltin(name, arguments, request_, syntaxBudget))
      return std::move(*result);
    if (name == "length") {
      const auto size = string(arguments.front()).size();
      if (size > static_cast<size_t>(std::numeric_limits<int64_t>::max()))
        throw Failure{"M3003", "CTFE String is too large."};
      return static_cast<int64_t>(size);
    }
    if (name == "charAt") {
      const auto &text = string(arguments[0]);
      const auto index = integer(arguments[1]);
      if (index < 0 || static_cast<uint64_t>(index) >= text.size())
        throw Failure{"M3001", "charAt index is out of range."};
      charge(1);
      return std::string(1, text[static_cast<size_t>(index)]);
    }
    if (name == "slice") {
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
      charge(sizeof(Binding) + parameter.name.size() + 64);
      if (!frame.bindings
               .emplace(parameter.name,
                        Binding{std::move(arguments[index]), false,
                                signature->parameters[index]})
               .second)
        throw Failure{"M3002", "Duplicate CTFE parameter name."};
    }
    auto returned = body(*function.body_, frame);
    if (!returned && signature->result == ValueType::Void)
      return std::monostate{};
    if (!returned)
      throw Failure{"M3001", "CTFE function did not return a value."};
    if (valueType(*returned) != signature->result)
      throw Failure{"M3001", "CTFE return type mismatch."};
    return std::move(*returned);
  }

  Value expression(const ExpressionNode &node, Frame &frame) {
    tick();
    if (const auto *literal = dynamic_cast<const ConstInt *>(&node)) {
      auto value = integerLiteral(literal->value_);
      if (!value)
        throw Failure{"M3001", "Unsupported CTFE integer literal."};
      return *value;
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
      if (unary->op_ == "-") {
        if (const auto *literal =
                dynamic_cast<const ConstInt *>(unary->expr_.get())) {
          auto value = integerLiteral(literal->value_, true);
          if (!value)
            throw Failure{"M3001", "Unsupported CTFE integer literal."};
          return *value;
        }
      }
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
      auto type = valueType(value);
      charge(sizeof(Binding) + binding->name_.size() + 64);
      if (!frame.bindings
               .emplace(binding->name_,
                        Binding{std::move(value),
                                binding->kind_ == BindingKind::Mutable, type})
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
      Value value = expression(*assignment->expr_, frame);
      if (valueType(value) != binding->type)
        throw Failure{"M3001", "CTFE assignment type mismatch."};
      binding->value = std::move(value);
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
      (void)expression(*block.result, frame);
    return std::nullopt;
  }
};

} // namespace

bool CtfeInterpreter::validateDefinition(const std::string &source,
                                         const SourceSpan &bodySpan,
                                         zap::DiagnosticEngine &diagnostics,
                                         CtfeLimits limits) {
  zap::DiagnosticEngine local(diagnostics.sourceText(),
                              diagnostics.sourceName());
  local.inheritSourcesFrom(diagnostics);
  bool valid = false;
  try {
    size_t memoryUsed = source.size();
    FunctionTypes types;
    valid = static_cast<bool>(
        parseDefinition(source, local, limits, memoryUsed, types, &bodySpan));
  } catch (const Failure &failure) {
    local.report(bodySpan, zap::DiagnosticLevel::Error, failure.code,
                 failure.message);
  }
  for (const auto &diagnostic : local.diagnostics())
    diagnostics.report(diagnostic.span, diagnostic.level, diagnostic.code,
                       diagnostic.message);
  return valid;
}

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
    if (definitionSource.size() > limits.maxMemoryBytes)
      throw Failure{"M3003", "CTFE memory limit exceeded."};
    auto encodedRequest =
        encodeRequest(request, limits.maxMemoryBytes - definitionSource.size());
    if (const auto *error = std::get_if<SyntaxProtocolError>(&encodedRequest))
      throw Failure{*error == SyntaxProtocolError::LimitExceeded ? "M3003"
                                                                 : "M3001",
                    "Invalid or oversized CTFE syntax request."};
    const auto &requestBytes = std::get<std::string>(encodedRequest);
    size_t memoryUsed = definitionSource.size();
    chargeParseBudget(limits, memoryUsed, definitionSource.size(), 2);
    // Encoding, semantic key and owned syntax values must fit before copying.
    chargeParseBudget(limits, memoryUsed, requestBytes.size(), 4);
    chargeParseBudget(limits, memoryUsed, entryName.size(), 2);
    auto semanticRequest = encodeCacheRequest(request, requestBytes.size());
    if (!std::holds_alternative<std::string>(semanticRequest))
      throw Failure{"M3001", "Invalid CTFE cache request."};
    CacheKey key{std::string(definitionSource), std::string(entryName),
                 std::move(std::get<std::string>(semanticRequest)), limits};
    constexpr size_t cacheEntryOverhead =
        sizeof(CacheKey) + sizeof(CacheEntry) + 64;
    auto cached = cache_.find(key);
    auto eraseCached = [&]() {
      cacheBytes_ -= key.definitionSource.size() + key.entryName.size() +
                     key.request.size() + cached->second.ownedBytes() +
                     cacheEntryOverhead;
      cache_.erase(cached);
      cached = cache_.end();
    };
    if (cached != cache_.end() && !cached->second.locationRequest.empty() &&
        cached->second.locationRequest != requestBytes)
      eraseCached();
    if (cached != cache_.end()) {
      if (cached->second.returnsInput) {
        ++cacheHits_;
        result.output = request.input;
        return result;
      }
      chargeParseBudget(limits, memoryUsed,
                        cached->second.generatedResult.size(), 4);
      auto decoded = decodeResult(cached->second.generatedResult);
      if (auto *value = std::get_if<SyntaxMacroResult>(&decoded)) {
        if (value->output) {
          bool ignored = false;
          CtfeSyntaxBudget budget{limits, memoryUsed, ignored};
          restoreSyntaxOutput(*value->output, request,
                              cached->second.provenance, budget);
        }
        auto encoded = encodeResult(*value, limits.maxMemoryBytes - memoryUsed);
        if (!std::holds_alternative<std::string>(encoded))
          throw Failure{
              "M3003",
              "Rebased CTFE result exceeds protocol or memory limits."};
        ++cacheHits_;
        return *value;
      }
      eraseCached();
    }

    std::string source(definitionSource);
    zap::DiagnosticEngine diagnostics(source, "<ctfe-definition>");
    FunctionTypes types;
    auto root = parseDefinition(source, diagnostics, limits, memoryUsed, types);
    if (!root)
      throw Failure{"M3002", diagnostics.empty()
                                 ? "Invalid CTFE function source."
                                 : diagnostics.diagnostics().front().message};
    Evaluator evaluator(*root, request, limits, memoryUsed, types);
    Value value = evaluator.run(std::string(entryName));
    bool returnsInput = false;
    if (auto *syntax = std::get_if<SyntaxHandle>(&value)) {
      returnsInput = evaluator.returnsInput(*syntax);
      const SyntaxTokens *outputTokens =
          std::get_if<SyntaxTokens>(syntax->get());
      if (const auto *expr = std::get_if<SyntaxExpr>(syntax->get()))
        outputTokens = &expr->syntax;
      if (const auto *item = std::get_if<SyntaxItem>(syntax->get()))
        outputTokens = &item->syntax;
      if (outputTokens) {
        if (outputTokens->tokens.size() > limits.maxDefinitionTokens)
          throw Failure{"M3003", "CTFE output token limit exceeded."};
        checkSyntaxDepth(outputTokens->tokens, limits.maxSyntaxDepth);
        evaluator.reserveParseMemory(outputTokens->tokens.size(), 256);
      }
      result.output = **syntax;
    } else
      throw Failure{"M3001", "CTFE macro must return syntax."};

    CacheEntry entry;
    entry.returnsInput = returnsInput;
    if (evaluator.observesLocations()) {
      evaluator.reserveParseMemory(requestBytes.size(), 1);
      entry.locationRequest = requestBytes;
    }
    // The request encoder already validates and budgets input. A passthrough
    // needs no second serialization, on either a cache hit or a miss.
    if (!returnsInput) {
      auto budget = evaluator.syntaxBudget();
      entry.provenance =
          syntaxOutputProvenance(*result.output, request.input, budget);
      auto encodedResult = encodeResult(result, evaluator.remainingMemory());
      if (!std::holds_alternative<std::string>(encodedResult))
        throw Failure{std::get<SyntaxProtocolError>(encodedResult) ==
                              SyntaxProtocolError::LimitExceeded
                          ? "M3003"
                          : "M3001",
                      "CTFE macro returned invalid or oversized syntax data."};
      entry.generatedResult = std::move(std::get<std::string>(encodedResult));
    }
    const size_t entryBytes = key.definitionSource.size() +
                              key.entryName.size() + key.request.size() +
                              entry.ownedBytes() + cacheEntryOverhead;
    if (limits.maxCacheEntries && entryBytes <= limits.maxCacheBytes) {
      if (cacheBytes_ > limits.maxCacheBytes - entryBytes ||
          cache_.size() >= limits.maxCacheEntries) {
        cache_.clear();
        cacheBytes_ = 0;
      }
      cache_.emplace(std::move(key), std::move(entry));
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
