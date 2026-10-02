#include "macros/syntax_bridge.hpp"

#include <map>
#include <string_view>
#include <tuple>
#include <utility>

namespace zap::ctfe_bridge {

ctfe::SyntaxSpan syntaxSpan(const SourceSpan &span) {
  return {span.sourceName, span.line, span.column, span.offset, span.length};
}

SourceSpan sourceSpan(const ctfe::SyntaxSpan &span) {
  return {static_cast<size_t>(span.line), static_cast<size_t>(span.column),
          static_cast<size_t>(span.offset), static_cast<size_t>(span.length),
          span.sourceName};
}

namespace {

using CapturedTokenKey =
    std::tuple<uint32_t, std::string_view, std::string_view, uint32_t,
               std::string_view, uint64_t, uint64_t, uint64_t, uint64_t>;

CapturedTokenKey tokenKey(const ctfe::SyntaxToken &token) {
  return {token.type,        token.value,           token.spelling,
          token.context,     token.span.sourceName, token.span.line,
          token.span.column, token.span.offset,     token.span.length};
}

CapturedTokenKey tokenKey(const Token &token) {
  return {static_cast<uint32_t>(token.type),
          token.value,
          token.spelling,
          token.syntaxContext,
          token.span.sourceName,
          token.span.line,
          token.span.column,
          token.span.offset,
          token.span.length};
}

struct ConversionBudget {
  size_t remaining;

  bool charge(size_t count, size_t bytesPerEntry = 1) {
    if (count > remaining / bytesPerEntry)
      return false;
    remaining -= count * bytesPerEntry;
    return true;
  }
};

std::optional<ctfe::SyntaxSource> convertSource(const SourceFragment &fragment,
                                                size_t depth,
                                                ConversionBudget &budget,
                                                SyntaxInput &input);

bool appendToken(const Token &token, ctfe::SyntaxTokens &output, size_t depth,
                 ConversionBudget &budget, SyntaxInput &input) {
  if (token.type == TokenType::EOF_TOKEN)
    return true;
  // Reserve growing vectors and the provenance map built after CTFE execution.
  constexpr size_t entryBytes =
      3 * sizeof(ctfe::SyntaxToken) + 3 * sizeof(const Token *) +
      sizeof(CapturedTokenKey) + sizeof(const Token *) + 64;
  if (output.tokens.size() >= ctfe::MaxSyntaxEntries ||
      !budget.charge(entryBytes) || !budget.charge(token.value.size()) ||
      !budget.charge(token.spelling.size()) ||
      !budget.charge(token.span.sourceName.size()))
    return false;
  input.capturedTokens.push_back(&token);
  output.tokens.push_back({static_cast<uint32_t>(token.type), token.value,
                           token.spelling, syntaxSpan(token.span),
                           token.syntaxContext, nullptr});
  if (token.sourceFragment) {
    auto source =
        convertSource(*token.sourceFragment, depth + 1, budget, input);
    if (!source)
      return false;
    output.tokens.back().sourceFragment =
        std::make_shared<const ctfe::SyntaxSource>(std::move(*source));
  }
  return true;
}

std::optional<ctfe::SyntaxTokens>
convertTokens(const std::vector<Token> &tokens, size_t depth,
              ConversionBudget &budget, SyntaxInput &input) {
  if (depth > ctfe::MaxSyntaxNesting || tokens.size() > ctfe::MaxSyntaxEntries)
    return std::nullopt;
  ctfe::SyntaxTokens output;
  for (const auto &token : tokens)
    if (!appendToken(token, output, depth, budget, input))
      return std::nullopt;
  return output;
}

bool convertTrees(const std::vector<TokenTree> &trees,
                  ctfe::SyntaxTokens &output, size_t treeDepth,
                  ConversionBudget &budget, SyntaxInput &input) {
  if (treeDepth > 64)
    return false;
  for (const auto &tree : trees) {
    if (tree.isLeaf()) {
      if (!appendToken(tree.token(), output, 0, budget, input))
        return false;
    } else {
      if (!tree.closing() ||
          !appendToken(tree.opening(), output, 0, budget, input) ||
          !convertTrees(tree.children(), output, treeDepth + 1, budget,
                        input) ||
          !appendToken(*tree.closing(), output, 0, budget, input))
        return false;
    }
  }
  return true;
}

std::optional<ctfe::SyntaxSource> convertSource(const SourceFragment &fragment,
                                                size_t depth,
                                                ConversionBudget &budget,
                                                SyntaxInput &input) {
  if (depth > ctfe::MaxSyntaxNesting ||
      fragment.offsetMap.size() > ctfe::MaxSyntaxEntries ||
      fragment.interpolations.size() > ctfe::MaxSyntaxEntries ||
      !budget.charge(sizeof(ctfe::SyntaxSource) + 64) ||
      !budget.charge(fragment.text.size()) ||
      !budget.charge(fragment.sourceName.size()) ||
      !budget.charge(fragment.offsetMap.size(), sizeof(ctfe::SyntaxOffset)) ||
      !budget.charge(fragment.interpolations.size(),
                     sizeof(ctfe::SyntaxInterpolation)))
    return std::nullopt;
  ctfe::SyntaxSource output;
  output.text = fragment.text;
  output.sourceName = fragment.sourceName;
  output.offsets.reserve(fragment.offsetMap.size());
  output.interpolations.reserve(fragment.interpolations.size());
  for (const auto &offset : fragment.offsetMap)
    output.offsets.push_back({offset.line, offset.column, offset.offset});
  for (const auto &interpolation : fragment.interpolations) {
    if (!budget.charge(interpolation.span.sourceName.size()))
      return std::nullopt;
    auto tokens = convertTokens(interpolation.tokens, depth + 1, budget, input);
    if (!tokens)
      return std::nullopt;
    output.interpolations.push_back(
        {syntaxSpan(interpolation.span), interpolation.bodyBegin,
         interpolation.bodyEnd, std::move(*tokens)});
  }
  return output;
}

} // namespace

std::optional<SyntaxInput> syntaxTokens(const std::vector<TokenTree> &trees,
                                        size_t maxBytes) {
  ConversionBudget budget{maxBytes};
  SyntaxInput input;
  ctfe::SyntaxTokens output;
  if (!convertTrees(trees, output, 0, budget, input))
    return std::nullopt;
  input.value = std::move(output);
  return input;
}

std::optional<SyntaxInput> syntaxSource(const SourceFragment &fragment,
                                        size_t maxBytes) {
  ConversionBudget budget{maxBytes};
  SyntaxInput input;
  auto output = convertSource(fragment, 0, budget, input);
  if (!output)
    return std::nullopt;
  input.value = std::move(*output);
  return input;
}

std::optional<std::vector<Token>>
compilerTokens(const ctfe::SyntaxTokens &syntax, const SourceSpan &invocation,
               const std::shared_ptr<const ExpansionOrigin> &origin,
               const SyntaxInput &input) {
  std::map<CapturedTokenKey, const Token *> captured;
  for (const auto *token : input.capturedTokens) {
    if (!token)
      return std::nullopt;
    captured.emplace(tokenKey(*token), token);
  }
  std::vector<Token> output;
  output.reserve(syntax.tokens.size());
  for (const auto &item : syntax.tokens) {
    if (item.type < static_cast<uint32_t>(TokenType::IMPORT) ||
        item.type > static_cast<uint32_t>(TokenType::DOLLAR) ||
        item.type == static_cast<uint32_t>(TokenType::EOF_TOKEN))
      return std::nullopt;
    if (item.context != ctfe::GeneratedSyntaxContext) {
      const auto found = captured.find(tokenKey(item));
      if (found == captured.end())
        return std::nullopt;
      // Compiler-owned origins stay outside the wire protocol and the cache.
      output.push_back(*found->second);
    } else {
      if (item.sourceFragment)
        return std::nullopt;
      output.emplace_back(static_cast<TokenType>(item.type), item.value,
                          invocation, item.spelling, origin->mark, origin);
    }
  }
  return output;
}

} // namespace zap::ctfe_bridge
