#include "token_tree.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace {

std::optional<Delimiter> delimiterForOpening(TokenType type) {
  switch (type) {
  case TokenType::LPAREN:
    return Delimiter::Parenthesis;
  case TokenType::SQUARE_LBRACE:
    return Delimiter::Bracket;
  case TokenType::LBRACE:
    return Delimiter::Brace;
  default:
    return std::nullopt;
  }
}

bool isClosingDelimiter(TokenType type) {
  return type == TokenType::RPAREN || type == TokenType::SQUARE_RBRACE ||
         type == TokenType::RBRACE;
}

TokenType closingTokenFor(Delimiter delimiter) {
  switch (delimiter) {
  case Delimiter::Parenthesis:
    return TokenType::RPAREN;
  case Delimiter::Bracket:
    return TokenType::SQUARE_RBRACE;
  case Delimiter::Brace:
    return TokenType::RBRACE;
  }
  return TokenType::EOF_TOKEN;
}

class TreeParser {
public:
  TreeParser(const std::vector<Token> &tokens, size_t begin, size_t end,
             zap::DiagnosticEngine &diagnostics)
      : tokens_(tokens), diagnostics_(diagnostics), position_(begin), end_(end) {}

  TokenTreeResult parse(bool firstOnly = false) {
    TokenTreeResult result;
    result.trees = parseSequence(false, firstOnly);
    result.hadDelimiterErrors = hadDelimiterErrors_;
    result.nextPosition = position_;
    return result;
  }

private:
  std::vector<TokenTree> parseSequence(bool stopAtClosingDelimiter,
                                        bool firstOnly = false) {
    std::vector<TokenTree> trees;

    while (position_ < end_) {
      const Token &current = tokens_[position_];
      if (const auto delimiter = delimiterForOpening(current.type)) {
        Token opening = current;
        ++position_;
        openDelimiters_.push_back(*delimiter);
        auto children = parseSequence(true);
        std::optional<Token> closing;
        if (position_ < end_ &&
            isClosingDelimiter(tokens_[position_].type)) {
          const Token &candidate = tokens_[position_];
          if (candidate.type == closingTokenFor(*delimiter)) {
            closing = candidate;
            ++position_;
          } else if (matchesAncestor(candidate.type)) {
            report(opening.span,
                   "Unterminated delimiter group; expected '" +
                       tokenTypeToString(closingTokenFor(*delimiter)) + "'.");
          } else {
            closing = candidate;
            ++position_;
            report(closing->span,
                   "Mismatched closing delimiter '" + closing->spelling +
                       "'; expected '" +
                       tokenTypeToString(closingTokenFor(*delimiter)) + "'.");
          }
        } else {
          report(opening.span,
                 "Unterminated delimiter group; expected '" +
                     tokenTypeToString(closingTokenFor(*delimiter)) + "'.");
        }
        openDelimiters_.pop_back();
        trees.push_back(TokenTree::group(*delimiter, std::move(opening),
                                         std::move(children),
                                         std::move(closing)));
        if (firstOnly) {
          return trees;
        }
        continue;
      }

      if (isClosingDelimiter(current.type)) {
        if (stopAtClosingDelimiter) {
          return trees;
        }
        report(current.span,
               "Unexpected closing delimiter '" + current.spelling + "'.");
      }

      trees.push_back(TokenTree::leaf(current));
      ++position_;
      if (firstOnly) {
        return trees;
      }
    }

    return trees;
  }

  bool matchesAncestor(TokenType closing) const {
    for (size_t index = 0; index + 1 < openDelimiters_.size(); ++index) {
      if (closingTokenFor(openDelimiters_[index]) == closing) {
        return true;
      }
    }
    return false;
  }

  void report(const SourceSpan &span, const std::string &message) {
    diagnostics_.report(span, zap::DiagnosticLevel::Error, message);
    hadDelimiterErrors_ = true;
  }

  const std::vector<Token> &tokens_;
  zap::DiagnosticEngine &diagnostics_;
  size_t position_ = 0;
  size_t end_;
  std::vector<Delimiter> openDelimiters_;
  bool hadDelimiterErrors_ = false;
};

void appendFlattened(const TokenTree &tree, std::vector<Token> &tokens) {
  if (tree.isLeaf()) {
    tokens.push_back(tree.token());
    return;
  }

  tokens.push_back(tree.opening());
  for (const TokenTree &child : tree.children()) {
    appendFlattened(child, tokens);
  }
  if (tree.closing()) {
    tokens.push_back(*tree.closing());
  }
}

} // namespace

TokenTree::TokenTree(Token token) : token_(std::move(token)) {}

TokenTree::TokenTree(Delimiter delimiter, Token opening,
                     std::vector<TokenTree> children,
                     std::optional<Token> closing)
    : delimiter_(delimiter), opening_(std::move(opening)),
      closing_(std::move(closing)), children_(std::move(children)) {}

TokenTree TokenTree::leaf(Token token) { return TokenTree(std::move(token)); }

TokenTree TokenTree::group(Delimiter delimiter, Token opening,
                           std::vector<TokenTree> children,
                           std::optional<Token> closing) {
  return TokenTree(delimiter, std::move(opening), std::move(children),
                   std::move(closing));
}

bool TokenTree::isLeaf() const noexcept { return token_.has_value(); }

const Token &TokenTree::token() const {
  if (!token_) {
    throw std::logic_error("token tree group has no leaf token");
  }
  return *token_;
}

Delimiter TokenTree::delimiter() const {
  if (isLeaf()) {
    throw std::logic_error("token tree leaf has no delimiter");
  }
  return delimiter_;
}

const Token &TokenTree::opening() const {
  if (!opening_) {
    throw std::logic_error("token tree leaf has no opening delimiter");
  }
  return *opening_;
}

const std::optional<Token> &TokenTree::closing() const {
  if (isLeaf()) {
    throw std::logic_error("token tree leaf has no closing delimiter");
  }
  return closing_;
}

const std::vector<TokenTree> &TokenTree::children() const {
  if (isLeaf()) {
    throw std::logic_error("token tree leaf has no children");
  }
  return children_;
}

SourceSpan TokenTree::span() const {
  if (isLeaf()) {
    return token().span;
  }
  if (closing_) {
    return SourceSpan::merge(opening().span, closing_->span);
  }
  if (!children_.empty()) {
    return SourceSpan::merge(opening().span, children_.back().span());
  }
  return opening().span;
}

TokenTreeResult TokenTreeBuilder::build(
    const std::vector<Token> &tokens, zap::DiagnosticEngine &diagnostics) {
  return TreeParser(tokens, 0, tokens.size(), diagnostics).parse();
}

TokenTreeResult TokenTreeBuilder::buildPrefix(
    const std::vector<Token> &tokens, size_t begin, size_t end,
    zap::DiagnosticEngine &diagnostics) {
  const size_t clampedBegin = std::min(begin, tokens.size());
  const size_t clampedEnd = std::min(std::max(clampedBegin, end), tokens.size());
  return TreeParser(tokens, clampedBegin, clampedEnd, diagnostics).parse(true);
}

std::vector<Token> flattenTokenTrees(const std::vector<TokenTree> &trees) {
  std::vector<Token> tokens;
  for (const TokenTree &tree : trees) {
    appendFlattened(tree, tokens);
  }
  return tokens;
}
