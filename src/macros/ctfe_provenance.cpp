#include "macros/ctfe_provenance.hpp"

#include <map>

namespace zap::ctfe {
namespace {

SyntaxTokens *outputTokens(SyntaxValue &value) {
  if (auto *tokens = std::get_if<SyntaxTokens>(&value))
    return tokens;
  if (auto *expr = std::get_if<SyntaxExpr>(&value))
    return &expr->syntax;
  if (auto *item = std::get_if<SyntaxItem>(&value))
    return &item->syntax;
  return nullptr;
}
const SyntaxTokens *outputTokens(const SyntaxValue &value) {
  if (const auto *tokens = std::get_if<SyntaxTokens>(&value))
    return tokens;
  if (const auto *expr = std::get_if<SyntaxExpr>(&value))
    return &expr->syntax;
  if (const auto *item = std::get_if<SyntaxItem>(&value))
    return &item->syntax;
  return nullptr;
}

void collectSource(const SyntaxSource &source,
                   std::vector<const SyntaxToken *> &output,
                   CtfeSyntaxBudget &budget, size_t depth);

void collectTokens(const SyntaxTokens &tokens,
                   std::vector<const SyntaxToken *> &output,
                   CtfeSyntaxBudget &budget, size_t depth) {
  if (depth > MaxSyntaxNesting)
    throw CtfeFailure{"M3003", "Syntax provenance nesting limit exceeded."};
  for (const auto &token : tokens.tokens) {
    if (output.size() >= MaxSyntaxEntries)
      throw CtfeFailure{"M3003", "Syntax provenance entry limit exceeded."};
    budget.reserve(2, sizeof(const SyntaxToken *));
    output.push_back(&token);
    if (token.sourceFragment)
      collectSource(*token.sourceFragment, output, budget, depth + 1);
  }
}

void collectSource(const SyntaxSource &source,
                   std::vector<const SyntaxToken *> &output,
                   CtfeSyntaxBudget &budget, size_t depth) {
  for (const auto &interpolation : source.interpolations)
    collectTokens(interpolation.expression, output, budget, depth);
}

std::vector<const SyntaxToken *> capturedTokens(const SyntaxValue &value,
                                                CtfeSyntaxBudget &budget) {
  std::vector<const SyntaxToken *> tokens;
  if (const auto *source = std::get_if<SyntaxSource>(&value))
    collectSource(*source, tokens, budget, 0);
  else
    collectTokens(*outputTokens(value), tokens, budget, 0);
  return tokens;
}

} // namespace

std::vector<uint32_t> syntaxOutputProvenance(const SyntaxValue &output,
                                             const SyntaxValue &input,
                                             CtfeSyntaxBudget &budget) {
  const auto *tokens = outputTokens(output);
  if (!tokens)
    throw CtfeFailure{"M3001", "Only whole-input source values can be cached."};
  auto captured = capturedTokens(input, budget);
  std::map<SyntaxTokenIdentity, uint32_t> identities;
  budget.reserve(captured.size(),
                 sizeof(SyntaxTokenIdentity) + sizeof(uint32_t) + 64);
  for (size_t i = 0; i < captured.size(); ++i)
    identities.emplace(syntaxTokenIdentity(*captured[i]),
                       static_cast<uint32_t>(i));
  budget.reserve(tokens->tokens.size(), sizeof(uint32_t));
  std::vector<uint32_t> provenance;
  provenance.reserve(tokens->tokens.size());
  for (const auto &token : tokens->tokens) {
    auto found = identities.find(syntaxTokenIdentity(token));
    if (found != identities.end())
      provenance.push_back(found->second);
    else if (token.context == GeneratedSyntaxContext && !token.sourceFragment)
      provenance.push_back(GeneratedTokenIndex);
    else
      throw CtfeFailure{"M3001", "Syntax output lost capture provenance."};
  }
  return provenance;
}

void restoreSyntaxOutput(SyntaxValue &output, const SyntaxMacroRequest &request,
                         const std::vector<uint32_t> &provenance,
                         CtfeSyntaxBudget &budget) {
  auto *tokens = outputTokens(output);
  if (!tokens || tokens->tokens.size() != provenance.size())
    throw CtfeFailure{"M3001", "Invalid cached syntax provenance."};
  auto captured = capturedTokens(request.input, budget);
  for (size_t i = 0; i < provenance.size(); ++i) {
    const auto ordinal = provenance[i];
    if (ordinal == GeneratedTokenIndex) {
      budget.reserve(request.invocation.sourceName.size(), 3);
      tokens->tokens[i].span = request.invocation;
    } else {
      if (ordinal >= captured.size())
        throw CtfeFailure{"M3001", "Invalid cached capture ordinal."};
      const auto &token = *captured[ordinal];
      budget.reserve(sizeof(SyntaxToken) + token.value.size() +
                         token.spelling.size() + token.span.sourceName.size(),
                     3);
      tokens->tokens[i] = token;
    }
  }
}

} // namespace zap::ctfe
