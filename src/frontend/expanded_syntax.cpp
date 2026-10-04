#include "frontend/expanded_syntax.hpp"

#include <filesystem>
#include <utility>

namespace zap::frontend {
namespace {

std::string relativePath(const std::filesystem::path &entryDirectory,
                         const std::string &sourceName) {
  if (sourceName.empty())
    return "<unknown>";
  std::filesystem::path path(sourceName);
  const auto relative = path.lexically_relative(entryDirectory);
  return (relative.empty() ? path : relative).generic_string();
}

std::string commentSafe(std::string text) {
  for (size_t index = 0; index < text.size(); ++index) {
    if (text[index] == '\n' || text[index] == '\r' || text[index] == '\t') {
      const char escaped = text[index] == '\n'   ? 'n'
                           : text[index] == '\r' ? 'r'
                                                 : 't';
      text.replace(index, 1, std::string{"\\"} + escaped);
      ++index;
    }
  }
  size_t position = 0;
  while ((position = text.find("*/", position)) != std::string::npos) {
    text.replace(position, 2, "* /");
    position += 3;
  }
  return text;
}

std::string originLabel(const std::filesystem::path &entryDirectory,
                        const SourceSpan &span) {
  return commentSafe(relativePath(entryDirectory, span.sourceName)) + ":" +
         std::to_string(span.line) + ":" + std::to_string(span.column);
}

} // namespace

std::string ExpandedSyntaxEmitter::render(const std::string &entryModuleId,
                                          const std::string &moduleId,
                                          const std::vector<Token> &tokens) {
  const auto entryDirectory =
      std::filesystem::path(entryModuleId).parent_path();
  std::string output =
      "// module " + commentSafe(relativePath(entryDirectory, moduleId)) + "\n";
  std::shared_ptr<const ExpansionOrigin> previous;
  for (const auto &token : tokens) {
    if (token.type == TokenType::EOF_TOKEN)
      continue;
    if (token.expansionOrigin != previous) {
      if (token.expansionOrigin) {
        output +=
            "/* @macro call " +
            originLabel(entryDirectory, token.expansionOrigin->invocationSpan) +
            " define " +
            originLabel(entryDirectory, token.expansionOrigin->definitionSpan) +
            " */ ";
      } else if (previous) {
        output += "/* @source */ ";
      }
      previous = token.expansionOrigin;
    }
    output += token.spelling;
    output += ' ';
  }
  if (!tokens.empty())
    output.back() = '\n';
  return output;
}

} // namespace zap::frontend
