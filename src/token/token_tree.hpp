#pragma once

#include "../utils/diagnostics.hpp"
#include "token.hpp"

#include <optional>
#include <vector>

enum class Delimiter { Parenthesis, Bracket, Brace };

class TokenTree {
public:
  static TokenTree leaf(Token token);
  static TokenTree group(Delimiter delimiter, Token opening,
                         std::vector<TokenTree> children,
                         std::optional<Token> closing);

  bool isLeaf() const noexcept;
  const Token &token() const;
  Delimiter delimiter() const;
  const Token &opening() const;
  const std::optional<Token> &closing() const;
  const std::vector<TokenTree> &children() const;
  SourceSpan span() const;
  size_t tokenCount() const noexcept;

private:
  explicit TokenTree(Token token);
  TokenTree(Delimiter delimiter, Token opening, std::vector<TokenTree> children,
            std::optional<Token> closing);

  std::optional<Token> token_;
  Delimiter delimiter_ = Delimiter::Parenthesis;
  std::optional<Token> opening_;
  std::optional<Token> closing_;
  std::vector<TokenTree> children_;
};

struct TokenTreeResult {
  std::vector<TokenTree> trees;
  bool hadDelimiterErrors = false;
  size_t nextPosition = 0;
};

class TokenTreeBuilder {
public:
  static TokenTreeResult build(const std::vector<Token> &tokens,
                               zap::DiagnosticEngine &diagnostics);
  static TokenTreeResult buildPrefix(const std::vector<Token> &tokens,
                                     size_t begin, size_t end,
                                     zap::DiagnosticEngine &diagnostics);
};

std::vector<Token> flattenTokenTrees(const std::vector<TokenTree> &trees);
