#pragma once

#include "../token/token.hpp"
#include <cstddef>
#include <vector>

namespace zap {

class TokenCursor {
public:
  // The token collection must outlive the cursor.
  explicit TokenCursor(const std::vector<Token> &tokens);
  TokenCursor(const std::vector<Token> &tokens, size_t begin, size_t end);

  const Token &peek(size_t offset = 0) const noexcept;
  const Token &previous() const noexcept;

  bool isAtEnd() const noexcept;
  size_t position() const noexcept;
  size_t begin() const noexcept;
  size_t end() const noexcept;

  void advance(size_t count = 1) noexcept;

private:
  static Token makeEndToken(const std::vector<Token> &tokens, size_t end);

  const std::vector<Token> &_tokens;
  size_t _begin;
  size_t _end;
  size_t _position;
  Token _endToken;
};

} // namespace zap
