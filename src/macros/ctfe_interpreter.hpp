#pragma once

#include "macros/syntax_protocol.hpp"
#include "token/token.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <tuple>

namespace zap {
class DiagnosticEngine;
}

namespace zap::ctfe {

struct CtfeLimits {
  size_t maxSteps = 100'000;
  size_t maxMemoryBytes = 4 * 1024 * 1024;
  size_t maxCallDepth = 32;
  size_t maxDefinitionBytes = 64 * 1024;
  size_t maxDefinitionTokens = 1'024;
  size_t maxSyntaxDepth = 64;
  size_t maxCacheBytes = 8 * 1024 * 1024;
  size_t maxCacheEntries = 256;

  auto key() const {
    return std::make_tuple(maxSteps, maxMemoryBytes, maxCallDepth,
                           maxDefinitionBytes, maxDefinitionTokens,
                           maxSyntaxDepth, maxCacheBytes, maxCacheEntries);
  }
};

class CtfeInterpreter {
public:
  static bool validateDefinition(const std::string &source,
                                 const SourceSpan &bodySpan,
                                 zap::DiagnosticEngine &diagnostics,
                                 CtfeLimits limits = {});
  SyntaxMacroResult execute(std::string_view definitionSource,
                            std::string_view entryName,
                            const SyntaxMacroRequest &request,
                            CtfeLimits limits = {});

  size_t cacheHits() const noexcept { return cacheHits_; }
  size_t cacheEntries() const noexcept { return cache_.size(); }

private:
  struct CacheKey {
    std::string definitionSource;
    std::string entryName;
    std::string request;
    CtfeLimits limits;

    bool operator<(const CacheKey &other) const {
      const auto ownLimits = limits.key();
      const auto otherLimits = other.limits.key();
      return std::tie(definitionSource, entryName, request, ownLimits) <
             std::tie(other.definitionSource, other.entryName, other.request,
                      otherLimits);
    }
  };

  struct CacheEntry {
    std::string generatedResult;
    // Input is returned by identity; recover captures and hygiene from this
    // call. ponytail: whole-input or generated output only; syntax composition
    // will require per-fragment provenance instead of this identity flag.
    bool returnsInput = false;
  };
  std::map<CacheKey, CacheEntry> cache_;
  size_t cacheBytes_ = 0;
  size_t cacheHits_ = 0;
};

} // namespace zap::ctfe
