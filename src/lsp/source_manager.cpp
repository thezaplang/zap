#include "lsp/source_manager.hpp"

#include "lsp/protocol_utils.hpp"
#include <utility>

namespace zap::lsp {

std::filesystem::path
SourceManager::canonicalPath(const std::filesystem::path &path) const {
  std::error_code ec;
  auto canonical = std::filesystem::weakly_canonical(path, ec);
  return ec ? std::filesystem::absolute(path).lexically_normal() : canonical;
}

SourceManager::Snapshot SourceManager::makeSnapshot(std::string uri,
                                                    std::filesystem::path path,
                                                    std::string text,
                                                    int64_t version) {
  auto snapshot = std::make_shared<SourceSnapshot>(
      SourceSnapshot{nextSourceId_++, std::move(uri), std::move(path),
                     std::move(text), version});
  return snapshot;
}

const SourceSnapshot *SourceManager::document(const std::string &uri) const {
  auto source = sourcesByUri_.find(uri);
  return source == sourcesByUri_.end() ? nullptr : source->second.get();
}

bool SourceManager::contains(const std::string &uri) const {
  return sourcesByUri_.find(uri) != sourcesByUri_.end();
}

void SourceManager::open(const std::string &uri, std::filesystem::path path,
                         std::string text, int64_t version) {
  path = canonicalPath(path);
  const std::string pathKey = path.string();
  auto existingUri = uriByCanonicalPath_.find(pathKey);
  if (existingUri != uriByCanonicalPath_.end() && existingUri->second != uri) {
    sourcesByUri_.erase(existingUri->second);
  }
  uriByCanonicalPath_[pathKey] = uri;
  sourcesByUri_[uri] =
      makeSnapshot(uri, std::move(path), std::move(text), version);
}

void SourceManager::update(const std::string &uri, std::string text,
                           int64_t version) {
  auto source = sourcesByUri_.find(uri);
  if (source == sourcesByUri_.end()) {
    return;
  }
  source->second =
      makeSnapshot(uri, source->second->path, std::move(text), version);
}

void SourceManager::close(const std::string &uri) {
  auto source = sourcesByUri_.find(uri);
  if (source == sourcesByUri_.end()) {
    return;
  }
  auto path = uriByCanonicalPath_.find(source->second->path.string());
  if (path != uriByCanonicalPath_.end() && path->second == uri) {
    uriByCanonicalPath_.erase(path);
  }
  sourcesByUri_.erase(source);
}

void SourceManager::invalidatePath(const std::filesystem::path &path) {
  fileContentCache_.erase(canonicalPath(path).string());
}

std::vector<std::string> SourceManager::openUris() const {
  std::vector<std::string> uris;
  uris.reserve(sourcesByUri_.size());
  for (const auto &[uri, _] : sourcesByUri_) {
    uris.push_back(uri);
  }
  return uris;
}

std::optional<SourceManager::Snapshot>
SourceManager::sourceForPath(const std::filesystem::path &path) {
  const auto canonical = canonicalPath(path);
  const std::string pathKey = canonical.string();
  auto openUri = uriByCanonicalPath_.find(pathKey);
  if (openUri != uriByCanonicalPath_.end()) {
    auto source = sourcesByUri_.find(openUri->second);
    if (source != sourcesByUri_.end()) {
      return source->second;
    }
  }

  std::error_code ec;
  const auto writeTime = std::filesystem::last_write_time(canonical, ec);
  if (!ec) {
    auto cached = fileContentCache_.find(pathKey);
    if (cached != fileContentCache_.end() &&
        cached->second.lastWriteTime == writeTime) {
      return cached->second.snapshot;
    }
  }

  std::string content;
  if (!readSourceFile(canonical, content)) {
    return std::nullopt;
  }
  auto snapshot =
      makeSnapshot(pathToUri(canonical), canonical, std::move(content), 0);
  if (!ec) {
    fileContentCache_[pathKey] = CachedFile{writeTime, snapshot};
  }
  return snapshot;
}

std::optional<SourceManager::Snapshot>
SourceManager::sourceForUri(const std::string &uri) {
  auto source = sourcesByUri_.find(uri);
  if (source != sourcesByUri_.end()) {
    return source->second;
  }
  auto path = uriToPath(uri);
  return path ? sourceForPath(*path) : std::nullopt;
}

std::string SourceManager::uriForPath(const std::filesystem::path &path) const {
  const auto canonical = canonicalPath(path);
  auto openUri = uriByCanonicalPath_.find(canonical.string());
  return openUri == uriByCanonicalPath_.end() ? pathToUri(canonical)
                                              : openUri->second;
}

} // namespace zap::lsp
