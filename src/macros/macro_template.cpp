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

bool isWhen(const TokenTree &tree) { return isName(tree, "when"); }

size_t nextBrace(const std::vector<TokenTree> &trees, size_t from) {
  while (from < trees.size() && !isGroup(trees[from], Delimiter::Brace))
    ++from;
  return from;
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

bool usesMetaProperty(const MetaValue &value, const std::string &name) {
  if (!std::holds_alternative<MetaFragment>(value))
    return true;
  return name == "kind" || name == "span" || name == "source" ||
         name == "isIdent" || name == "isLiteral" || name == "text";
}

bool startsMetaCase(const std::vector<TokenTree> &trees, size_t index,
                    const MetaScope &scope) {
  if (!isLeaf(trees[index], TokenType::CASE) || index + 2 >= trees.size() ||
      !isLeaf(trees[index + 1], TokenType::DOLLAR) ||
      !isLeaf(trees[index + 2], TokenType::ID))
    return false;
  const auto *binding = scope.find(trees[index + 2].token().value);
  if (!binding)
    return true;
  if (index + 4 < trees.size() && isLeaf(trees[index + 3], TokenType::DOT) &&
      isLeaf(trees[index + 4], TokenType::ID) &&
      usesMetaProperty(*binding, trees[index + 4].token().value))
    return true;
  return !std::holds_alternative<MetaFragment>(*binding);
}

bool sameMetaValue(const MetaValue &left, const MetaValue &right) {
  if (const auto *kind = std::get_if<MetaSyntaxKind>(&left)) {
    if (const auto *other = std::get_if<MetaSyntaxKind>(&right))
      return kind->name == other->name;
    if (const auto *other = std::get_if<std::string>(&right))
      return kind->name == *other;
  }
  if (const auto *text = std::get_if<std::string>(&left)) {
    if (const auto *other = std::get_if<std::string>(&right))
      return *text == *other;
    if (const auto *other = std::get_if<MetaSyntaxKind>(&right))
      return *text == other->name;
  }
  if (const auto *integer = std::get_if<int64_t>(&left)) {
    if (const auto *other = std::get_if<int64_t>(&right))
      return *integer == *other;
  }
  if (const auto *boolean = std::get_if<bool>(&left)) {
    if (const auto *other = std::get_if<bool>(&right))
      return *boolean == *other;
  }
  return false;
}

std::optional<DiagnosticLevel> metaDiagnosticLevel(const TokenTree &tree) {
  if (!tree.isLeaf() || tree.token().type != TokenType::ID)
    return std::nullopt;
  if (tree.token().value == "compileError")
    return DiagnosticLevel::Error;
  if (tree.token().value == "compileWarning")
    return DiagnosticLevel::Warning;
  if (tree.token().value == "compileNote")
    return DiagnosticLevel::Note;
  return std::nullopt;
}

Token generatedToken(const Token &source, const SourceSpan &invocation,
                     const std::shared_ptr<const ExpansionOrigin> &origin) {
  Token token = source;
  token.span = invocation;
  token.syntaxContext = origin->mark;
  token.expansionOrigin = origin;
  return token;
}

} // namespace

struct MacroTemplateExpander::Loop {
  std::string elementName;
  std::optional<std::string> positionName;
  std::string packName;
  const TokenTree *separator = nullptr;
  const TokenTree *body = nullptr;
  size_t nextIndex = 0;
};

struct MacroTemplateExpander::When {
  std::vector<TokenTree> condition;
  const TokenTree *thenBody = nullptr;
  const TokenTree *elseBody = nullptr;
  size_t nextIndex = 0;
};

struct MacroTemplateExpander::Case {
  struct Arm {
    std::vector<TokenTree> label;
    const TokenTree *body = nullptr;
    bool isElse = false;
  };
  std::vector<TokenTree> subject;
  std::vector<Arm> arms;
  size_t nextIndex = 0;
};

struct MacroTemplateExpander::Let {
  std::string name;
  std::vector<TokenTree> value;
  size_t nextIndex = 0;
};

MacroTemplateExpander::MacroTemplateExpander(
    const MacroCaptures &captures, const SourceSpan &invocation,
    std::shared_ptr<const ExpansionOrigin> origin,
    DiagnosticEngine &diagnostics, size_t maxTokens, size_t maxIterations,
    MacroMetaEvaluator::FreshContext freshContext)
    : captures_(captures), invocation_(invocation), origin_(std::move(origin)),
      diagnostics_(diagnostics), maxTokens_(maxTokens),
      maxIterations_(maxIterations),
      meta_(diagnostics, invocation, origin_, std::move(freshContext)) {}

std::optional<std::vector<TokenTree>>
MacroTemplateExpander::expand(const std::vector<TokenTree> &templateTrees) {
  MetaScope root;
  for (const auto &[name, capture] : captures_) {
    if (capture.isVariadic)
      root.bindings.emplace(name, &capture);
    else
      root.bindings.emplace(
          name, MetaFragment{&capture.elements.front(), capture.kind, nullptr});
  }
  if (!validateTrees(templateTrees, root))
    return std::nullopt;
  auto result = expandTrees(templateTrees, root);
  if (!result)
    return std::nullopt;
  if (result->flow != Flow::Normal) {
    report("Loop control is only valid inside a compile-time for loop.");
    return std::nullopt;
  }
  return std::move(result->trees);
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

std::optional<MacroTemplateExpander::When>
MacroTemplateExpander::parseWhen(const std::vector<TokenTree> &trees,
                                 size_t start) {
  const size_t bodyIndex = nextBrace(trees, start + 1);
  if (bodyIndex == trees.size() || bodyIndex == start + 1) {
    report("Expected 'when condition { template }'.");
    return std::nullopt;
  }
  When result;
  result.condition.assign(trees.begin() + start + 1, trees.begin() + bodyIndex);
  result.thenBody = &trees[bodyIndex];
  result.nextIndex = bodyIndex + 1;
  if (result.nextIndex < trees.size() &&
      isLeaf(trees[result.nextIndex], TokenType::ELSE)) {
    if (result.nextIndex + 1 >= trees.size() ||
        !isGroup(trees[result.nextIndex + 1], Delimiter::Brace)) {
      report("Expected '{ template }' after 'else'.");
      return std::nullopt;
    }
    result.elseBody = &trees[result.nextIndex + 1];
    result.nextIndex += 2;
  }
  return result;
}

std::optional<MacroTemplateExpander::Case>
MacroTemplateExpander::parseCase(const std::vector<TokenTree> &trees,
                                 size_t start) {
  const size_t bodyIndex = nextBrace(trees, start + 1);
  if (bodyIndex == trees.size() || bodyIndex == start + 1) {
    report("Expected 'case value { arms }' in macro template.");
    return std::nullopt;
  }
  Case result;
  result.subject.assign(trees.begin() + start + 1, trees.begin() + bodyIndex);
  result.nextIndex = bodyIndex + 1;
  const auto &children = trees[bodyIndex].children();
  for (size_t index = 0; index < children.size();) {
    Case::Arm arm;
    if (isLeaf(children[index], TokenType::ELSE)) {
      arm.isElse = true;
      ++index;
    } else if (children[index].isLeaf() &&
               (children[index].token().type == TokenType::ID ||
                children[index].token().type == TokenType::INTEGER ||
                children[index].token().type == TokenType::BOOL ||
                children[index].token().type == TokenType::STRING)) {
      arm.label.push_back(children[index++]);
    } else {
      report("Expected a compile-time case label.");
      return std::nullopt;
    }
    if (index >= children.size() ||
        !isGroup(children[index], Delimiter::Brace)) {
      report("Expected '{ template }' after compile-time case label.");
      return std::nullopt;
    }
    arm.body = &children[index++];
    if (arm.isElse && index != children.size()) {
      report("Compile-time case 'else' must be the final arm.");
      return std::nullopt;
    }
    result.arms.push_back(std::move(arm));
  }
  return result;
}

std::optional<MacroTemplateExpander::Let>
MacroTemplateExpander::parseLet(const std::vector<TokenTree> &trees,
                                size_t start) {
  if (start + 3 >= trees.size() || !isLeaf(trees[start + 2], TokenType::ID) ||
      !isLeaf(trees[start + 3], TokenType::ASSIGN)) {
    report("Expected '$let name = value;'.");
    return std::nullopt;
  }
  const size_t end = [&] {
    size_t index = start + 4;
    while (index < trees.size() && !isLeaf(trees[index], TokenType::SEMICOLON))
      ++index;
    return index;
  }();
  if (end == trees.size() || end == start + 4) {
    report("Expected a value and ';' after '$let'.");
    return std::nullopt;
  }
  Let result;
  result.name = trees[start + 2].token().value;
  result.value.assign(trees.begin() + start + 4, trees.begin() + end);
  result.nextIndex = end + 1;
  return result;
}

bool MacroTemplateExpander::validateTrees(const std::vector<TokenTree> &trees,
                                          const MetaScope &scope,
                                          size_t loopDepth) {
  MetaScope current;
  current.parent = &scope;
  for (size_t index = 0; index < trees.size();) {
    const TokenTree &tree = trees[index];
    if (isWhen(tree)) {
      auto when = parseWhen(trees, index);
      if (!when || !validateTrees(when->condition, current, loopDepth) ||
          !validateTrees(when->thenBody->children(), current, loopDepth) ||
          (when->elseBody &&
           !validateTrees(when->elseBody->children(), current, loopDepth)))
        return false;
      index = when->nextIndex;
      continue;
    }
    if (startsMetaCase(trees, index, current)) {
      auto match = parseCase(trees, index);
      if (!match || !validateTrees(match->subject, current, loopDepth))
        return false;
      for (const auto &arm : match->arms) {
        if (!validateTrees(arm.body->children(), current, loopDepth))
          return false;
      }
      index = match->nextIndex;
      continue;
    }
    if (isLeaf(tree, TokenType::DOLLAR) && index + 1 < trees.size() &&
        isLeaf(trees[index + 1], TokenType::LET)) {
      auto let = parseLet(trees, index);
      if (!let || !validateTrees(let->value, current, loopDepth))
        return false;
      if (!current.bindings.emplace(let->name, std::monostate{}).second) {
        report("Duplicate compile-time binding '$" + let->name + "'.");
        return false;
      }
      index = let->nextIndex;
      continue;
    }
    if (isLeaf(tree, TokenType::DOLLAR) && index + 1 < trees.size() &&
        (isLeaf(trees[index + 1], TokenType::BREAK) ||
         isLeaf(trees[index + 1], TokenType::CONTINUE))) {
      if (!loopDepth || index + 2 >= trees.size() ||
          !isLeaf(trees[index + 2], TokenType::SEMICOLON)) {
        report("'$break' and '$continue' require a compile-time loop and ';'.");
        return false;
      }
      index += 3;
      continue;
    }
    if ((isLeaf(tree, TokenType::WHILE) && index + 1 < trees.size() &&
         isLeaf(trees[index + 1], TokenType::DOLLAR)) ||
        (isLeaf(tree, TokenType::DOLLAR) && index + 1 < trees.size() &&
         isLeaf(trees[index + 1], TokenType::WHILE))) {
      report("Compile-time 'while' is not supported.");
      return false;
    }
    if (isLeaf(tree, TokenType::DOLLAR) && index + 1 < trees.size() &&
        (isLeaf(trees[index + 1], TokenType::VAR) ||
         isLeaf(trees[index + 1], TokenType::GLOBAL))) {
      report("Mutable compile-time state is not supported.");
      return false;
    }
    if (startsMetaLoop(trees, index)) {
      auto loop = parseLoop(trees, index);
      if (!loop)
        return false;
      const auto *binding = current.find(loop->packName);
      const auto *pack =
          binding ? std::get_if<const MacroCapture *>(binding) : nullptr;
      if ((!pack || !(*pack)->isVariadic) &&
          (!binding || !std::holds_alternative<std::monostate>(*binding))) {
        report("'$" + loop->packName + "' is not a variadic pack.");
        return false;
      }
      const std::vector<TokenTree> placeholder;
      MetaScope iteration;
      iteration.parent = &current;
      iteration.bindings.emplace(
          loop->elementName,
          MetaFragment{&placeholder,
                       pack ? (*pack)->kind : MacroParameterKind::Tokens,
                       nullptr});
      if (loop->positionName)
        iteration.bindings.emplace(*loop->positionName, MetaPosition{0, 1});
      if (!validateTrees(loop->body->children(), iteration, loopDepth + 1) ||
          (loop->separator && !validateTrees(loop->separator->children(),
                                             iteration, loopDepth + 1)))
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
      const auto *binding = current.find(name);
      if (!binding) {
        report("Unknown macro capture '$" + name + "'.");
        return false;
      }
      index += 2;
      if (std::holds_alternative<MetaPosition>(*binding) &&
          (index + 1 >= trees.size() || !isLeaf(trees[index], TokenType::DOT) ||
           !isLeaf(trees[index + 1], TokenType::ID))) {
        report("Expected a position property after '$" + name + "'.");
        return false;
      }
      if (index + 1 < trees.size() && isLeaf(trees[index], TokenType::DOT) &&
          isLeaf(trees[index + 1], TokenType::ID) &&
          usesMetaProperty(*binding, trees[index + 1].token().value)) {
        const std::string &property = trees[index + 1].token().value;
        if (std::holds_alternative<MetaPosition>(*binding) &&
            property != "index" && property != "isFirst" &&
            property != "isLast") {
          report("Unknown loop position property '" + property + "'.");
          return false;
        }
        index += 2;
      }
      continue;
    }

    if (!tree.isLeaf() && !validateTrees(tree.children(), current, loopDepth))
      return false;
    ++index;
  }
  return true;
}

std::optional<MacroTemplateExpander::ExpansionResult>
MacroTemplateExpander::expandTrees(const std::vector<TokenTree> &trees,
                                   const MetaScope &scope, size_t loopDepth) {
  ExpansionResult output;
  MetaScope current;
  current.parent = &scope;
  for (size_t index = 0; index < trees.size();) {
    const TokenTree &tree = trees[index];
    if (isWhen(tree)) {
      auto when = parseWhen(trees, index);
      if (!when)
        return std::nullopt;
      auto condition = meta_.evaluate(when->condition, current);
      if (!condition)
        return std::nullopt;
      auto selected = meta_.boolean(*condition);
      if (!selected)
        return std::nullopt;
      const TokenTree *body = *selected ? when->thenBody : when->elseBody;
      if (body) {
        auto branch = expandTrees(body->children(), current, loopDepth);
        if (!branch)
          return std::nullopt;
        output.trees.insert(output.trees.end(), branch->trees.begin(),
                            branch->trees.end());
        if (branch->flow != Flow::Normal) {
          output.flow = branch->flow;
          return output;
        }
      }
      index = when->nextIndex;
      continue;
    }
    if (startsMetaCase(trees, index, current)) {
      auto match = parseCase(trees, index);
      if (!match)
        return std::nullopt;
      auto subject = meta_.evaluate(match->subject, current);
      if (!subject)
        return std::nullopt;
      const TokenTree *selected = nullptr;
      for (const auto &arm : match->arms) {
        if (arm.isElse) {
          if (!selected)
            selected = arm.body;
          break;
        }
        auto label = meta_.evaluate(arm.label, current);
        if (!label)
          return std::nullopt;
        if (sameMetaValue(*subject, *label)) {
          selected = arm.body;
          break;
        }
      }
      if (selected) {
        auto branch = expandTrees(selected->children(), current, loopDepth);
        if (!branch)
          return std::nullopt;
        output.trees.insert(output.trees.end(), branch->trees.begin(),
                            branch->trees.end());
        if (branch->flow != Flow::Normal) {
          output.flow = branch->flow;
          return output;
        }
      }
      index = match->nextIndex;
      continue;
    }
    if (isLeaf(tree, TokenType::DOLLAR) && index + 1 < trees.size() &&
        isLeaf(trees[index + 1], TokenType::LET)) {
      auto let = parseLet(trees, index);
      if (!let)
        return std::nullopt;
      auto value = meta_.evaluate(let->value, current);
      if (!value)
        return std::nullopt;
      if (!current.bindings.emplace(let->name, std::move(*value)).second) {
        report("Duplicate compile-time binding '$" + let->name + "'.");
        return std::nullopt;
      }
      index = let->nextIndex;
      continue;
    }
    if (isLeaf(tree, TokenType::DOLLAR) && index + 1 < trees.size() &&
        (isLeaf(trees[index + 1], TokenType::BREAK) ||
         isLeaf(trees[index + 1], TokenType::CONTINUE))) {
      if (!loopDepth || index + 2 >= trees.size() ||
          !isLeaf(trees[index + 2], TokenType::SEMICOLON)) {
        report("'$break' and '$continue' require a compile-time loop and ';'.");
        return std::nullopt;
      }
      output.flow = isLeaf(trees[index + 1], TokenType::BREAK) ? Flow::Break
                                                               : Flow::Continue;
      return output;
    }
    if (const auto level = metaDiagnosticLevel(tree)) {
      if (index + 1 >= trees.size() ||
          !isGroup(trees[index + 1], Delimiter::Parenthesis)) {
        report("Expected arguments for compile-time diagnostic.");
        return std::nullopt;
      }
      const auto &args = trees[index + 1].children();
      size_t comma = 0;
      while (comma < args.size() && !isLeaf(args[comma], TokenType::COMMA))
        ++comma;
      if (comma == 0 || comma + 1 == args.size()) {
        report("Compile-time diagnostic expects a message and optional span.");
        return std::nullopt;
      }
      SourceSpan span = invocation_;
      std::vector<TokenTree> messageTokens;
      if (comma == args.size()) {
        messageTokens = args;
      } else {
        std::vector<TokenTree> spanTokens(args.begin(), args.begin() + comma);
        messageTokens.assign(args.begin() + comma + 1, args.end());
        auto spanValue = meta_.evaluate(spanTokens, current);
        if (!spanValue)
          return std::nullopt;
        auto explicitSpan = meta_.span(*spanValue);
        if (!explicitSpan)
          return std::nullopt;
        span = *explicitSpan;
      }
      auto messageValue = meta_.evaluate(messageTokens, current);
      if (!messageValue)
        return std::nullopt;
      auto message = meta_.string(*messageValue);
      if (!message)
        return std::nullopt;
      diagnostics_.report(span, *level, *message);
      if (*level == DiagnosticLevel::Error)
        return std::nullopt;
      index += 2;
      if (index < trees.size() && isLeaf(trees[index], TokenType::SEMICOLON))
        ++index;
      continue;
    }
    if (startsMetaLoop(trees, index)) {
      auto loop = parseLoop(trees, index);
      if (!loop)
        return std::nullopt;
      const auto *binding = current.find(loop->packName);
      const auto *pack =
          binding ? std::get_if<const MacroCapture *>(binding) : nullptr;
      if (!pack || !(*pack)->isVariadic) {
        report("'$" + loop->packName + "' is not a variadic pack.");
        return std::nullopt;
      }
      const auto &elements = (*pack)->elements;
      std::optional<size_t> previousEmittedElement;
      for (size_t element = 0; element < elements.size(); ++element) {
        if (expandedIterations_ == maxIterations_) {
          report("Macro template iteration limit exceeded.");
          return std::nullopt;
        }
        ++expandedIterations_;
        MetaScope iteration;
        iteration.parent = &current;
        iteration.bindings.emplace(
            loop->elementName,
            MetaFragment{&elements[element], (*pack)->kind, nullptr});
        if (loop->positionName) {
          iteration.bindings.emplace(*loop->positionName,
                                     MetaPosition{element, elements.size()});
        }
        auto body =
            expandTrees(loop->body->children(), iteration, loopDepth + 1);
        if (!body)
          return std::nullopt;
        if (loop->separator && previousEmittedElement && !body->trees.empty()) {
          MetaScope previous;
          previous.parent = &current;
          previous.bindings.emplace(
              loop->elementName,
              MetaFragment{&elements[*previousEmittedElement], (*pack)->kind,
                           nullptr});
          if (loop->positionName) {
            previous.bindings.emplace(
                *loop->positionName,
                MetaPosition{*previousEmittedElement, elements.size()});
          }
          auto separator =
              expandTrees(loop->separator->children(), previous, loopDepth + 1);
          if (!separator)
            return std::nullopt;
          if (separator->flow != Flow::Normal) {
            report("Loop control is not allowed in a separator.");
            return std::nullopt;
          }
          output.trees.insert(output.trees.end(), separator->trees.begin(),
                              separator->trees.end());
        }
        output.trees.insert(output.trees.end(), body->trees.begin(),
                            body->trees.end());
        if (!body->trees.empty())
          previousEmittedElement = element;
        if (body->flow == Flow::Break)
          break;
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
      const auto *binding = current.find(name);
      if (!binding) {
        report("Unknown macro capture '$" + name + "'.");
        return std::nullopt;
      }
      MetaValue value = *binding;
      index += 2;
      if (std::holds_alternative<MetaPosition>(value) &&
          (index + 1 >= trees.size() || !isLeaf(trees[index], TokenType::DOT) ||
           !isLeaf(trees[index + 1], TokenType::ID))) {
        report("Expected a position property after '$" + name + "'.");
        return std::nullopt;
      }
      while (index + 1 < trees.size() && isLeaf(trees[index], TokenType::DOT) &&
             isLeaf(trees[index + 1], TokenType::ID) &&
             usesMetaProperty(value, trees[index + 1].token().value)) {
        auto property = meta_.member(value, trees[index + 1].token().value);
        if (!property)
          return std::nullopt;
        value = std::move(*property);
        index += 2;
      }
      auto replacement = meta_.emit(value);
      if (!replacement)
        return std::nullopt;
      for (const auto &token : *replacement) {
        if (!reserve(token.tokenCount()))
          return std::nullopt;
        output.trees.push_back(token);
      }
      continue;
    }

    if (tree.isLeaf()) {
      if (!reserve(1))
        return std::nullopt;
      output.trees.push_back(
          TokenTree::leaf(generatedToken(tree.token(), invocation_, origin_)));
    } else {
      auto children = expandTrees(tree.children(), current, loopDepth);
      if (!children)
        return std::nullopt;
      if (children->flow != Flow::Normal) {
        report("Compile-time loop control cannot cross a syntax group.");
        return std::nullopt;
      }
      if (!reserve(1 + (tree.closing() ? 1 : 0)))
        return std::nullopt;
      std::optional<Token> closing;
      if (tree.closing())
        closing = generatedToken(*tree.closing(), invocation_, origin_);
      output.trees.push_back(
          TokenTree::group(tree.delimiter(),
                           generatedToken(tree.opening(), invocation_, origin_),
                           std::move(children->trees), std::move(closing)));
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
