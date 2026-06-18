#pragma once

#include "driver/args/argparse.hpp"
#include "lsp.hpp"
#include "utils/diagnostics.hpp"
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace zap::lsp {

const JsonObject *getField(const JsonObject &object, std::string_view key);
const JsonObject *getPath(const JsonObject &object,
                          std::initializer_list<std::string_view> path);
std::optional<std::string>
getStringField(const JsonObject &object,
               std::initializer_list<std::string_view> path);
std::optional<int64_t>
getIntegerField(const JsonObject &object,
                std::initializer_list<std::string_view> path);

std::optional<std::filesystem::path> uriToPath(std::string_view uri);
std::string pathToUri(const std::filesystem::path &path);
bool readSourceFile(const std::filesystem::path &path, std::string &content);
zap::args::CmdlineArgs findAndReadFlags(std::filesystem::path startPath);

size_t offsetFromPosition(const std::string &text, int64_t line,
                          int64_t character);
bool containsOffset(const SourceSpan &span, size_t offset);
bool isIdentifierChar(char ch);
std::optional<std::string> identifierAt(const std::string &source,
                                        size_t offset);
std::optional<std::pair<std::string, std::string>>
memberAccessBeforeCursor(const std::string &source, size_t offset);
std::optional<std::pair<std::string, std::string>>
qualifiedIdentifierAtOffset(const std::string &source, size_t offset);

} // namespace zap::lsp
