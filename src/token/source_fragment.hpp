#pragma once

#include "token/token.hpp"

#include <string>
#include <vector>

struct SourceOffset {
  size_t line = 0;
  size_t column = 0;
  size_t offset = 0;
};

struct SourceInterpolation {
  SourceSpan span;
  size_t bodyBegin = 0;
  size_t bodyEnd = 0;
  std::vector<Token> tokens;
};

struct SourceFragment {
  std::string text;
  std::string sourceName;
  // One entry per source byte, plus the position immediately after the text.
  std::vector<SourceOffset> offsetMap;
  std::vector<SourceInterpolation> interpolations;

  SourceSpan spanAt(size_t begin, size_t length) const {
    const auto &position = offsetMap.at(begin);
    return SourceSpan(position.line, position.column, position.offset, length,
                      sourceName);
  }
};
