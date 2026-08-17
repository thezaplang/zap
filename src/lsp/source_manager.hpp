#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace zap::lsp {

using SourceId = uint64_t;

struct SourceSnapshot {
  SourceId id = 0;
  std::string uri;
  std::filesystem::path path;
  std::string text;
  int64_t version = 0;
};

class SourceManager {
public:
  using Snapshot = std::shared_ptr<const SourceSnapshot>;

private:
  struct CachedFile {
    std::filesystem::file_time_type lastWriteTime;
    Snapshot snapshot;
  };

  SourceId nextSourceId_ = 1;
  std::unordered_map<std::string, Snapshot> sourcesByUri_;
  std::unordered_map<std::string, std::string> uriByCanonicalPath_;
  std::unordered_map<std::string, CachedFile> fileContentCache_;

  std::filesystem::path canonicalPath(const std::filesystem::path &path) const;
  Snapshot makeSnapshot(std::string uri, std::filesystem::path path,
                        std::string text, int64_t version);

public:
  const SourceSnapshot *document(const std::string &uri) const;
  bool contains(const std::string &uri) const;
  void open(const std::string &uri, std::filesystem::path path,
            std::string text, int64_t version);
  void update(const std::string &uri, std::string text, int64_t version);
  void close(const std::string &uri);
  void invalidatePath(const std::filesystem::path &path);
  std::vector<std::string> openUris() const;
  std::optional<Snapshot> sourceForPath(const std::filesystem::path &path);
  std::optional<Snapshot> sourceForUri(const std::string &uri);
  std::string uriForPath(const std::filesystem::path &path) const;
};

} // namespace zap::lsp
