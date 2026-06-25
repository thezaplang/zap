#include "lsp/protocol_utils.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <vector>

namespace zap::lsp {

const JsonObject *getField(const JsonObject &object, std::string_view key) {
  if (!object.isObject()) {
    return nullptr;
  }
  auto it = object.getAsObject().find(std::string(key));
  return it == object.getAsObject().end() ? nullptr : &it->second;
}

const JsonObject *getPath(const JsonObject &object,
                          std::initializer_list<std::string_view> path) {
  const JsonObject *current = &object;
  for (std::string_view key : path) {
    current = getField(*current, key);
    if (!current) {
      return nullptr;
    }
  }
  return current;
}

std::optional<std::string>
getStringField(const JsonObject &object,
               std::initializer_list<std::string_view> path) {
  const JsonObject *value = getPath(object, path);
  if (!value || !value->isString()) {
    return std::nullopt;
  }
  return value->getAsString();
}

std::optional<int64_t>
getIntegerField(const JsonObject &object,
                std::initializer_list<std::string_view> path) {
  const JsonObject *value = getPath(object, path);
  if (!value || !value->isInteger()) {
    return std::nullopt;
  }
  return value->getAsInteger();
}

std::optional<std::filesystem::path> uriToPath(std::string_view uri) {
  constexpr std::string_view prefix = "file://";
  if (uri.rfind(prefix, 0) != 0) {
    return std::nullopt;
  }

  std::string decoded;
  decoded.reserve(uri.size() - prefix.size());
  for (size_t i = prefix.size(); i < uri.size(); ++i) {
    char ch = uri[i];
    if (ch == '%' && i + 2 < uri.size()) {
      auto hex = [](char value) -> int {
        if (value >= '0' && value <= '9') {
          return value - '0';
        }
        if (value >= 'a' && value <= 'f') {
          return 10 + value - 'a';
        }
        if (value >= 'A' && value <= 'F') {
          return 10 + value - 'A';
        }
        return -1;
      };
      int hi = hex(uri[i + 1]);
      int lo = hex(uri[i + 2]);
      if (hi >= 0 && lo >= 0) {
        decoded.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    decoded.push_back(ch);
  }

#ifdef _WIN32
  if (decoded.size() >= 3 && decoded[0] == '/' &&
      std::isalpha(static_cast<unsigned char>(decoded[1])) &&
      decoded[2] == ':') {
    decoded.erase(decoded.begin());
  }
#endif

  return std::filesystem::path(decoded).lexically_normal();
}

std::string pathToUri(const std::filesystem::path &path) {
  std::string uri = "file://";
  std::string text = path.generic_string();
  for (unsigned char ch : text) {
    bool unreserved = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                      (ch >= '0' && ch <= '9') || ch == '/' || ch == '-' ||
                      ch == '_' || ch == '.' || ch == '~';
    if (unreserved) {
      uri.push_back(static_cast<char>(ch));
    } else {
      static constexpr char hex[] = "0123456789ABCDEF";
      uri.push_back('%');
      uri.push_back(hex[(ch >> 4) & 0x0F]);
      uri.push_back(hex[ch & 0x0F]);
    }
  }
  return uri;
}

bool readSourceFile(const std::filesystem::path &path, std::string &content) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    return false;
  }

  auto size = file.tellg();
  content.assign(static_cast<size_t>(std::max<std::streamsize>(size, 0)), '\0');
  if (size > 0) {
    file.seekg(0);
    file.read(content.data(), size);
  }
  return true;
}

namespace {

zap::args::CmdlineArgs readFlagsFromFile(const std::filesystem::path &path) {
  zap::args::CmdlineArgs args;
  std::ifstream file(path);
  if (!file) {
    return args;
  }

  std::vector<std::string> argv;
  argv.push_back("zapc");
  std::string word;
  while (file >> word) {
    argv.push_back(word);
  }

  zap::args::parse(argv, args);

  auto baseDir = path.parent_path();
  for (auto &[alias, target] : args.importMap) {
    std::filesystem::path targetPath(target);
    if (!target.empty() && !targetPath.is_absolute()) {
      target = (baseDir / targetPath).lexically_normal().generic_string();
    }
  }

  return args;
}

} // namespace

zap::args::CmdlineArgs findAndReadFlags(std::filesystem::path startPath) {
  if (std::filesystem::is_regular_file(startPath)) {
    startPath = startPath.parent_path();
  }

  while (true) {
    auto flagsPath = startPath / "zap_flags.txt";
    if (std::filesystem::exists(flagsPath)) {
      return readFlagsFromFile(flagsPath);
    }
    if (startPath == startPath.parent_path()) {
      break;
    }
    startPath = startPath.parent_path();
  }
  return {};
}

size_t offsetFromPosition(const std::string &text, int64_t line,
                          int64_t character) {
  size_t offset = 0;
  int64_t currentLine = 0;
  while (offset < text.size() && currentLine < line) {
    if (text[offset++] == '\n') {
      ++currentLine;
    }
  }

  int64_t currentChar = 0;
  while (offset < text.size() && currentChar < character &&
         text[offset] != '\n') {
    ++offset;
    ++currentChar;
  }
  return offset;
}

bool containsOffset(const SourceSpan &span, size_t offset) {
  return offset >= span.offset && offset <= span.offset + span.length;
}

bool isIdentifierChar(char ch) {
  return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
}

std::optional<std::string> identifierAt(const std::string &source,
                                        size_t offset) {
  if (source.empty()) {
    return std::nullopt;
  }
  size_t pos = offset < source.size() ? offset : source.size() - 1;
  if (!isIdentifierChar(source[pos])) {
    if (pos == 0 || !isIdentifierChar(source[pos - 1])) {
      return std::nullopt;
    }
    --pos;
  }

  size_t start = pos;
  while (start > 0 && isIdentifierChar(source[start - 1])) {
    --start;
  }
  size_t end = pos + 1;
  while (end < source.size() && isIdentifierChar(source[end])) {
    ++end;
  }
  return source.substr(start, end - start);
}

std::optional<std::pair<std::string, std::string>>
memberAccessBeforeCursor(const std::string &source, size_t offset) {
  if (offset == 0 || offset > source.size()) {
    return std::nullopt;
  }
  size_t pos = offset;
  while (pos > 0 && std::isspace(static_cast<unsigned char>(source[pos - 1]))) {
    --pos;
  }
  if (pos == 0 || source[pos - 1] != '.') {
    return std::nullopt;
  }

  size_t end = pos - 1;
  size_t start = end;
  while (start > 0 && isIdentifierChar(source[start - 1])) {
    --start;
  }
  if (start == end) {
    return std::nullopt;
  }
  std::string base = source.substr(start, end - start);

  size_t left = start;
  while (left > 1) {
    size_t dot = left;
    while (dot > 0 &&
           std::isspace(static_cast<unsigned char>(source[dot - 1]))) {
      --dot;
    }
    if (dot == 0 || source[dot - 1] != '.') {
      break;
    }
    size_t nameEnd = dot - 1;
    size_t nameStart = nameEnd;
    while (nameStart > 0 && isIdentifierChar(source[nameStart - 1])) {
      --nameStart;
    }
    if (nameStart == nameEnd) {
      break;
    }
    base = source.substr(nameStart, end - nameStart);
    left = nameStart;
  }

  size_t split = base.find('.');
  if (split == std::string::npos) {
    return std::make_pair(base, std::string());
  }
  return std::make_pair(base.substr(0, split), base.substr(split + 1));
}

std::optional<std::pair<std::string, std::string>>
memberAccessAtCursor(const std::string &source, size_t offset) {
  if (offset == 0 || offset > source.size()) {
    return std::nullopt;
  }

  size_t pos = offset;
  while (pos > 0 && std::isspace(static_cast<unsigned char>(source[pos - 1]))) {
    --pos;
  }

  std::string memberPrefix;
  if (pos > 0 && isIdentifierChar(source[pos - 1])) {
    size_t prefixEnd = pos;
    size_t prefixStart = prefixEnd;
    while (prefixStart > 0 && isIdentifierChar(source[prefixStart - 1])) {
      --prefixStart;
    }
    memberPrefix = source.substr(prefixStart, prefixEnd - prefixStart);
    pos = prefixStart;
  }

  if (pos == 0 || source[pos - 1] != '.') {
    return std::nullopt;
  }

  size_t end = pos - 1;
  size_t start = end;
  while (start > 0 && isIdentifierChar(source[start - 1])) {
    --start;
  }
  if (start == end) {
    return std::nullopt;
  }
  std::string base = source.substr(start, end - start);

  size_t left = start;
  while (left > 1) {
    size_t dot = left;
    while (dot > 0 &&
           std::isspace(static_cast<unsigned char>(source[dot - 1]))) {
      --dot;
    }
    if (dot == 0 || source[dot - 1] != '.') {
      break;
    }
    size_t nameEnd = dot - 1;
    size_t nameStart = nameEnd;
    while (nameStart > 0 && isIdentifierChar(source[nameStart - 1])) {
      --nameStart;
    }
    if (nameStart == nameEnd) {
      break;
    }
    base = source.substr(nameStart, end - nameStart);
    left = nameStart;
  }

  size_t split = base.find('.');
  if (split != std::string::npos) {
    base = base.substr(0, split);
  }
  return std::make_pair(base, memberPrefix);
}

std::optional<std::pair<std::string, std::string>>
qualifiedIdentifierAtOffset(const std::string &source, size_t offset) {
  if (source.empty()) {
    return std::nullopt;
  }
  size_t pos = offset < source.size() ? offset : source.size() - 1;
  if (!isIdentifierChar(source[pos])) {
    if (pos == 0 || !isIdentifierChar(source[pos - 1])) {
      return std::nullopt;
    }
    --pos;
  }

  size_t start = pos;
  while (start > 0 && isIdentifierChar(source[start - 1])) {
    --start;
  }
  size_t end = pos + 1;
  while (end < source.size() && isIdentifierChar(source[end])) {
    ++end;
  }

  while (start > 1) {
    size_t dot = start;
    while (dot > 0 &&
           std::isspace(static_cast<unsigned char>(source[dot - 1]))) {
      --dot;
    }
    if (dot == 0 || source[dot - 1] != '.') {
      break;
    }
    size_t nameEnd = dot - 1;
    size_t nameStart = nameEnd;
    while (nameStart > 0 && isIdentifierChar(source[nameStart - 1])) {
      --nameStart;
    }
    if (nameStart == nameEnd) {
      break;
    }
    start = nameStart;
  }

  while (end < source.size()) {
    size_t dot = end;
    while (dot < source.size() &&
           std::isspace(static_cast<unsigned char>(source[dot]))) {
      ++dot;
    }
    if (dot >= source.size() || source[dot] != '.') {
      break;
    }
    size_t nameStart = dot + 1;
    while (nameStart < source.size() &&
           std::isspace(static_cast<unsigned char>(source[nameStart]))) {
      ++nameStart;
    }
    size_t nameEnd = nameStart;
    while (nameEnd < source.size() && isIdentifierChar(source[nameEnd])) {
      ++nameEnd;
    }
    if (nameEnd == nameStart) {
      break;
    }
    end = nameEnd;
  }

  std::string text = source.substr(start, end - start);
  size_t split = text.find('.');
  if (split == std::string::npos) {
    return std::make_pair(text, std::string());
  }
  return std::make_pair(text.substr(0, split), text.substr(split + 1));
}

} // namespace zap::lsp
