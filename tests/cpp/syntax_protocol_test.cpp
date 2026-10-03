#include "macros/syntax_protocol.hpp"
#include "token/token.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <variant>

namespace {

using namespace zap::ctfe;

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

SyntaxSpan span(uint64_t offset, uint64_t length) {
  return {"query.zp", 1, offset + 1, offset, length};
}

SyntaxMacroRequest sourceRequest() {
  SyntaxSource source;
  source.text = "${x}";
  source.sourceName = "query.zp";
  for (uint64_t offset = 0; offset <= source.text.size(); ++offset)
    source.offsets.push_back({1, offset + 1, offset});
  SyntaxTokens expression;
  expression.tokens.push_back(
      {TokenType::ID, "x", "x", span(2, 1), 7, nullptr});
  source.interpolations.push_back({span(0, 4), 2, 3, expression});
  return {SyntaxProtocolVersion, "sql@v1", span(0, 6),
          SyntaxContext::Expression, source};
}

void testCanonicalRequest() {
  const auto request = sourceRequest();
  const auto encoded = encodeRequest(request);
  require(std::holds_alternative<std::string>(encoded),
          "source request could not be encoded");
  const auto second = encodeRequest(request);
  require(std::get<std::string>(encoded) == std::get<std::string>(second),
          "request encoding was not deterministic");
  const auto size = std::get<std::string>(encoded).size();
  require(std::holds_alternative<std::string>(encodeRequest(request, size)) &&
              std::get<SyntaxProtocolError>(encodeRequest(request, size - 1)) ==
                  SyntaxProtocolError::LimitExceeded,
          "request encoder bypassed the caller's allocation budget");
  const auto key = encodeCacheRequest(request, size);
  require(std::holds_alternative<std::string>(key) &&
              std::get<SyntaxProtocolError>(encodeCacheRequest(request, 0)) ==
                  SyntaxProtocolError::LimitExceeded,
          "semantic key encoder bypassed its allocation budget");
  SyntaxMacroResult result;
  result.output = request.input;
  const auto resultSize = std::get<std::string>(encodeResult(result)).size();
  require(
      std::holds_alternative<std::string>(encodeResult(result, resultSize)) &&
          std::get<SyntaxProtocolError>(encodeResult(result, resultSize - 1)) ==
              SyntaxProtocolError::LimitExceeded,
      "result encoder bypassed the caller's allocation budget");
  const auto decoded = decodeRequest(std::get<std::string>(encoded));
  require(std::holds_alternative<SyntaxMacroRequest>(decoded),
          "source request did not round trip");
  const auto &roundTrip = std::get<SyntaxMacroRequest>(decoded);
  const auto *source = std::get_if<SyntaxSource>(&roundTrip.input);
  require(source && roundTrip.definitionId == "sql@v1" &&
              roundTrip.expected == SyntaxContext::Expression &&
              source->offsets.size() == 5 &&
              source->interpolations.size() == 1 &&
              source->interpolations[0].expression.tokens[0].context == 7 &&
              std::get<std::string>(encodeRequest(roundTrip)) ==
                  std::get<std::string>(encoded),
          "source positions, hygiene, or canonical encoding changed");
}

void testStableWireEnvelope() {
  SyntaxMacroRequest request;
  const auto encoded = encodeRequest(request);
  require(std::holds_alternative<std::string>(encoded),
          "empty token request could not be encoded");
  const auto &bytes = std::get<std::string>(encoded);
  require(bytes.size() == 53 &&
              bytes.substr(0, 7) == std::string("ZSM1\x01\x01\x00", 7) &&
              static_cast<uint8_t>(bytes[47]) == 1 &&
              static_cast<uint8_t>(bytes[48]) == 1 &&
              static_cast<uint8_t>(bytes[49]) == 0,
          "wire envelope or field order changed without a version bump");
  auto excessiveCount = bytes;
  excessiveCount[50] = static_cast<char>(0xff);
  excessiveCount[51] = static_cast<char>(0xff);
  excessiveCount[52] = static_cast<char>(0xff);
  require(std::get<SyntaxProtocolError>(decodeRequest(excessiveCount)) ==
              SyntaxProtocolError::LimitExceeded,
          "oversized token count was accepted");
}

void testTypedResults() {
  SyntaxTokens syntax;
  syntax.tokens.push_back(
      {TokenType::INTEGER, "42", "42", span(3, 2), 9, nullptr});
  SyntaxMacroResult result;
  result.output = SyntaxExpr{syntax};
  result.diagnostics.push_back(
      {SyntaxSeverity::Warning, "M2001", "example", span(3, 2)});
  const auto encoded = encodeResult(result);
  require(std::holds_alternative<std::string>(encoded),
          "typed result could not be encoded");
  const auto decoded = decodeResult(std::get<std::string>(encoded));
  require(std::holds_alternative<SyntaxMacroResult>(decoded),
          "typed result did not round trip");
  const auto &roundTrip = std::get<SyntaxMacroResult>(decoded);
  require(
      roundTrip.output &&
          std::holds_alternative<SyntaxExpr>(*roundTrip.output) &&
          std::get<SyntaxExpr>(*roundTrip.output).syntax.tokens[0].context ==
              9 &&
          roundTrip.diagnostics.size() == 1 &&
          roundTrip.diagnostics[0].severity == SyntaxSeverity::Warning &&
          std::get<std::string>(encodeResult(roundTrip)) ==
              std::get<std::string>(encoded),
      "typed output, diagnostics, or hygiene changed");
}

void testNestedSourceToken() {
  auto nested = std::make_shared<SyntaxSource>();
  nested->text = "abc";
  nested->sourceName = "query.zp";
  for (uint64_t offset = 0; offset <= nested->text.size(); ++offset)
    nested->offsets.push_back({1, offset + 1, offset});
  SyntaxTokens tokens;
  tokens.tokens.push_back({TokenType::LBRACE, "{", "{", span(0, 1), 3, nested});
  SyntaxMacroRequest request;
  request.input = tokens;
  const auto encoded = encodeRequest(request);
  require(std::holds_alternative<std::string>(encoded),
          "nested source group was not encoded");
  const auto decoded = decodeRequest(std::get<std::string>(encoded));
  require(std::holds_alternative<SyntaxMacroRequest>(decoded),
          "nested source group did not decode");
  const auto &roundTrip = std::get<SyntaxMacroRequest>(decoded);
  const auto *decodedTokens = std::get_if<SyntaxTokens>(&roundTrip.input);
  require(decodedTokens && decodedTokens->tokens[0].sourceFragment &&
              decodedTokens->tokens[0].sourceFragment->text == "abc" &&
              std::get<std::string>(encodeRequest(roundTrip)) ==
                  std::get<std::string>(encoded),
          "nested source bytes were not preserved canonically");
}

void testRejectsExcessiveNesting() {
  auto nested = std::make_shared<SyntaxSource>();
  nested->text = "leaf";
  nested->sourceName = "query.zp";
  for (uint64_t offset = 0; offset <= nested->text.size(); ++offset)
    nested->offsets.push_back({1, offset + 1, offset});

  for (size_t depth = 0; depth <= MaxSyntaxNesting; ++depth) {
    auto parent = std::make_shared<SyntaxSource>();
    parent->text = "${x}";
    parent->sourceName = "query.zp";
    for (uint64_t offset = 0; offset <= parent->text.size(); ++offset)
      parent->offsets.push_back({1, offset + 1, offset});
    SyntaxTokens expression;
    expression.tokens.push_back(
        {TokenType::LBRACE, "{", "{", span(2, 1), 0, nested});
    parent->interpolations.push_back({span(0, 4), 2, 3, expression});
    nested = std::move(parent);
  }

  SyntaxTokens tokens;
  tokens.tokens.push_back({TokenType::LBRACE, "{", "{", span(0, 1), 0, nested});
  SyntaxMacroRequest request;
  request.input = std::move(tokens);
  require(std::get<SyntaxProtocolError>(encodeRequest(request)) ==
              SyntaxProtocolError::InvalidMessage,
          "excessively nested source groups were accepted");
}

void testRejectsMalformedMessages() {
  SyntaxMacroRequest reserved;
  reserved.input = SyntaxTokens{{{TokenType::ID, "name", "name", span(0, 4),
                                  GeneratedSyntaxContext, nullptr}}};
  require(std::get<SyntaxProtocolError>(encodeRequest(reserved)) ==
                  SyntaxProtocolError::InvalidMessage &&
              std::get<SyntaxProtocolError>(
                  encodeCacheRequest(reserved, MaxSyntaxMessageBytes)) ==
                  SyntaxProtocolError::InvalidMessage,
          "request accepted the reserved generated-output context");
  SyntaxMacroResult generated;
  generated.output = reserved.input;
  require(std::holds_alternative<std::string>(encodeResult(generated)),
          "result rejected its legitimate generated-output marker");
  std::get<SyntaxTokens>(reserved.input).tokens[0].context = 0;
  auto forged = std::get<std::string>(encodeRequest(reserved));
  for (size_t i = forged.size() - 5; i < forged.size() - 1; ++i)
    forged[i] = static_cast<char>(0xff);
  require(std::get<SyntaxProtocolError>(decodeRequest(forged)) ==
              SyntaxProtocolError::InvalidMessage,
          "wire request decoder accepted the reserved context");
  auto nestedReserved = sourceRequest();
  std::get<SyntaxSource>(nestedReserved.input)
      .interpolations[0]
      .expression.tokens[0]
      .context = GeneratedSyntaxContext;
  require(std::get<SyntaxProtocolError>(encodeRequest(nestedReserved)) ==
              SyntaxProtocolError::InvalidMessage,
          "nested source interpolation accepted the reserved context");
  const auto encoded = std::get<std::string>(encodeRequest(sourceRequest()));
  auto truncated = encoded.substr(0, encoded.size() - 1);
  require(std::holds_alternative<SyntaxProtocolError>(decodeRequest(truncated)),
          "truncated request was accepted");
  auto trailing = encoded + "x";
  require(std::holds_alternative<SyntaxProtocolError>(decodeRequest(trailing)),
          "trailing bytes were accepted");
  auto wrongVersion = encoded;
  wrongVersion[5] = 2;
  require(std::get<SyntaxProtocolError>(decodeRequest(wrongVersion)) ==
              SyntaxProtocolError::UnsupportedVersion,
          "unknown protocol version was accepted");
  auto wrongTag = encoded;
  wrongTag[4] = 2;
  require(std::holds_alternative<SyntaxProtocolError>(decodeRequest(wrongTag)),
          "result envelope was accepted as a request");
  auto corruptedSource = encoded;
  const auto sourceBegin = corruptedSource.find("${x}");
  require(sourceBegin != std::string::npos,
          "encoded request lost raw source bytes");
  corruptedSource[sourceBegin] = '#';
  require(std::get<SyntaxProtocolError>(decodeRequest(corruptedSource)) ==
              SyntaxProtocolError::InvalidMessage,
          "source interpolation mismatch was accepted");
  require(std::get<SyntaxProtocolError>(
              decodeRequest(std::string(MaxSyntaxMessageBytes + 1, 'x'))) ==
              SyntaxProtocolError::LimitExceeded,
          "oversized request was accepted");

  auto invalidSource = sourceRequest();
  std::get<SyntaxSource>(invalidSource.input).offsets.pop_back();
  require(std::get<SyntaxProtocolError>(encodeRequest(invalidSource)) ==
              SyntaxProtocolError::InvalidMessage,
          "invalid source mapping was encoded");
  auto invalidToken = sourceRequest();
  std::get<SyntaxSource>(invalidToken.input)
      .interpolations[0]
      .expression.tokens[0]
      .type = 999;
  require(std::get<SyntaxProtocolError>(encodeRequest(invalidToken)) ==
              SyntaxProtocolError::InvalidMessage,
          "unknown token kind was encoded");
  auto unsupported = sourceRequest();
  unsupported.version = SyntaxProtocolVersion + 1;
  require(std::get<SyntaxProtocolError>(encodeRequest(unsupported)) ==
              SyntaxProtocolError::UnsupportedVersion,
          "unsupported request version was encoded");
}

} // namespace

int main() {
  testCanonicalRequest();
  testStableWireEnvelope();
  testTypedResults();
  testNestedSourceToken();
  testRejectsExcessiveNesting();
  testRejectsMalformedMessages();
}
