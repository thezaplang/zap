#pragma once

#include "lsp/lsp.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace zap::lsp {

struct TextDocumentPosition {
  std::string uri;
  int64_t line = 0;
  int64_t character = 0;
};

struct OpenDocumentParams {
  std::string uri;
  std::string text;
  int64_t version = 0;
};

struct ChangeDocumentParams {
  std::string uri;
  std::string text;
  int64_t version = 0;
};

struct WatchedFileChange {
  std::string uri;
};

std::optional<TextDocumentPosition>
decodeTextDocumentPosition(const JsonObject &request);
std::optional<OpenDocumentParams> decodeOpenDocument(const JsonObject &request);
std::optional<ChangeDocumentParams>
decodeChangeDocument(const JsonObject &request);
std::optional<std::string> decodeCloseDocument(const JsonObject &request);
std::optional<std::vector<WatchedFileChange>>
decodeWatchedFiles(const JsonObject &request);

} // namespace zap::lsp
