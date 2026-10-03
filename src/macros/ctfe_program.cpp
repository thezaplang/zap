#include "macros/ctfe_program.hpp"

#include "ast/fun_decl.hpp"
#include "lexer/lexer.hpp"
#include "macros/ctfe_runtime.hpp"
#include "parser/parser.hpp"
#include "utils/diagnostics.hpp"

#include <algorithm>
#include <tuple>

namespace zap::ctfe {

CtfeProgramBuilder::CtfeProgramBuilder(DiagnosticEngine &diagnostics,
                                       CtfeLimits limits)
    : diagnostics_(diagnostics), limits_(limits),
      program_(std::make_shared<CtfeProgram>()) {}

void CtfeProgramBuilder::reserveSource(const std::string &source) {
  if (!limits_.maxMemoryBytes || !limits_.maxSteps || !limits_.maxCallDepth ||
      !limits_.maxSyntaxDepth ||
      source.size() > limits_.maxDefinitionBytes - program_->sourceBytes_)
    throw CtfeFailure{"M3003", "CTFE configuration or source limit exceeded."};
  program_->sourceBytes_ += source.size();
  chargeParseBudget(limits_, program_->memoryUsed_, source.size(), 256);
  program_->cacheKey_ += "S" + std::to_string(source.size()) + ":" + source;
}

const RootNode *CtfeProgramBuilder::parse(std::vector<Token> tokens) {
  if (tokens.size() > limits_.maxDefinitionTokens - program_->tokenCount_)
    throw CtfeFailure{"M3003", "CTFE definition token limit exceeded."};
  program_->tokenCount_ += tokens.size();
  chargeParseBudget(limits_, program_->memoryUsed_, tokens.size(), 256);
  size_t depth = 0;
  for (const auto &token : tokens) {
    if (token.type == TokenType::LPAREN || token.type == TokenType::LBRACE ||
        token.type == TokenType::SQUARE_LBRACE) {
      if (depth >= limits_.maxSyntaxDepth)
        throw CtfeFailure{"M3003", "CTFE syntax nesting limit exceeded."};
      ++depth;
      program_->syntaxDepth_ = std::max(program_->syntaxDepth_, depth);
    } else if (token.type == TokenType::RPAREN ||
               token.type == TokenType::RBRACE ||
               token.type == TokenType::SQUARE_RBRACE) {
      if (depth)
        --depth;
    }
  }
  Parser parser(std::move(tokens), diagnostics_, nullptr, {},
                MacroParseMode::ValidateFragmentSyntax);
  auto root = parser.parse();
  if (!parser.macroDefinitions().empty())
    diagnostics_.report(root->span, DiagnosticLevel::Error, "M3002",
                        "CTFE does not accept macro declarations.");
  if (!root || diagnostics_.hadErrors())
    return nullptr;
  const auto *result = root.get();
  program_->roots_.push_back(std::move(root));
  return result;
}

const RootNode *CtfeProgramBuilder::addSource(const std::string &source,
                                              const SourceSpan *bodySpan) {
  reserveSource(source);
  DiagnosticEngine lexical(source, diagnostics_.sourceName());
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
    diagnostics_.report(rebase(diagnostic.span), diagnostic.level,
                        diagnostic.code, diagnostic.message);
  if (lexical.hadErrors())
    return nullptr;
  for (auto &token : tokens)
    token.span = rebase(token.span);
  return parse(std::move(tokens));
}

const FunDecl *CtfeProgramBuilder::addFunction(const std::vector<Token> &tokens,
                                               const std::string &source,
                                               const std::string &identity) {
  reserveSource(source);
  chargeParseBudget(limits_, program_->memoryUsed_, identity.size(), 2);
  program_->cacheKey_ += "I" + std::to_string(identity.size()) + ":" + identity;
  // Account for the copy before allocating the parser's token storage.
  if (tokens.size() > limits_.maxDefinitionTokens - program_->tokenCount_)
    throw CtfeFailure{"M3003", "CTFE definition token limit exceeded."};
  chargeParseBudget(limits_, program_->memoryUsed_, tokens.size(),
                    sizeof(Token));
  const auto *root = parse(tokens);
  if (!root || root->children.size() != 1)
    return nullptr;
  return dynamic_cast<const FunDecl *>(root->children.front().get());
}

std::shared_ptr<const CtfeProgram>
CtfeProgramBuilder::finish(const std::vector<FunctionDefinition> &entries,
                           const FunctionLookup &lookup) {
  auto owned = [&](const FunDecl *function) {
    for (const auto &root : program_->roots_)
      for (const auto &node : root->children)
        if (node.get() == function)
          return true;
    return false;
  };
  for (const auto &entry : entries)
    if (!owned(entry.declaration))
      throw CtfeFailure{"M3002", "CTFE entry is not owned by this program."};
  auto checkedLookup = [&](const FunDecl &caller,
                           const ExpressionNode &callee) {
    auto target = lookup(caller, callee);
    if (target && !owned(target->declaration))
      throw CtfeFailure{"M3002", "CTFE helper is not owned by this program."};
    return target;
  };
  if (!validateFunctions(entries, checkedLookup, diagnostics_,
                         program_->functions_) ||
      diagnostics_.hadErrors())
    return nullptr;
  for (const auto &[id, function] : program_->functions_.declarations) {
    chargeParseBudget(limits_, program_->memoryUsed_, id.size(), 4);
    program_->cacheKey_ += "F" + std::to_string(id.size()) + ":" + id;
    (void)function;
  }
  using CallIdentity = std::tuple<std::string, size_t, std::string>;
  chargeParseBudget(limits_, program_->memoryUsed_,
                    program_->functions_.calls.size(),
                    sizeof(CallIdentity) + 128);
  std::vector<CallIdentity> calls;
  for (const auto &[call, target] : program_->functions_.calls) {
    chargeParseBudget(limits_, program_->memoryUsed_,
                      call->span.sourceName.size() + target.size(), 4);
    calls.emplace_back(call->span.sourceName, call->span.offset, target);
  }
  std::sort(calls.begin(), calls.end());
  for (const auto &[sourceName, offset, target] : calls) {
    program_->cacheKey_ += "C" + std::to_string(sourceName.size()) + ":" +
                           sourceName + ":" + std::to_string(offset) + ":";
    program_->cacheKey_ += std::to_string(target.size()) + ":" + target;
  }
  return std::move(program_);
}

} // namespace zap::ctfe
