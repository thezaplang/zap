#include "lsp/protocol_codec.hpp"

#include "lsp/protocol_utils.hpp"

namespace zap::lsp {

std::optional<TextDocumentPosition>
decodeTextDocumentPosition(const JsonObject &request) {
  auto uri = getStringField(request, {"params", "textDocument", "uri"});
  auto line = getIntegerField(request, {"params", "position", "line"});
  auto character =
      getIntegerField(request, {"params", "position", "character"});
  if (!uri || !line || !character || *line < 0 || *character < 0) {
    return std::nullopt;
  }
  return TextDocumentPosition{std::move(*uri), *line, *character};
}

std::optional<OpenDocumentParams> decodeOpenDocument(const JsonObject &request) {
  auto uri = getStringField(request, {"params", "textDocument", "uri"});
  auto text = getStringField(request, {"params", "textDocument", "text"});
  auto version = getIntegerField(request, {"params", "textDocument", "version"});
  if (!uri || !text || !version) {
    return std::nullopt;
  }
  return OpenDocumentParams{std::move(*uri), std::move(*text), *version};
}

std::optional<ChangeDocumentParams>
decodeChangeDocument(const JsonObject &request) {
  auto uri = getStringField(request, {"params", "textDocument", "uri"});
  auto version = getIntegerField(request, {"params", "textDocument", "version"});
  const JsonObject *changes = getPath(request, {"params", "contentChanges"});
  if (!uri || !version || !changes || !changes->isList() ||
      changes->getAsList().empty()) {
    return std::nullopt;
  }
  auto text = getStringField(changes->getAsList().back(), {"text"});
  if (!text) {
    return std::nullopt;
  }
  return ChangeDocumentParams{std::move(*uri), std::move(*text), *version};
}

std::optional<std::string> decodeCloseDocument(const JsonObject &request) {
  return getStringField(request, {"params", "textDocument", "uri"});
}

std::optional<std::vector<WatchedFileChange>>
decodeWatchedFiles(const JsonObject &request) {
  const JsonObject *changes = getPath(request, {"params", "changes"});
  if (!changes || !changes->isList()) {
    return std::nullopt;
  }
  std::vector<WatchedFileChange> result;
  result.reserve(changes->getAsList().size());
  for (const auto &change : changes->getAsList()) {
    auto uri = getStringField(change, {"uri"});
    if (!uri) {
      return std::nullopt;
    }
    result.push_back({std::move(*uri)});
  }
  return result;
}

} // namespace zap::lsp
