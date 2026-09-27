#include "token_cursor.hpp"

#include <algorithm>
#include <utility>

namespace zap {

TokenCursor::TokenCursor(const std::vector<Token> &tokens)
    : TokenCursor(tokens, 0, tokens.size()) {}

TokenCursor::TokenCursor(const std::vector<Token> &tokens, size_t begin,
                         size_t end)
    : _tokens(tokens), _begin(std::min(begin, tokens.size())),
      _end(std::min(std::max(begin, end), tokens.size())), _position(_begin),
      _endToken(makeEndToken(tokens, _end)) {}

const Token &TokenCursor::peek(size_t offset) const noexcept {
  if (offset > _end - _position) {
    return _endToken;
  }

  const size_t index = _position + offset;
  if (index >= _end) {
    return _endToken;
  }
  return _tokens[index];
}

const Token &TokenCursor::previous() const noexcept {
  if (_position == _begin) {
    return _endToken;
  }
  return _tokens[_position - 1];
}

bool TokenCursor::isAtEnd() const noexcept { return _position >= _end; }

size_t TokenCursor::position() const noexcept { return _position; }

size_t TokenCursor::begin() const noexcept { return _begin; }

size_t TokenCursor::end() const noexcept { return _end; }

void TokenCursor::advance(size_t count) noexcept {
  _position += std::min(count, _end - _position);
}

Token TokenCursor::makeEndToken(const std::vector<Token> &tokens, size_t end) {
  if (end < tokens.size()) {
    const SourceSpan &next = tokens[end].span;
    SourceSpan span(next.line, next.column, next.offset, 0, next.sourceName);
    span.expansionOrigin = next.expansionOrigin;
    return Token(TokenType::EOF_TOKEN, "", std::move(span));
  }

  if (end == 0 || tokens.empty()) {
    return Token(TokenType::EOF_TOKEN, "", SourceSpan());
  }

  const SourceSpan &last = tokens[end - 1].span;
  SourceSpan span(0, 0, last.offset + last.length, 0, last.sourceName);
  span.expansionOrigin = last.expansionOrigin;
  return Token(TokenType::EOF_TOKEN, "", std::move(span));
}

} // namespace zap
