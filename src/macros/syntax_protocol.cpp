#include "macros/syntax_protocol.hpp"

#include "token/token.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace zap::ctfe {
namespace {

constexpr std::string_view Magic = "ZSM1";
constexpr uint8_t RequestTag = 1;
constexpr uint8_t ResultTag = 2;

class Writer {
public:
  enum class Purpose { Wire, CacheKey };
  explicit Writer(size_t maximum = MaxSyntaxMessageBytes,
                  Purpose purpose = Purpose::Wire)
      : maximum_(std::min(maximum, MaxSyntaxMessageBytes)), purpose_(purpose) {}
  bool sourceName(std::string_view value) {
    return string(purpose_ == Purpose::Wire ? value : std::string_view{});
  }
  bool location(uint64_t value) {
    return u64(purpose_ == Purpose::Wire ? value : 0);
  }
  bool context(uint32_t value) {
    return u32(purpose_ == Purpose::Wire ? value : 0);
  }
  bool byte(uint8_t value) { return append(&value, 1); }

  bool u16(uint16_t value) {
    return byte(static_cast<uint8_t>(value)) &&
           byte(static_cast<uint8_t>(value >> 8));
  }

  bool u32(uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
      if (!byte(static_cast<uint8_t>(value >> shift)))
        return false;
    return true;
  }

  bool u64(uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8)
      if (!byte(static_cast<uint8_t>(value >> shift)))
        return false;
    return true;
  }

  bool string(std::string_view value) {
    return value.size() <= std::numeric_limits<uint32_t>::max() &&
           u32(static_cast<uint32_t>(value.size())) &&
           append(value.data(), value.size());
  }

  bool raw(std::string_view value) {
    return append(value.data(), value.size());
  }

  std::string take() { return std::move(bytes_); }

private:
  bool append(const void *data, size_t size) {
    if (size > maximum_ - bytes_.size())
      return false;
    bytes_.append(static_cast<const char *>(data), size);
    return true;
  }

  std::string bytes_;
  size_t maximum_;
  Purpose purpose_;
};

class Reader {
public:
  explicit Reader(std::string_view bytes) : bytes_(bytes) {}

  bool byte(uint8_t &value) {
    if (remaining() < 1)
      return false;
    value = static_cast<uint8_t>(bytes_[position_++]);
    return true;
  }

  bool u16(uint16_t &value) {
    uint8_t lo = 0;
    uint8_t hi = 0;
    if (!byte(lo) || !byte(hi))
      return false;
    value = static_cast<uint16_t>(lo | (static_cast<uint16_t>(hi) << 8));
    return true;
  }

  bool u32(uint32_t &value) {
    value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
      uint8_t part = 0;
      if (!byte(part))
        return false;
      value |= static_cast<uint32_t>(part) << shift;
    }
    return true;
  }

  bool u64(uint64_t &value) {
    value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
      uint8_t part = 0;
      if (!byte(part))
        return false;
      value |= static_cast<uint64_t>(part) << shift;
    }
    return true;
  }

  bool string(std::string &value) {
    uint32_t size = 0;
    if (!u32(size) || size > remaining())
      return false;
    value.assign(bytes_.data() + position_, size);
    position_ += size;
    return true;
  }

  bool raw(std::string_view value) {
    if (remaining() < value.size() ||
        bytes_.substr(position_, value.size()) != value)
      return false;
    position_ += value.size();
    return true;
  }

  bool count(uint32_t &value) {
    if (!u32(value))
      return false;
    if (value > MaxSyntaxEntries) {
      error_ = SyntaxProtocolError::LimitExceeded;
      return false;
    }
    return true;
  }

  size_t remaining() const { return bytes_.size() - position_; }
  SyntaxProtocolError error() const { return error_; }

private:
  std::string_view bytes_;
  size_t position_ = 0;
  SyntaxProtocolError error_ = SyntaxProtocolError::InvalidMessage;
};

bool writeSpan(Writer &writer, const SyntaxSpan &span) {
  return writer.sourceName(span.sourceName) && writer.location(span.line) &&
         writer.location(span.column) && writer.location(span.offset) &&
         writer.location(span.length);
}

bool readSpan(Reader &reader, SyntaxSpan &span) {
  return reader.string(span.sourceName) && reader.u64(span.line) &&
         reader.u64(span.column) && reader.u64(span.offset) &&
         reader.u64(span.length);
}

bool writeSource(Writer &writer, const SyntaxSource &source, size_t depth);
bool readSource(Reader &reader, SyntaxSource &source, size_t depth);
bool validSource(const SyntaxSource &source, size_t depth);

bool writeTokens(Writer &writer, const SyntaxTokens &syntax, size_t depth) {
  if (depth > MaxSyntaxNesting || syntax.tokens.size() > MaxSyntaxEntries ||
      !writer.u32(static_cast<uint32_t>(syntax.tokens.size())))
    return false;
  for (const auto &token : syntax.tokens) {
    if (token.type < TokenType::IMPORT || token.type > TokenType::DOLLAR ||
        !writer.u32(token.type) || !writer.string(token.value) ||
        !writer.string(token.spelling) || !writeSpan(writer, token.span) ||
        !writer.context(token.context) ||
        !writer.byte(token.sourceFragment ? 1 : 0) ||
        (token.sourceFragment &&
         !writeSource(writer, *token.sourceFragment, depth + 1)))
      return false;
  }
  return true;
}

bool validTokens(const SyntaxTokens &syntax, size_t depth) {
  if (depth > MaxSyntaxNesting || syntax.tokens.size() > MaxSyntaxEntries)
    return false;
  for (const auto &token : syntax.tokens)
    if (token.type < TokenType::IMPORT || token.type > TokenType::DOLLAR ||
        (token.sourceFragment && token.type != TokenType::LBRACE) ||
        (token.sourceFragment &&
         !validSource(*token.sourceFragment, depth + 1)))
      return false;
  return true;
}

bool readTokens(Reader &reader, SyntaxTokens &syntax, size_t depth) {
  uint32_t count = 0;
  if (depth > MaxSyntaxNesting || !reader.count(count) ||
      count > reader.remaining() / 53)
    return false;
  syntax.tokens.reserve(count);
  for (uint32_t index = 0; index < count; ++index) {
    SyntaxToken token;
    uint8_t hasSource = 0;
    if (!reader.u32(token.type) || token.type < TokenType::IMPORT ||
        token.type > TokenType::DOLLAR || !reader.string(token.value) ||
        !reader.string(token.spelling) || !readSpan(reader, token.span) ||
        !reader.u32(token.context) || !reader.byte(hasSource) || hasSource > 1)
      return false;
    if (hasSource && token.type != TokenType::LBRACE)
      return false;
    if (hasSource) {
      auto source = std::make_shared<SyntaxSource>();
      if (!readSource(reader, *source, depth + 1))
        return false;
      token.sourceFragment = std::move(source);
    }
    syntax.tokens.push_back(std::move(token));
  }
  return true;
}

bool validSource(const SyntaxSource &source, size_t depth) {
  if (depth > MaxSyntaxNesting ||
      source.offsets.size() != source.text.size() + 1 ||
      source.offsets.size() > MaxSyntaxEntries ||
      source.interpolations.size() > MaxSyntaxEntries)
    return false;
  for (size_t index = 0; index < source.text.size(); ++index) {
    const auto &current = source.offsets[index];
    const auto &next = source.offsets[index + 1];
    if (current.offset == std::numeric_limits<uint64_t>::max() ||
        next.offset != current.offset + 1)
      return false;
    if (source.text[index] == '\n') {
      if (current.line == std::numeric_limits<uint64_t>::max() ||
          next.line != current.line + 1 || next.column != 1)
        return false;
    } else if (current.column == std::numeric_limits<uint64_t>::max() ||
               next.line != current.line || next.column != current.column + 1) {
      return false;
    }
  }
  uint64_t previousEnd = 0;
  for (const auto &interpolation : source.interpolations) {
    if (interpolation.bodyBegin < 2)
      return false;
    const auto begin = interpolation.bodyBegin - 2;
    if (begin < previousEnd ||
        interpolation.bodyEnd < interpolation.bodyBegin ||
        interpolation.bodyEnd >= source.text.size() ||
        source.text.compare(begin, 2, "${") != 0 ||
        source.text[interpolation.bodyEnd] != '}' ||
        interpolation.span.sourceName != source.sourceName ||
        interpolation.span.line != source.offsets[begin].line ||
        interpolation.span.column != source.offsets[begin].column ||
        interpolation.span.offset != source.offsets[begin].offset ||
        interpolation.span.length != interpolation.bodyEnd + 1 - begin ||
        interpolation.expression.tokens.empty() ||
        !validTokens(interpolation.expression, depth))
      return false;
    previousEnd = interpolation.bodyEnd + 1;
  }
  return true;
}

bool writeSource(Writer &writer, const SyntaxSource &source, size_t depth) {
  if (!validSource(source, depth) || !writer.string(source.text) ||
      !writer.sourceName(source.sourceName) ||
      !writer.u32(static_cast<uint32_t>(source.offsets.size())))
    return false;
  for (const auto &offset : source.offsets)
    if (!writer.location(offset.line) || !writer.location(offset.column) ||
        !writer.location(offset.offset))
      return false;
  if (!writer.u32(static_cast<uint32_t>(source.interpolations.size())))
    return false;
  for (const auto &interpolation : source.interpolations)
    if (!writeSpan(writer, interpolation.span) ||
        !writer.u64(interpolation.bodyBegin) ||
        !writer.u64(interpolation.bodyEnd) ||
        !writeTokens(writer, interpolation.expression, depth))
      return false;
  return true;
}

bool readSource(Reader &reader, SyntaxSource &source, size_t depth) {
  if (depth > MaxSyntaxNesting)
    return false;
  uint32_t count = 0;
  if (!reader.string(source.text) || !reader.string(source.sourceName) ||
      !reader.count(count) || count > reader.remaining() / 24)
    return false;
  source.offsets.reserve(count);
  for (uint32_t index = 0; index < count; ++index) {
    SyntaxOffset offset;
    if (!reader.u64(offset.line) || !reader.u64(offset.column) ||
        !reader.u64(offset.offset))
      return false;
    source.offsets.push_back(offset);
  }
  if (!reader.count(count) || count > reader.remaining() / 56)
    return false;
  source.interpolations.reserve(count);
  for (uint32_t index = 0; index < count; ++index) {
    SyntaxInterpolation interpolation;
    if (!readSpan(reader, interpolation.span) ||
        !reader.u64(interpolation.bodyBegin) ||
        !reader.u64(interpolation.bodyEnd) ||
        !readTokens(reader, interpolation.expression, depth))
      return false;
    source.interpolations.push_back(std::move(interpolation));
  }
  return validSource(source, depth);
}

bool writeValue(Writer &writer, const SyntaxValue &value) {
  if (!writer.byte(static_cast<uint8_t>(value.index() + 1)))
    return false;
  if (const auto *tokens = std::get_if<SyntaxTokens>(&value))
    return writeTokens(writer, *tokens, 0);
  if (const auto *source = std::get_if<SyntaxSource>(&value))
    return writeSource(writer, *source, 0);
  if (const auto *expr = std::get_if<SyntaxExpr>(&value))
    return writeTokens(writer, expr->syntax, 0);
  return writeTokens(writer, std::get<SyntaxItem>(value).syntax, 0);
}

bool validValue(const SyntaxValue &value) {
  if (const auto *tokens = std::get_if<SyntaxTokens>(&value))
    return validTokens(*tokens, 0);
  if (const auto *source = std::get_if<SyntaxSource>(&value))
    return validSource(*source, 0);
  if (const auto *expr = std::get_if<SyntaxExpr>(&value))
    return validTokens(expr->syntax, 0);
  return validTokens(std::get<SyntaxItem>(value).syntax, 0);
}

bool capturedSource(const SyntaxSource &source);
bool capturedTokens(const SyntaxTokens &tokens) {
  for (const auto &token : tokens.tokens)
    if (token.context == GeneratedSyntaxContext ||
        (token.sourceFragment && !capturedSource(*token.sourceFragment)))
      return false;
  return true;
}
bool capturedSource(const SyntaxSource &source) {
  for (const auto &interpolation : source.interpolations)
    if (!capturedTokens(interpolation.expression))
      return false;
  return true;
}
bool validRequestValue(const SyntaxValue &value) {
  // Validate depth/shape first, before walking possibly cyclic source pointers.
  if (!validValue(value))
    return false;
  if (const auto *tokens = std::get_if<SyntaxTokens>(&value))
    return capturedTokens(*tokens);
  if (const auto *source = std::get_if<SyntaxSource>(&value))
    return capturedSource(*source);
  if (const auto *expr = std::get_if<SyntaxExpr>(&value))
    return capturedTokens(expr->syntax);
  return capturedTokens(std::get<SyntaxItem>(value).syntax);
}

bool readValue(Reader &reader, SyntaxValue &value) {
  uint8_t kind = 0;
  if (!reader.byte(kind))
    return false;
  if (kind == 2) {
    SyntaxSource source;
    if (!readSource(reader, source, 0))
      return false;
    value = std::move(source);
    return true;
  }
  if (kind != 1 && kind != 3 && kind != 4)
    return false;
  SyntaxTokens tokens;
  if (!readTokens(reader, tokens, 0))
    return false;
  if (kind == 1)
    value = std::move(tokens);
  else if (kind == 3)
    value = SyntaxExpr{std::move(tokens)};
  else
    value = SyntaxItem{std::move(tokens)};
  return true;
}

bool validContext(uint8_t value) {
  return value >= static_cast<uint8_t>(SyntaxContext::Expression) &&
         value <= static_cast<uint8_t>(SyntaxContext::Item);
}

bool validSeverity(uint8_t value) {
  return value >= static_cast<uint8_t>(SyntaxSeverity::Error) &&
         value <= static_cast<uint8_t>(SyntaxSeverity::Note);
}

bool writeHeader(Writer &writer, uint8_t tag) {
  return writer.raw(Magic) && writer.byte(tag) &&
         writer.u16(SyntaxProtocolVersion);
}

bool readHeader(Reader &reader, uint8_t tag, uint16_t &version) {
  uint8_t actualTag = 0;
  return reader.raw(Magic) && reader.byte(actualTag) && actualTag == tag &&
         reader.u16(version);
}

} // namespace

SyntaxProtocolOutcome<std::string>
encodeRequest(const SyntaxMacroRequest &request, size_t maxBytes) {
  if (request.version != SyntaxProtocolVersion)
    return SyntaxProtocolError::UnsupportedVersion;
  const auto context = static_cast<uint8_t>(request.expected);
  if (!validContext(context) || !validRequestValue(request.input))
    return SyntaxProtocolError::InvalidMessage;
  Writer writer(maxBytes);
  if (!writeHeader(writer, RequestTag) ||
      !writer.string(request.definitionId) ||
      !writeSpan(writer, request.invocation) || !writer.byte(context) ||
      !writeValue(writer, request.input))
    return SyntaxProtocolError::LimitExceeded;
  return writer.take();
}

SyntaxProtocolOutcome<std::string>
encodeCacheRequest(const SyntaxMacroRequest &request, size_t maxBytes) {
  if (request.version != SyntaxProtocolVersion)
    return SyntaxProtocolError::UnsupportedVersion;
  const auto context = static_cast<uint8_t>(request.expected);
  if (!validContext(context) || !validRequestValue(request.input))
    return SyntaxProtocolError::InvalidMessage;
  Writer writer(maxBytes, Writer::Purpose::CacheKey);
  if (!writeHeader(writer, RequestTag) ||
      !writer.string(request.definitionId) ||
      !writeSpan(writer, request.invocation) || !writer.byte(context) ||
      !writeValue(writer, request.input))
    return SyntaxProtocolError::LimitExceeded;
  return writer.take();
}

SyntaxProtocolOutcome<SyntaxMacroRequest>
decodeRequest(std::string_view bytes) {
  if (bytes.size() > MaxSyntaxMessageBytes)
    return SyntaxProtocolError::LimitExceeded;
  Reader reader(bytes);
  SyntaxMacroRequest request;
  uint8_t context = 0;
  if (!readHeader(reader, RequestTag, request.version))
    return reader.error();
  if (request.version != SyntaxProtocolVersion)
    return SyntaxProtocolError::UnsupportedVersion;
  if (!reader.string(request.definitionId) ||
      !readSpan(reader, request.invocation) || !reader.byte(context) ||
      !validContext(context) || !readValue(reader, request.input) ||
      reader.remaining() != 0 || !validRequestValue(request.input))
    return reader.error();
  request.expected = static_cast<SyntaxContext>(context);
  return request;
}

SyntaxProtocolOutcome<std::string> encodeResult(const SyntaxMacroResult &result,
                                                size_t maxBytes) {
  if (result.version != SyntaxProtocolVersion)
    return SyntaxProtocolError::UnsupportedVersion;
  if (result.diagnostics.size() > MaxSyntaxEntries)
    return SyntaxProtocolError::LimitExceeded;
  if (result.output && !validValue(*result.output))
    return SyntaxProtocolError::InvalidMessage;
  for (const auto &diagnostic : result.diagnostics)
    if (!validSeverity(static_cast<uint8_t>(diagnostic.severity)))
      return SyntaxProtocolError::InvalidMessage;
  Writer writer(maxBytes);
  if (!writeHeader(writer, ResultTag) || !writer.byte(result.output ? 1 : 0) ||
      (result.output && !writeValue(writer, *result.output)) ||
      !writer.u32(static_cast<uint32_t>(result.diagnostics.size())))
    return SyntaxProtocolError::LimitExceeded;
  for (const auto &diagnostic : result.diagnostics) {
    const auto severity = static_cast<uint8_t>(diagnostic.severity);
    if (!writer.byte(severity) || !writer.string(diagnostic.code) ||
        !writer.string(diagnostic.message) ||
        !writeSpan(writer, diagnostic.span))
      return SyntaxProtocolError::LimitExceeded;
  }
  return writer.take();
}

SyntaxProtocolOutcome<SyntaxMacroResult> decodeResult(std::string_view bytes) {
  if (bytes.size() > MaxSyntaxMessageBytes)
    return SyntaxProtocolError::LimitExceeded;
  Reader reader(bytes);
  SyntaxMacroResult result;
  uint8_t hasOutput = 0;
  if (!readHeader(reader, ResultTag, result.version))
    return reader.error();
  if (result.version != SyntaxProtocolVersion)
    return SyntaxProtocolError::UnsupportedVersion;
  if (!reader.byte(hasOutput) || hasOutput > 1)
    return reader.error();
  if (hasOutput) {
    SyntaxValue output;
    if (!readValue(reader, output))
      return reader.error();
    result.output = std::move(output);
  }
  uint32_t count = 0;
  if (!reader.count(count) || count > reader.remaining() / 45)
    return reader.error();
  result.diagnostics.reserve(count);
  for (uint32_t index = 0; index < count; ++index) {
    SyntaxDiagnostic diagnostic;
    uint8_t severity = 0;
    if (!reader.byte(severity) || !validSeverity(severity) ||
        !reader.string(diagnostic.code) || !reader.string(diagnostic.message) ||
        !readSpan(reader, diagnostic.span))
      return reader.error();
    diagnostic.severity = static_cast<SyntaxSeverity>(severity);
    result.diagnostics.push_back(std::move(diagnostic));
  }
  if (reader.remaining() != 0)
    return reader.error();
  return result;
}

} // namespace zap::ctfe
