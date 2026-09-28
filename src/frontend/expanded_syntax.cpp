#include "frontend/expanded_syntax.hpp"

#include "macros/macro_diagnostic_codes.hpp"
#include "token/token_tree.hpp"

#include <filesystem>
#include <utility>

namespace zap::frontend {
namespace {

bool leafIs(const TokenTree &tree, TokenType type) {
  return tree.isLeaf() && tree.token().type == type;
}

size_t macroDeclarationEnd(const std::vector<TokenTree> &trees, size_t index) {
  if (index < trees.size() && (leafIs(trees[index], TokenType::PUB) ||
                               leafIs(trees[index], TokenType::PRIV)))
    ++index;
  if (index + 3 >= trees.size() || !leafIs(trees[index], TokenType::MACRO) ||
      !leafIs(trees[index + 1], TokenType::ID) || trees[index + 2].isLeaf() ||
      trees[index + 2].delimiter() != Delimiter::Parenthesis ||
      trees[index + 3].isLeaf() ||
      trees[index + 3].delimiter() != Delimiter::Brace)
    return 0;
  return index + 4;
}

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

ExpandedSyntaxEmitter::ExpandedSyntaxEmitter(const MacroResolver &macros,
                                             DiagnosticEngine &diagnostics)
    : expander_(macros, diagnostics), diagnostics_(diagnostics) {}

std::optional<std::vector<Token>>
ExpandedSyntaxEmitter::expand(const std::string &moduleId,
                              const std::vector<Token> &tokens) {
  auto grouped = TokenTreeBuilder::build(tokens, diagnostics_);
  if (grouped.hadDelimiterErrors)
    return std::nullopt;
  auto expanded = expandTrees(moduleId, grouped.trees, true, 1);
  if (!expanded)
    return std::nullopt;
  return flattenTokenTrees(*expanded);
}

std::optional<std::vector<TokenTree>>
ExpandedSyntaxEmitter::expandTrees(const std::string &moduleId,
                                   const std::vector<TokenTree> &trees,
                                   bool topLevel, size_t depth) {
  std::vector<TokenTree> output;
  for (size_t index = 0; index < trees.size();) {
    if (topLevel) {
      const size_t end = macroDeclarationEnd(trees, index);
      if (end != 0) {
        index = end;
        continue;
      }
    }

    if (leafIs(trees[index], TokenType::ID)) {
      std::vector<std::string> path{trees[index].token().value};
      size_t cursor = index + 1;
      while (cursor + 1 < trees.size() &&
             leafIs(trees[cursor], TokenType::DOT) &&
             leafIs(trees[cursor + 1], TokenType::ID)) {
        path.push_back(trees[cursor + 1].token().value);
        cursor += 2;
      }
      if (cursor + 1 < trees.size() && leafIs(trees[cursor], TokenType::NOT) &&
          !trees[cursor + 1].isLeaf()) {
        const SourceSpan span =
            SourceSpan::merge(trees[index].span(), trees[cursor + 1].span());
        if (depth > MacroLimits{}.maxDepth) {
          diagnostics_.report(span, DiagnosticLevel::Error,
                              macro_diagnostic::Limit,
                              "Macro expansion depth limit exceeded.");
          return std::nullopt;
        }
        MacroCall call{std::move(path), trees[cursor + 1], span,
                       trees[index].token().expansionOrigin};
        auto expanded = expander_.expand(moduleId, call);
        if (!expanded)
          return std::nullopt;
        auto nested = expandTrees(moduleId, *expanded, false, depth + 1);
        if (!nested)
          return std::nullopt;
        output.insert(output.end(), nested->begin(), nested->end());
        index = cursor + 2;
        continue;
      }
    }

    const TokenTree &tree = trees[index++];
    if (tree.isLeaf()) {
      output.push_back(tree);
      continue;
    }
    auto children = expandTrees(moduleId, tree.children(), false, depth);
    if (!children)
      return std::nullopt;
    output.push_back(TokenTree::group(tree.delimiter(), tree.opening(),
                                      std::move(*children), tree.closing()));
  }
  return output;
}

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
