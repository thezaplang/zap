#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace zap::ctfe {

inline constexpr uint16_t SyntaxProtocolVersion = 1;
inline constexpr size_t MaxSyntaxMessageBytes = 16 * 1024 * 1024;
inline constexpr size_t MaxSyntaxEntries = 100'000;
inline constexpr size_t MaxSyntaxNesting = 32;

struct SyntaxSpan {
  std::string sourceName;
  uint64_t line = 0;
  uint64_t column = 0;
  uint64_t offset = 0;
  uint64_t length = 0;
};

struct SyntaxSource;

struct SyntaxToken {
  uint32_t type = 0;
  std::string value;
  std::string spelling;
  SyntaxSpan span;
  uint32_t context = 0;
  std::shared_ptr<const SyntaxSource> sourceFragment;
};

struct SyntaxTokens {
  std::vector<SyntaxToken> tokens;
};

struct SyntaxOffset {
  uint64_t line = 0;
  uint64_t column = 0;
  uint64_t offset = 0;
};

struct SyntaxInterpolation {
  SyntaxSpan span;
  uint64_t bodyBegin = 0;
  uint64_t bodyEnd = 0;
  SyntaxTokens expression;
};

struct SyntaxSource {
  std::string text;
  std::string sourceName;
  std::vector<SyntaxOffset> offsets;
  std::vector<SyntaxInterpolation> interpolations;
};

// The receiver validates these fragments with its parser before using them.
// The wire format carries syntax, never compiler-owned AST pointers.
struct SyntaxExpr {
  SyntaxTokens syntax;
};

struct SyntaxItem {
  SyntaxTokens syntax;
};

using SyntaxValue =
    std::variant<SyntaxTokens, SyntaxSource, SyntaxExpr, SyntaxItem>;

enum class SyntaxContext : uint8_t {
  Expression = 1,
  Statement = 2,
  Type = 3,
  Item = 4,
};

enum class SyntaxSeverity : uint8_t { Error = 1, Warning = 2, Note = 3 };

struct SyntaxDiagnostic {
  SyntaxSeverity severity = SyntaxSeverity::Error;
  std::string code;
  std::string message;
  SyntaxSpan span;
};

struct SyntaxMacroRequest {
  uint16_t version = SyntaxProtocolVersion;
  std::string definitionId;
  SyntaxSpan invocation;
  SyntaxContext expected = SyntaxContext::Expression;
  SyntaxValue input;
};

struct SyntaxMacroResult {
  uint16_t version = SyntaxProtocolVersion;
  std::optional<SyntaxValue> output;
  std::vector<SyntaxDiagnostic> diagnostics;
};

enum class SyntaxProtocolError {
  UnsupportedVersion,
  InvalidMessage,
  LimitExceeded,
};

template <typename T>
using SyntaxProtocolOutcome = std::variant<T, SyntaxProtocolError>;

SyntaxProtocolOutcome<std::string>
encodeRequest(const SyntaxMacroRequest &request);
SyntaxProtocolOutcome<SyntaxMacroRequest> decodeRequest(std::string_view bytes);
SyntaxProtocolOutcome<std::string>
encodeResult(const SyntaxMacroResult &result);
SyntaxProtocolOutcome<SyntaxMacroResult> decodeResult(std::string_view bytes);

} // namespace zap::ctfe
