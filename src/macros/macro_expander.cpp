#include "macros/macro_expander.hpp"
#include "macros/macro_diagnostic_codes.hpp"
#include "macros/macro_template.hpp"

#include "parser/parser.hpp"

#include <functional>
#include <limits>
#include <map>
#include <utility>

namespace zap {
namespace {

bool isLeaf(const TokenTree &tree, TokenType type) {
  return tree.isLeaf() && tree.token().type == type;
}

std::optional<MacroCapture> splitArguments(const TokenTree &group) {
  if (group.isLeaf() || !group.closing())
    return std::nullopt;
  MacroCapture arguments;
  if (group.children().empty())
    return arguments;

  std::vector<TokenTree> current;
  for (const auto &tree : group.children()) {
    if (isLeaf(tree, TokenType::COMMA)) {
      if (current.empty())
        return std::nullopt;
      arguments.elements.push_back(std::move(current));
      arguments.separators.push_back(tree);
      current.clear();
    } else {
      current.push_back(tree);
    }
  }
  if (!current.empty())
    arguments.elements.push_back(std::move(current));
  return arguments;
}

std::optional<FragmentKind> fragmentKind(MacroParameterKind kind) {
  switch (kind) {
  case MacroParameterKind::Expression:
    return FragmentKind::Expression;
  case MacroParameterKind::Type:
    return FragmentKind::Type;
  case MacroParameterKind::Statement:
    return FragmentKind::Statement;
  case MacroParameterKind::Block:
    return FragmentKind::Block;
  case MacroParameterKind::Item:
    return FragmentKind::Item;
  case MacroParameterKind::Identifier:
  case MacroParameterKind::Literal:
  case MacroParameterKind::Tokens:
    return std::nullopt;
  }
  return std::nullopt;
}

bool isLiteral(TokenType type) {
  return type == TokenType::INTEGER || type == TokenType::FLOAT ||
         type == TokenType::STRING || type == TokenType::CHAR ||
         type == TokenType::BOOL || type == TokenType::NULL_LITERAL;
}

bool matchesArgument(const std::vector<TokenTree> &argument,
                     MacroParameterKind kind,
                     const DiagnosticEngine &diagnostics) {
  if (kind == MacroParameterKind::Tokens)
    return true;
  if (kind == MacroParameterKind::Identifier)
    return argument.size() == 1 && isLeaf(argument.front(), TokenType::ID);
  if (kind == MacroParameterKind::Literal)
    return argument.size() == 1 && argument.front().isLeaf() &&
           isLiteral(argument.front().token().type);

  const auto expected = fragmentKind(kind);
  if (!expected)
    return false;
  DiagnosticEngine scratch(diagnostics.sourceText(), diagnostics.sourceName());
  Parser parser(flattenTokenTrees(argument), scratch, nullptr, {},
                MacroParseMode::ValidateFragmentSyntax);
  return parser.parseFragment(*expected).has_value();
}

int specificity(MacroParameterKind kind) {
  switch (kind) {
  case MacroParameterKind::Identifier:
  case MacroParameterKind::Literal:
  case MacroParameterKind::Block:
    return 2;
  case MacroParameterKind::Expression:
  case MacroParameterKind::Type:
  case MacroParameterKind::Statement:
  case MacroParameterKind::Item:
    return 1;
  case MacroParameterKind::Tokens:
    return 0;
  }
  return 0;
}

const char *fragmentName(MacroParameterKind kind) {
  switch (kind) {
  case MacroParameterKind::Identifier:
    return "ident";
  case MacroParameterKind::Literal:
    return "literal";
  case MacroParameterKind::Expression:
    return "expr";
  case MacroParameterKind::Type:
    return "type";
  case MacroParameterKind::Statement:
    return "stmt";
  case MacroParameterKind::Block:
    return "block";
  case MacroParameterKind::Item:
    return "item";
  case MacroParameterKind::Tokens:
    return "tokens";
  }
  return "fragment";
}

} // namespace

MacroExpander::MacroExpander(const MacroResolver &registry,
                             DiagnosticEngine &diagnostics, MacroLimits limits)
    : registry_(registry), diagnostics_(diagnostics), limits_(limits) {}

std::optional<std::vector<TokenTree>>
MacroExpander::expand(const std::string &moduleId, const MacroCall &call) {
  return expandCall(moduleId, moduleId, call, 1);
}

std::optional<std::vector<TokenTree>>
MacroExpander::expandCall(const std::string &lookupModuleId,
                          const std::string &outputModuleId,
                          const MacroCall &call, size_t depth) {
  if (depth > limits_.maxDepth) {
    report(call.span, macro_diagnostic::Limit,
           "Macro expansion depth limit exceeded.");
    return std::nullopt;
  }
  if (call.path.empty() || call.path.size() > 2) {
    report(call.span, macro_diagnostic::Resolution,
           "Expected a macro name or module-qualified macro name.");
    return std::nullopt;
  }
  const auto *overloads =
      call.path.size() == 1
          ? registry_.find(lookupModuleId, call.path.front())
          : registry_.findQualified(lookupModuleId, call.path.front(),
                                    call.path[1]);
  if (!overloads) {
    report(call.span, macro_diagnostic::Resolution,
           "Unknown or private macro '" + call.path.back() + "'.");
    return std::nullopt;
  }
  const auto arguments = splitArguments(call.arguments);
  if (!arguments) {
    report(call.span, macro_diagnostic::Arguments,
           "Macro arguments must be a balanced, comma-separated group.");
    return std::nullopt;
  }

  const MacroBinding *selected = nullptr;
  size_t bestScore = 0;
  bool bestIsFixed = false;
  bool ambiguous = false;
  size_t attempts = 0;
  size_t matchingArity = 0;
  std::optional<std::pair<size_t, MacroParameterKind>> mismatch;
  for (const auto &candidate : *overloads) {
    if (++attempts > limits_.maxMatchAttempts) {
      report(call.span, macro_diagnostic::Limit,
             "Macro matcher attempt limit exceeded.");
      return std::nullopt;
    }
    const auto &parameters = candidate.definition->parameters;
    const bool isVariadic = !parameters.empty() && parameters.back().isVariadic;
    const size_t fixedCount = parameters.size() - (isVariadic ? 1 : 0);
    if (arguments->elements.size() < fixedCount ||
        (!isVariadic && arguments->elements.size() != fixedCount))
      continue;
    ++matchingArity;
    size_t score = 0;
    bool matches = true;
    for (size_t index = 0; index < arguments->elements.size(); ++index) {
      const auto &parameter =
          index < fixedCount ? parameters[index] : parameters.back();
      if (!matchesArgument(arguments->elements[index], parameter.kind,
                           diagnostics_)) {
        matches = false;
        if (matchingArity == 1)
          mismatch = std::make_pair(index, parameter.kind);
        break;
      }
      score += specificity(parameter.kind);
    }
    if (!matches)
      continue;
    const bool isFixed = !isVariadic;
    if (!selected || (isFixed && !bestIsFixed) ||
        (isFixed == bestIsFixed && score > bestScore)) {
      selected = &candidate;
      bestScore = score;
      bestIsFixed = isFixed;
      ambiguous = false;
    } else if (isFixed == bestIsFixed && score == bestScore) {
      ambiguous = true;
    }
  }
  if (ambiguous) {
    report(call.span, macro_diagnostic::Arguments,
           "Ambiguous macro overload for '" + call.path.back() + "'.");
    return std::nullopt;
  }
  if (!selected) {
    if (matchingArity == 1 && mismatch) {
      report(call.span, macro_diagnostic::Arguments,
             "Argument " + std::to_string(mismatch->first + 1) +
                 " for macro '" + call.path.back() + "' is not a valid " +
                 fragmentName(mismatch->second) + " fragment.");
    } else {
      report(call.span, macro_diagnostic::Arguments,
             "No matching macro overload for '" + call.path.back() + "' with " +
                 std::to_string(arguments->elements.size()) + " argument(s).");
    }
    return std::nullopt;
  }

  MacroCaptures captures;
  const auto &parameters = selected->definition->parameters;
  for (size_t index = 0; index < parameters.size(); ++index) {
    MacroCapture capture;
    capture.kind = parameters[index].kind;
    capture.isVariadic = parameters[index].isVariadic;
    if (parameters[index].isVariadic) {
      capture.elements.insert(capture.elements.end(),
                              arguments->elements.begin() + index,
                              arguments->elements.end());
      if (!capture.elements.empty()) {
        capture.separators.insert(capture.separators.end(),
                                  arguments->separators.begin() + index,
                                  arguments->separators.end());
      }
    } else {
      capture.elements.push_back(arguments->elements[index]);
    }
    captures.emplace(parameters[index].name.value, std::move(capture));
  }
  if (nextFreshContext_ == std::numeric_limits<SyntaxContextId>::max()) {
    report(call.span, macro_diagnostic::Limit,
           "Macro syntax context limit exceeded.");
    return std::nullopt;
  }
  auto origin = std::make_shared<ExpansionOrigin>(
      ExpansionOrigin{call.span, selected->definition->span, call.parentOrigin,
                      selected->definingModuleId, nextFreshContext_++});
  auto &generatedTokens = generatedTokensByModule_[outputModuleId];
  if (generatedTokens > limits_.maxGeneratedTokens) {
    report(call.span, macro_diagnostic::Limit,
           "Macro generated token limit exceeded.");
    return std::nullopt;
  }
  MacroTemplateExpander templateExpander(
      captures, call.span, origin, diagnostics_,
      limits_.maxGeneratedTokens - generatedTokens,
      limits_.maxTemplateIterations,
      [this]() -> std::optional<SyntaxContextId> {
        if (nextFreshContext_ == std::numeric_limits<SyntaxContextId>::max())
          return std::nullopt;
        return nextFreshContext_++;
      });
  auto output =
      templateExpander.expand(selected->definition->expansion.children());
  if (!output)
    return std::nullopt;
  size_t count = 0;
  for (const auto &tree : *output) {
    if (tree.tokenCount() > limits_.maxGeneratedTokens - count) {
      report(call.span, macro_diagnostic::Limit,
             "Macro generated token limit exceeded.");
      return std::nullopt;
    }
    count += tree.tokenCount();
  }
  if (generatedTokens > limits_.maxGeneratedTokens - count) {
    report(call.span, macro_diagnostic::Limit,
           "Macro generated token limit exceeded.");
    return std::nullopt;
  }
  generatedTokens += count;
  return expandGenerated(*output, selected->definingModuleId, outputModuleId,
                         origin, depth);
}

std::optional<std::vector<TokenTree>> MacroExpander::expandGenerated(
    const std::vector<TokenTree> &trees, const std::string &definitionModuleId,
    const std::string &outputModuleId,
    const std::shared_ptr<const ExpansionOrigin> &origin, size_t depth) {
  std::vector<TokenTree> output;
  for (size_t index = 0; index < trees.size();) {
    if (isLeaf(trees[index], TokenType::ID) &&
        trees[index].token().expansionOrigin == origin) {
      size_t groupIndex = index + 2;
      std::vector<std::string> path{trees[index].token().value};
      if (index + 3 < trees.size() &&
          isLeaf(trees[index + 1], TokenType::DOT) &&
          isLeaf(trees[index + 2], TokenType::ID)) {
        path.push_back(trees[index + 2].token().value);
        groupIndex = index + 4;
      }
      if (groupIndex < trees.size() &&
          isLeaf(trees[groupIndex - 1], TokenType::NOT) &&
          !trees[groupIndex].isLeaf()) {
        const MacroCall nested{
            path, trees[groupIndex],
            SourceSpan::merge(trees[index].span(), trees[groupIndex].span()),
            origin};
        auto expansion =
            expandCall(definitionModuleId, outputModuleId, nested, depth + 1);
        if (!expansion)
          return std::nullopt;
        output.insert(output.end(), expansion->begin(), expansion->end());
        index = groupIndex + 1;
        continue;
      }
    }

    const TokenTree &tree = trees[index++];
    if (tree.isLeaf()) {
      output.push_back(tree);
      continue;
    }
    auto children = expandGenerated(tree.children(), definitionModuleId,
                                    outputModuleId, origin, depth);
    if (!children)
      return std::nullopt;
    output.push_back(TokenTree::group(tree.delimiter(), tree.opening(),
                                      std::move(*children), tree.closing()));
  }
  return output;
}

void MacroExpander::report(const SourceSpan &span, const char *code,
                           const std::string &message) {
  diagnostics_.report(span, DiagnosticLevel::Error, code, message);
}

} // namespace zap
