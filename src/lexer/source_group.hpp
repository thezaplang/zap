#pragma once

#include "token/source_fragment.hpp"
#include "token/token_tree.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace zap {
class DiagnosticEngine;

// A per-capture logical allocation budget, shared by nested interpolation
// lexers.
struct SourceCaptureBudget {
  size_t remaining = 4 * 1024 * 1024;
  size_t depth = 0;
  static constexpr size_t MaxDepth = 32;

  bool charge(size_t count, size_t bytesPerEntry = 1) {
    if (count == 0 || bytesPerEntry == 0)
      return true;
    if (count > remaining / bytesPerEntry)
      return false;
    remaining -= count * bytesPerEntry;
    return true;
  }
};

struct SourceGroupCapture {
  std::shared_ptr<SourceFragment> fragment;
  size_t nextOffset = 0;
  size_t closingLine = 0;
  size_t closingColumn = 0;
  size_t nextLine = 0;
  size_t nextColumn = 0;
};

std::optional<SourceGroupCapture>
captureSourceGroup(const std::string &input, size_t openingOffset,
                   size_t openingLine, size_t openingColumn,
                   DiagnosticEngine &diagnostics, SourceCaptureBudget &budget);

// Tokenize only this group, leaving nested source groups opaque.
std::optional<TokenTree> materializeSourceGroup(const TokenTree &group,
                                                DiagnosticEngine &diagnostics);
} // namespace zap
