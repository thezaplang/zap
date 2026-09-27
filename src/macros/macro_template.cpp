#include "macros/macro_template.hpp"

#include <utility>
#include <variant>

namespace zap {
namespace {

bool isLeaf(const TokenTree &tree, TokenType type) {
  return tree.isLeaf() && tree.token().type == type;
}

bool isName(const TokenTree &tree, const char *name) {
  return isLeaf(tree, TokenType::ID) && tree.token().value == name;
}

bool isGroup(const TokenTree &tree, Delimiter delimiter) {
  return !tree.isLeaf() && tree.delimiter() == delimiter && tree.closing();
}

bool startsMetaLoop(const std::vector<TokenTree> &trees, size_t index) {
  if (!isLeaf(trees[index], TokenType::FOR) || index + 1 >= trees.size())
    return false;
  if (isLeaf(trees[index + 1], TokenType::DOLLAR))
    return true;
  const TokenTree &next = trees[index + 1];
  return isGroup(next, Delimiter::Parenthesis) && !next.children().empty() &&
         isLeaf(next.children().front(), TokenType::DOLLAR);
}

Token generatedToken(const Token &source, const SourceSpan &invocation,
                     const std::shared_ptr<const ExpansionOrigin> &origin) {
  Token token = source;
  token.span = invocation;
  token.expansionOrigin = origin;
  return token;
}

} // namespace

struct MacroTemplateExpander::Scope {
  struct Position {
    size_t index;
    size_t count;
  };
  using Binding = std::variant<const MacroCapture *,
                               const std::vector<TokenTree> *, Position>;

  const Scope *parent = nullptr;
  std::map<std::string, Binding> bindings;

  const Binding *find(const std::string &name) const {
    const auto it = bindings.find(name);
    if (it != bindings.end())
      return &it->second;
    return parent ? parent->find(name) : nullptr;
  }
};

struct MacroTemplateExpander::Loop {
  std::string elementName;
  std::optional<std::string> positionName;
  std::string packName;
  const TokenTree *separator = nullptr;
  const TokenTree *body = nullptr;
  size_t nextIndex = 0;
};

MacroTemplateExpander::MacroTemplateExpander(
    const MacroCaptures &captures, const SourceSpan &invocation,
    std::shared_ptr<const ExpansionOrigin> origin,
    DiagnosticEngine &diagnostics, size_t maxTokens, size_t maxIterations)
    : captures_(captures), invocation_(invocation), origin_(std::move(origin)),
      diagnostics_(diagnostics), maxTokens_(maxTokens),
      maxIterations_(maxIterations) {}

std::optional<std::vector<TokenTree>>
MacroTemplateExpander::expand(const std::vector<TokenTree> &templateTrees) {
  Scope root;
  for (const auto &[name, capture] : captures_)
    root.bindings.emplace(name, &capture);
  if (!validateTrees(templateTrees, root))
    return std::nullopt;
  return expandTrees(templateTrees, root);
}

std::optional<MacroTemplateExpander::Loop>
MacroTemplateExpander::parseLoop(const std::vector<TokenTree> &trees,
                                 size_t start) {
  Loop loop;
  size_t index = start + 1;
  if (isLeaf(trees[index], TokenType::DOLLAR)) {
    if (++index >= trees.size() || !isLeaf(trees[index], TokenType::ID)) {
      report("Expected an element name after 'for $'.");
      return std::nullopt;
    }
    loop.elementName = trees[index++].token().value;
  } else {
    const auto &bindings = trees[index++].children();
    if (bindings.size() != 5 || !isLeaf(bindings[0], TokenType::DOLLAR) ||
        !isLeaf(bindings[1], TokenType::ID) ||
        !isLeaf(bindings[2], TokenType::COMMA) ||
        !isLeaf(bindings[3], TokenType::DOLLAR) ||
        !isLeaf(bindings[4], TokenType::ID)) {
      report("Expected 'for ($element, $position)' in macro template.");
      return std::nullopt;
    }
    loop.elementName = bindings[1].token().value;
    loop.positionName = bindings[4].token().value;
    if (loop.elementName == *loop.positionName) {
      report("Loop element and position must have different names.");
      return std::nullopt;
    }
  }

  if (index >= trees.size() || !isName(trees[index], "in") ||
      index + 2 >= trees.size() ||
      !isLeaf(trees[index + 1], TokenType::DOLLAR) ||
      !isLeaf(trees[index + 2], TokenType::ID)) {
    report("Expected 'in $pack' in macro template for loop.");
    return std::nullopt;
  }
  loop.packName = trees[index + 2].token().value;
  index += 3;

  if (index < trees.size() && isName(trees[index], "separated")) {
    if (index + 2 >= trees.size() || !isName(trees[index + 1], "by") ||
        !isGroup(trees[index + 2], Delimiter::Brace)) {
      report("Expected 'separated by { tokens }' in macro template.");
      return std::nullopt;
    }
    loop.separator = &trees[index + 2];
    index += 3;
  }
  if (index >= trees.size() || !isGroup(trees[index], Delimiter::Brace)) {
    report("Expected '{ template }' after macro template for loop.");
    return std::nullopt;
  }
  loop.body = &trees[index];
  loop.nextIndex = index + 1;
  return loop;
}

bool MacroTemplateExpander::validateTrees(const std::vector<TokenTree> &trees,
                                          const Scope &scope) {
  for (size_t index = 0; index < trees.size();) {
    const TokenTree &tree = trees[index];
    if (startsMetaLoop(trees, index)) {
      auto loop = parseLoop(trees, index);
      if (!loop)
        return false;
      const auto *binding = scope.find(loop->packName);
      const auto *pack =
          binding ? std::get_if<const MacroCapture *>(binding) : nullptr;
      if (!pack || !(*pack)->isVariadic) {
        report("'$" + loop->packName + "' is not a variadic pack.");
        return false;
      }
      const std::vector<TokenTree> placeholder;
      Scope iteration;
      iteration.parent = &scope;
      iteration.bindings.emplace(loop->elementName, &placeholder);
      if (loop->positionName)
        iteration.bindings.emplace(*loop->positionName, Scope::Position{0, 1});
      if (!validateTrees(loop->body->children(), iteration) ||
          (loop->separator &&
           !validateTrees(loop->separator->children(), iteration)))
        return false;
      index = loop->nextIndex;
      continue;
    }

    if (isLeaf(tree, TokenType::DOLLAR)) {
      if (index + 1 >= trees.size() ||
          !isLeaf(trees[index + 1], TokenType::ID)) {
        report("Expected a capture name after '$'.");
        return false;
      }
      const std::string &name = trees[index + 1].token().value;
      const auto *binding = scope.find(name);
      if (!binding) {
        report("Unknown macro capture '$" + name + "'.");
        return false;
      }
      if (std::holds_alternative<Scope::Position>(*binding)) {
        if (index + 3 >= trees.size() ||
            !isLeaf(trees[index + 2], TokenType::DOT) ||
            !isLeaf(trees[index + 3], TokenType::ID)) {
          report("Expected a position property after '$" + name + "'.");
          return false;
        }
        const std::string &property = trees[index + 3].token().value;
        if (property != "index" && property != "isFirst" &&
            property != "isLast") {
          report("Unknown loop position property '" + property + "'.");
          return false;
        }
        index += 4;
      } else {
        index += 2;
      }
      continue;
    }

    if (!tree.isLeaf() && !validateTrees(tree.children(), scope))
      return false;
    ++index;
  }
  return true;
}

std::optional<std::vector<TokenTree>>
MacroTemplateExpander::expandTrees(const std::vector<TokenTree> &trees,
                                   const Scope &scope) {
  std::vector<TokenTree> output;
  for (size_t index = 0; index < trees.size();) {
    const TokenTree &tree = trees[index];
    if (startsMetaLoop(trees, index)) {
      auto loop = parseLoop(trees, index);
      if (!loop)
        return std::nullopt;
      const auto *binding = scope.find(loop->packName);
      const auto *pack =
          binding ? std::get_if<const MacroCapture *>(binding) : nullptr;
      if (!pack || !(*pack)->isVariadic) {
        report("'$" + loop->packName + "' is not a variadic pack.");
        return std::nullopt;
      }
      const auto &elements = (*pack)->elements;
      for (size_t element = 0; element < elements.size(); ++element) {
        if (expandedIterations_ == maxIterations_) {
          report("Macro template iteration limit exceeded.");
          return std::nullopt;
        }
        ++expandedIterations_;
        Scope iteration;
        iteration.parent = &scope;
        iteration.bindings.emplace(loop->elementName, &elements[element]);
        if (loop->positionName) {
          iteration.bindings.emplace(*loop->positionName,
                                     Scope::Position{element, elements.size()});
        }
        auto body = expandTrees(loop->body->children(), iteration);
        if (!body)
          return std::nullopt;
        output.insert(output.end(), body->begin(), body->end());
        if (loop->separator && element + 1 < elements.size()) {
          auto separator = expandTrees(loop->separator->children(), iteration);
          if (!separator)
            return std::nullopt;
          output.insert(output.end(), separator->begin(), separator->end());
        }
      }
      index = loop->nextIndex;
      continue;
    }

    if (isLeaf(tree, TokenType::DOLLAR)) {
      if (index + 1 >= trees.size() ||
          !isLeaf(trees[index + 1], TokenType::ID)) {
        report("Expected a capture name after '$'.");
        return std::nullopt;
      }
      const std::string &name = trees[index + 1].token().value;
      const auto *binding = scope.find(name);
      if (!binding) {
        report("Unknown macro capture '$" + name + "'.");
        return std::nullopt;
      }
      if (const auto *capture = std::get_if<const MacroCapture *>(binding)) {
        for (size_t element = 0; element < (*capture)->elements.size();
             ++element) {
          for (const auto &token : (*capture)->elements[element]) {
            if (!reserve(token.tokenCount()))
              return std::nullopt;
            output.push_back(token);
          }
          if (element + 1 < (*capture)->elements.size()) {
            if (!reserve((*capture)->separators[element].tokenCount()))
              return std::nullopt;
            output.push_back((*capture)->separators[element]);
          }
        }
        index += 2;
        continue;
      }
      if (const auto *fragment =
              std::get_if<const std::vector<TokenTree> *>(binding)) {
        for (const auto &token : **fragment) {
          if (!reserve(token.tokenCount()))
            return std::nullopt;
          output.push_back(token);
        }
        index += 2;
        continue;
      }
      const auto &position = std::get<Scope::Position>(*binding);
      if (index + 3 >= trees.size() ||
          !isLeaf(trees[index + 2], TokenType::DOT) ||
          !isLeaf(trees[index + 3], TokenType::ID)) {
        report("Expected a position property after '$" + name + "'.");
        return std::nullopt;
      }
      const std::string &property = trees[index + 3].token().value;
      TokenType type = TokenType::BOOL;
      std::string value;
      if (property == "index") {
        type = TokenType::INTEGER;
        value = std::to_string(position.index);
      } else if (property == "isFirst") {
        value = position.index == 0 ? "true" : "false";
      } else if (property == "isLast") {
        value = position.index + 1 == position.count ? "true" : "false";
      } else {
        report("Unknown loop position property '" + property + "'.");
        return std::nullopt;
      }
      if (!reserve(1))
        return std::nullopt;
      output.push_back(TokenTree::leaf(Token(type, value, invocation_, value,
                                             ROOT_SYNTAX_CONTEXT, origin_)));
      index += 4;
      continue;
    }

    if (tree.isLeaf()) {
      if (!reserve(1))
        return std::nullopt;
      output.push_back(
          TokenTree::leaf(generatedToken(tree.token(), invocation_, origin_)));
    } else {
      auto children = expandTrees(tree.children(), scope);
      if (!children || !reserve(1 + (tree.closing() ? 1 : 0)))
        return std::nullopt;
      std::optional<Token> closing;
      if (tree.closing())
        closing = generatedToken(*tree.closing(), invocation_, origin_);
      output.push_back(
          TokenTree::group(tree.delimiter(),
                           generatedToken(tree.opening(), invocation_, origin_),
                           std::move(*children), std::move(closing)));
    }
    ++index;
  }
  return output;
}

bool MacroTemplateExpander::reserve(size_t count) {
  if (count > maxTokens_ - emittedTokens_) {
    report("Macro generated token limit exceeded.");
    return false;
  }
  emittedTokens_ += count;
  return true;
}

void MacroTemplateExpander::report(const std::string &message) {
  diagnostics_.report(invocation_, DiagnosticLevel::Error, message);
}

} // namespace zap
