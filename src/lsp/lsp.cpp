#include "lsp.hpp"
#include <charconv>
#include <cctype>
#include <cstddef>
#include <limits>
#include <stdexcept>

namespace zap::lsp {

namespace {

JsonObject parseJson(const char *&it, const char *end);

void jsonSkipWhitespace(const char *&it, const char *end) {
  while (it != end && std::isspace(static_cast<unsigned char>(*it))) {
    ++it;
  }
}

bool appendUtf8(std::string &out, unsigned codepoint) {
  if (codepoint <= 0x7F) {
    out.push_back(static_cast<char>(codepoint));
    return true;
  }
  if (codepoint <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    return true;
  }
  if (codepoint <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    return true;
  }
  if (codepoint <= 0x10FFFF) {
    out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    return true;
  }
  return false;
}

bool parseHex4(const char *&it, const char *end, unsigned &codepoint) {
  codepoint = 0;
  for (int i = 0; i < 4; ++i) {
    if (it == end) {
      return false;
    }
    codepoint <<= 4;
    char ch = *it++;
    if (ch >= '0' && ch <= '9') {
      codepoint |= static_cast<unsigned>(ch - '0');
    } else if (ch >= 'a' && ch <= 'f') {
      codepoint |= static_cast<unsigned>(10 + ch - 'a');
    } else if (ch >= 'A' && ch <= 'F') {
      codepoint |= static_cast<unsigned>(10 + ch - 'A');
    } else {
      return false;
    }
  }
  return true;
}

JsonObject parseJsonString(const char *&it, const char *end) {
  std::string result;
  while (it != end) {
    char ch = *it++;
    if (ch == '"') {
      return JsonObject(std::move(result));
    }
    if (ch != '\\') {
      result.push_back(ch);
      continue;
    }
    if (it == end) {
      return JsonObject(nullptr);
    }
    char escaped = *it++;
    switch (escaped) {
    case '"':
    case '\\':
    case '/':
      result.push_back(escaped);
      break;
    case 'b':
      result.push_back('\b');
      break;
    case 'f':
      result.push_back('\f');
      break;
    case 'n':
      result.push_back('\n');
      break;
    case 'r':
      result.push_back('\r');
      break;
    case 't':
      result.push_back('\t');
      break;
    case 'u': {
      unsigned codepoint = 0;
      if (!parseHex4(it, end, codepoint) || !appendUtf8(result, codepoint)) {
        return JsonObject(nullptr);
      }
      break;
    }
    default:
      return JsonObject(nullptr);
    }
  }

  return JsonObject(nullptr);
}

JsonObject parseJsonNumber(const char *&it, const char *end) {
  const char *start = it;
  if (it != end && *it == '-') {
    ++it;
  }
  if (it == end || !std::isdigit(static_cast<unsigned char>(*it))) {
    return JsonObject(nullptr);
  }
  while (it != end && std::isdigit(static_cast<unsigned char>(*it))) {
    ++it;
  }
  if (it != end && (*it == '.' || *it == 'e' || *it == 'E')) {
    return JsonObject(nullptr);
  }

  JsonObject::Integer integer = 0;
  auto result = std::from_chars(start, it, integer);
  if (result.ec != std::errc()) {
    return JsonObject(nullptr);
  }
  return JsonObject(integer);
}

JsonObject parseKeyword(const char *&it, const char *end) {
  const char *start = it;
  while (it != end && std::isalpha(static_cast<unsigned char>(*it))) {
    ++it;
  }
  std::string_view keyword(start, static_cast<size_t>(it - start));
  if (keyword == "true") {
    return JsonObject(true);
  }
  if (keyword == "false") {
    return JsonObject(false);
  }
  if (keyword == "null") {
    return JsonObject(nullptr);
  }
  return JsonObject(nullptr);
}

JsonObject parseJsonList(const char *&it, const char *end) {
  JsonObject::List list;
  jsonSkipWhitespace(it, end);
  if (it != end && *it == ']') {
    ++it;
    return JsonObject(std::move(list));
  }

  while (it != end) {
    list.push_back(parseJson(it, end));
    if (list.back().isNull() && (it == end || *(it - 1) != 'l')) {
      return JsonObject(nullptr);
    }
    jsonSkipWhitespace(it, end);
    if (it == end) {
      return JsonObject(nullptr);
    }
    if (*it == ']') {
      ++it;
      return JsonObject(std::move(list));
    }
    if (*it != ',') {
      return JsonObject(nullptr);
    }
    ++it;
    jsonSkipWhitespace(it, end);
  }

  return JsonObject(nullptr);
}

JsonObject parseJsonObject(const char *&it, const char *end) {
  JsonObject::Object object;
  jsonSkipWhitespace(it, end);
  if (it != end && *it == '}') {
    ++it;
    return JsonObject(std::move(object));
  }

  while (it != end) {
    if (*it != '"') {
      return JsonObject(nullptr);
    }
    JsonObject key = parseJsonString(++it, end);
    if (!key.isString()) {
      return JsonObject(nullptr);
    }
    jsonSkipWhitespace(it, end);
    if (it == end || *it != ':') {
      return JsonObject(nullptr);
    }
    ++it;
    JsonObject value = parseJson(it, end);
    object.emplace(key.getAsString(), std::move(value));
    jsonSkipWhitespace(it, end);
    if (it == end) {
      return JsonObject(nullptr);
    }
    if (*it == '}') {
      ++it;
      return JsonObject(std::move(object));
    }
    if (*it != ',') {
      return JsonObject(nullptr);
    }
    ++it;
    jsonSkipWhitespace(it, end);
  }

  return JsonObject(nullptr);
}

JsonObject parseJson(const char *&it, const char *end) {
  jsonSkipWhitespace(it, end);
  if (it == end) {
    return JsonObject(nullptr);
  }

  switch (*it) {
  case '{':
    return parseJsonObject(++it, end);
  case '[':
    return parseJsonList(++it, end);
  case '"':
    return parseJsonString(++it, end);
  case 't':
  case 'f':
  case 'n':
    return parseKeyword(it, end);
  default:
    if (*it == '-' || std::isdigit(static_cast<unsigned char>(*it))) {
      return parseJsonNumber(it, end);
    }
    return JsonObject(nullptr);
  }
}

void appendEscapedJsonString(std::string &out, std::string_view value) {
  out.push_back('"');
  for (unsigned char ch : value) {
    switch (ch) {
    case '\\':
      out += "\\\\";
      break;
    case '"':
      out += "\\\"";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (ch < 0x20) {
        static constexpr char hex[] = "0123456789abcdef";
        out += "\\u00";
        out.push_back(hex[(ch >> 4) & 0x0F]);
        out.push_back(hex[ch & 0x0F]);
      } else {
        out.push_back(static_cast<char>(ch));
      }
      break;
    }
  }
  out.push_back('"');
}

void stringifyJson(std::string &out, const JsonObject &json) {
  switch (json.getType()) {
  case JsonObject::ObjectType::OBJECT_T: {
    out.push_back('{');
    bool first = true;
    for (const auto &entry : json.getAsObject()) {
      if (!first) {
        out.push_back(',');
      }
      first = false;
      appendEscapedJsonString(out, entry.first);
      out.push_back(':');
      stringifyJson(out, entry.second);
    }
    out.push_back('}');
    break;
  }
  case JsonObject::ObjectType::LIST_T: {
    out.push_back('[');
    bool first = true;
    for (const auto &entry : json.getAsList()) {
      if (!first) {
        out.push_back(',');
      }
      first = false;
      stringifyJson(out, entry);
    }
    out.push_back(']');
    break;
  }
  case JsonObject::ObjectType::INTEGER_T:
    out += std::to_string(json.getAsInteger());
    break;
  case JsonObject::ObjectType::STRING_T:
    appendEscapedJsonString(out, json.getAsString());
    break;
  case JsonObject::ObjectType::BOOLEAN_T:
    out += json.getAsBoolean() ? "true" : "false";
    break;
  case JsonObject::ObjectType::NULL_T:
    out += "null";
    break;
  }
}

} // namespace

JsonObject::ObjectType JsonObject::getType() const noexcept {
  if (isObject()) return ObjectType::OBJECT_T;
  if (isList()) return ObjectType::LIST_T;
  if (isInteger()) return ObjectType::INTEGER_T;
  if (isString()) return ObjectType::STRING_T;
  if (isBoolean()) return ObjectType::BOOLEAN_T;
  return ObjectType::NULL_T;
}

JsonObject::Object &JsonObject::getAsObject() { return std::get<Object>(storage); }
JsonObject::List &JsonObject::getAsList() { return std::get<List>(storage); }
const JsonObject::Object &JsonObject::getAsObject() const { return std::get<Object>(storage); }
const JsonObject::List &JsonObject::getAsList() const { return std::get<List>(storage); }
JsonObject::Integer JsonObject::getAsInteger() const { return std::get<Integer>(storage); }
const JsonObject::String &JsonObject::getAsString() const { return std::get<String>(storage); }
JsonObject::Boolean JsonObject::getAsBoolean() const { return std::get<Boolean>(storage); }
JsonObject::Null JsonObject::getAsNull() const { return std::get<Null>(storage); }
bool JsonObject::isObject() const noexcept { return std::holds_alternative<Object>(storage); }
bool JsonObject::isList() const noexcept { return std::holds_alternative<List>(storage); }
bool JsonObject::isInteger() const noexcept { return std::holds_alternative<Integer>(storage); }
bool JsonObject::isString() const noexcept { return std::holds_alternative<String>(storage); }
bool JsonObject::isBoolean() const noexcept { return std::holds_alternative<Boolean>(storage); }
bool JsonObject::isNull() const noexcept { return std::holds_alternative<Null>(storage); }

JsonObject JsonParser::parse(std::string_view view) {
  const char *it = view.data();
  JsonObject result = parseJson(it, view.data() + view.size());
  jsonSkipWhitespace(it, view.data() + view.size());
  if (it != view.data() + view.size()) {
    return JsonObject(nullptr);
  }
  return result;
}

void JsonParser::toString(std::string &dest, const JsonObject &json) {
  stringifyJson(dest, json);
}

JsonRPC::JsonRPC(const std::string &message)
    : serialized_(message), rpc_(JsonParser::parse(serialized_)) {}

JsonRPC::JsonRPC(JsonObject object) noexcept : rpc_(std::move(object)) {}

std::string_view JsonRPC::getStr() {
  if (serialized_.empty()) {
    JsonParser::toString(serialized_, rpc_);
  }
  return serialized_;
}

constexpr std::string_view prefix = "Content-Length: ";

void Server::sendMessageRaw(std::string_view message) {
  buffer += prefix;

  char chars[std::numeric_limits<unsigned>::digits10 + 0x20];
  auto result = std::to_chars(chars, chars + sizeof(chars), message.length());

  constexpr const char *crlf = "\r\n\r\n";
  std::copy(crlf, crlf + 4, result.ptr);
  result.ptr += 4;

  buffer.append(chars, static_cast<size_t>(result.ptr - chars));
  buffer += message;
}

void Server::logMessage(MessageType type, std::string_view message) {
  JsonObject::Object params;
  params.emplace("type", JsonObject(static_cast<JsonObject::Integer>(type)));
  params.emplace("message", JsonObject(message));

  JsonObject::Object rpc;
  rpc.emplace("jsonrpc", JsonObject("2.0"));
  rpc.emplace("method", JsonObject("window/logMessage"));
  rpc.emplace("params", JsonObject(std::move(params)));

  sendMessage(JsonObject(std::move(rpc)));
}

void Server::sendMessage(const JsonObject &message) {
  std::string serialized;
  JsonParser::toString(serialized, message);
  sendMessageRaw(serialized);
}

std::string Server::processMessage(std::string &line) {
  unsigned contentLength = 0;

  while (std::getline(std::cin, line)) {
    if (line.empty() || line == "\r") {
      break;
    }
    if (line.rfind(prefix, 0) == 0) {
      std::from_chars(line.data() + prefix.size(),
                      line.data() + line.size(), contentLength);
    }
  }

  if (!std::cin || contentLength == 0) {
    return {};
  }

  std::string message(contentLength, '\0');
  std::cin.read(message.data(), static_cast<std::streamsize>(contentLength));
  if (!std::cin) {
    return {};
  }
  return message;
}

} // namespace zap::lsp
