#pragma once

#include "token/source_fragment.hpp"
#include "token/token_tree.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace zap {
class DiagnosticEngine;

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
                   DiagnosticEngine &diagnostics);

// Tokenize only this group, leaving nested source groups opaque.
std::optional<TokenTree> materializeSourceGroup(const TokenTree &group,
                                                DiagnosticEngine &diagnostics);
} // namespace zap
