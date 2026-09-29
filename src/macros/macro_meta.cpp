#include "macros/macro_meta.hpp"
#include "lexer/lexer.hpp"
#include "macros/macro_diagnostic_codes.hpp"
#include "parser/parser.hpp"
#include "token/source_fragment.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>
#include <utility>

namespace zap {
namespace {

bool isLeaf(const TokenTree &tree, TokenType type) {
  return tree.isLeaf() && tree.token().type == type;
}

std::string quoted(const std::string &value) {
  std::string result = "\"";
  for (char ch : value) {
    switch (ch) {
    case '\\':
      result += "\\\\";
      break;
    case '"':
      result += "\\\"";
      break;
    case '\n':
      result += "\\n";
      break;
    case '\r':
      result += "\\r";
      break;
    case '\t':
      result += "\\t";
      break;
    default:
      result += ch;
      break;
    }
  }
  result += '"';
  return result;
}

bool validIdentifier(const std::string &name) {
  if (name.empty())
    return false;
  const auto start = static_cast<unsigned char>(name.front());
  if (!std::isalpha(start) && start != '_')
    return false;
  for (char ch : name) {
    const auto byte = static_cast<unsigned char>(ch);
    if (!std::isalnum(byte) && byte != '_')
      return false;
  }
  return true;
}

} // namespace

struct MacroMetaEvaluator::Parser {
  MacroMetaEvaluator &owner;
  const std::vector<TokenTree> &trees;
  const MetaScope &scope;
  size_t index = 0;

  bool take(TokenType type) {
    if (index < trees.size() && isLeaf(trees[index], type)) {
      ++index;
      return true;
    }
    return false;
  }

  std::optional<MetaValue> parse() {
    auto value = parseOr();
    if (value && index != trees.size()) {
      owner.report("Unexpected token in compile-time expression.");
      return std::nullopt;
    }
    return value;
  }

  std::optional<MetaValue> parseOr() {
    auto left = parseAnd();
    while (left && take(TokenType::OR)) {
      auto right = parseAnd();
      if (!right)
        return std::nullopt;
      auto lhs = owner.boolean(*left);
      auto rhs = owner.boolean(*right);
      if (!lhs || !rhs)
        return std::nullopt;
      left = *lhs || *rhs;
    }
    return left;
  }

  std::optional<MetaValue> parseAnd() {
    auto left = parseEquality();
    while (left && take(TokenType::AND)) {
      auto right = parseEquality();
      if (!right)
        return std::nullopt;
      auto lhs = owner.boolean(*left);
      auto rhs = owner.boolean(*right);
      if (!lhs || !rhs)
        return std::nullopt;
      left = *lhs && *rhs;
    }
    return left;
  }

  std::optional<MetaValue> parseEquality() {
    auto left = parseComparison();
    while (left && index < trees.size() &&
           (isLeaf(trees[index], TokenType::EQUAL) ||
            isLeaf(trees[index], TokenType::NOTEQUAL))) {
      const bool negate = isLeaf(trees[index++], TokenType::NOTEQUAL);
      auto right = parseComparison();
      if (!right)
        return std::nullopt;
      bool equal = false;
      if (left->index() == right->index()) {
        if (auto a = std::get_if<bool>(&*left))
          equal = *a == std::get<bool>(*right);
        else if (auto a = std::get_if<int64_t>(&*left))
          equal = *a == std::get<int64_t>(*right);
        else if (auto a = std::get_if<std::string>(&*left))
          equal = *a == std::get<std::string>(*right);
        else if (auto a = std::get_if<MetaSyntaxKind>(&*left))
          equal = a->name == std::get<MetaSyntaxKind>(*right).name;
        else {
          owner.report("These compile-time values cannot be compared.");
          return std::nullopt;
        }
      } else if (auto a = std::get_if<MetaSyntaxKind>(&*left)) {
        if (auto b = std::get_if<std::string>(&*right))
          equal = a->name == *b;
        else {
          owner.report("Incompatible compile-time comparison.");
          return std::nullopt;
        }
      } else if (auto a = std::get_if<std::string>(&*left)) {
        if (auto b = std::get_if<MetaSyntaxKind>(&*right))
          equal = *a == b->name;
        else {
          owner.report("Incompatible compile-time comparison.");
          return std::nullopt;
        }
      } else {
        owner.report("Incompatible compile-time comparison.");
        return std::nullopt;
      }
      left = negate ? !equal : equal;
    }
    return left;
  }

  std::optional<MetaValue> parseComparison() {
    auto left = parseUnary();
    while (left && index < trees.size()) {
      const TokenType op = trees[index].isLeaf() ? trees[index].token().type
                                                 : TokenType::EOF_TOKEN;
      if (op != TokenType::LESS && op != TokenType::LESSEQUAL &&
          op != TokenType::GREATER && op != TokenType::GREATEREQUAL)
        break;
      ++index;
      auto right = parseUnary();
      if (!right)
        return std::nullopt;
      const auto *a = std::get_if<int64_t>(&*left);
      const auto *b = std::get_if<int64_t>(&*right);
      if (!a || !b) {
        owner.report("Compile-time ordering requires integer values.");
        return std::nullopt;
      }
      switch (op) {
      case TokenType::LESS:
        left = *a < *b;
        break;
      case TokenType::LESSEQUAL:
        left = *a <= *b;
        break;
      case TokenType::GREATER:
        left = *a > *b;
        break;
      case TokenType::GREATEREQUAL:
        left = *a >= *b;
        break;
      default:
        break;
      }
    }
    return left;
  }

  std::optional<MetaValue> parseUnary() {
    if (take(TokenType::NOT)) {
      auto operand = parseUnary();
      if (!operand)
        return std::nullopt;
      auto boolean = owner.boolean(*operand);
      return boolean ? std::optional<MetaValue>(!*boolean) : std::nullopt;
    }
    return parsePrimary();
  }

  std::optional<MetaValue> parsePrimary() {
    if (index >= trees.size()) {
      owner.report("Expected a compile-time value.");
      return std::nullopt;
    }
    const TokenTree &tree = trees[index++];
    std::optional<MetaValue> value;
    if (isLeaf(tree, TokenType::DOLLAR)) {
      if (index >= trees.size() || !isLeaf(trees[index], TokenType::ID)) {
        owner.report("Expected a meta binding after '$'.");
        return std::nullopt;
      }
      const std::string &name = trees[index++].token().value;
      const auto *binding = scope.find(name);
      if (!binding) {
        owner.report("Unknown macro capture '$" + name + "'.");
        return std::nullopt;
      }
      value = *binding;
    } else if (isLeaf(tree, TokenType::BOOL)) {
      value = tree.token().value == "true";
    } else if (isLeaf(tree, TokenType::STRING)) {
      value = tree.token().value;
    } else if (isLeaf(tree, TokenType::INTEGER)) {
      int64_t number = 0;
      const auto &text = tree.token().value;
      const auto result =
          std::from_chars(text.data(), text.data() + text.size(), number);
      if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        owner.report("Invalid integer in compile-time expression.");
        return std::nullopt;
      }
      value = number;
    } else if (tree.isLeaf() && tree.token().type == TokenType::ID &&
               index < trees.size() && !trees[index].isLeaf() &&
               trees[index].delimiter() == Delimiter::Parenthesis) {
      value = owner.call(tree.token().value, trees[index++], scope);
    } else if (tree.isLeaf() && tree.token().type == TokenType::ID) {
      value = MetaSyntaxKind{tree.token().value};
    } else if (!tree.isLeaf() && tree.delimiter() == Delimiter::Parenthesis) {
      value = owner.evaluate(tree.children(), scope);
    } else {
      owner.report("Expected a compile-time value.");
      return std::nullopt;
    }
    if (!value)
      return std::nullopt;
    while (index + 1 < trees.size() && isLeaf(trees[index], TokenType::DOT) &&
           isLeaf(trees[index + 1], TokenType::ID)) {
      value = owner.member(*value, trees[index + 1].token().value);
      index += 2;
      if (!value)
        return std::nullopt;
    }
    return value;
  }
};

MacroMetaEvaluator::MacroMetaEvaluator(
    DiagnosticEngine &diagnostics, const SourceSpan &invocation,
    std::shared_ptr<const ExpansionOrigin> origin, FreshContext freshContext)
    : diagnostics_(diagnostics), invocation_(invocation),
      origin_(std::move(origin)), freshContext_(std::move(freshContext)) {}

std::optional<MetaValue>
MacroMetaEvaluator::evaluate(const std::vector<TokenTree> &expression,
                             const MetaScope &scope) {
  return Parser{*this, expression, scope}.parse();
}

std::optional<MetaValue> MacroMetaEvaluator::member(const MetaValue &value,
                                                    const std::string &name) {
  if (const auto *pack = std::get_if<const MacroCapture *>(&value)) {
    if (name == "count")
      return static_cast<int64_t>((*pack)->elements.size());
    if (name == "isEmpty")
      return (*pack)->elements.empty();
    if (name == "first" || name == "last") {
      if ((*pack)->elements.empty()) {
        report("Cannot access '" + name + "' of an empty macro pack.");
        return std::nullopt;
      }
      const auto &fragment = name == "first" ? (*pack)->elements.front()
                                             : (*pack)->elements.back();
      return MetaFragment{&fragment, (*pack)->kind, nullptr, nullptr};
    }
  } else if (const auto *fragment = std::get_if<MetaFragment>(&value)) {
    if (name == "kind")
      return MetaSyntaxKind{fragmentKind(*fragment)};
    if (name == "isIdent")
      return fragmentKind(*fragment) == "ident";
    if (name == "isLiteral")
      return fragmentKind(*fragment) == "literal";
    if (name == "source")
      return sourceText(*fragment);
    if (name == "text" && fragmentKind(*fragment) == "ident")
      return fragment->tokens->front().token().value;
    if (name == "span") {
      if (fragment->source)
        return fragment->source->spanAt(0, fragment->source->text.size());
      if (!fragment->tokens || fragment->tokens->empty()) {
        report("Empty fragment has no source span.");
        return std::nullopt;
      }
      return SourceSpan::merge(fragment->tokens->front().span(),
                               fragment->tokens->back().span());
    }
  } else if (const auto *position = std::get_if<MetaPosition>(&value)) {
    if (name == "index")
      return static_cast<int64_t>(position->index);
    if (name == "isFirst")
      return position->index == 0;
    if (name == "isLast")
      return position->index + 1 == position->count;
  }
  report("Unknown compile-time property '" + name + "'.");
  return std::nullopt;
}

std::optional<std::vector<TokenTree>>
MacroMetaEvaluator::emit(const MetaValue &value) {
  if (const auto *fragment = std::get_if<MetaFragment>(&value)) {
    if (fragment->tokens)
      return *fragment->tokens;
  }
  if (const auto *pack = std::get_if<const MacroCapture *>(&value)) {
    std::vector<TokenTree> output;
    for (size_t i = 0; i < (*pack)->elements.size(); ++i) {
      output.insert(output.end(), (*pack)->elements[i].begin(),
                    (*pack)->elements[i].end());
      if (i + 1 < (*pack)->elements.size())
        output.push_back((*pack)->separators[i]);
    }
    return output;
  }
  if (const auto *boolean = std::get_if<bool>(&value))
    return std::vector<TokenTree>{
        literal(TokenType::BOOL, *boolean ? "true" : "false")};
  if (const auto *integer = std::get_if<int64_t>(&value))
    return std::vector<TokenTree>{
        literal(TokenType::INTEGER, std::to_string(*integer))};
  if (const auto *stringValue = std::get_if<std::string>(&value))
    return std::vector<TokenTree>{literal(TokenType::STRING, *stringValue)};
  if (const auto *kind = std::get_if<MetaSyntaxKind>(&value))
    return std::vector<TokenTree>{literal(TokenType::ID, kind->name)};
  report("This compile-time value cannot be inserted as syntax.");
  return std::nullopt;
}

std::optional<bool> MacroMetaEvaluator::boolean(const MetaValue &value) {
  if (const auto *result = std::get_if<bool>(&value))
    return *result;
  report("Compile-time condition must be Boolean.");
  return std::nullopt;
}

std::optional<std::string> MacroMetaEvaluator::string(const MetaValue &value) {
  if (const auto *result = std::get_if<std::string>(&value))
    return *result;
  report("Expected a compile-time String.");
  return std::nullopt;
}

std::optional<SourceSpan> MacroMetaEvaluator::span(const MetaValue &value) {
  if (const auto *result = std::get_if<SourceSpan>(&value))
    return *result;
  report("Expected a compile-time source span.");
  return std::nullopt;
}

std::optional<MetaValue> MacroMetaEvaluator::call(const std::string &name,
                                                  const TokenTree &arguments,
                                                  const MetaScope &scope) {
  if (!arguments.closing()) {
    report("Unterminated compile-time function call.");
    return std::nullopt;
  }
  if (name != "sourceText" && name != "sourceArguments" &&
      name != "freshIdent") {
    report("Unknown compile-time function '" + name + "'.");
    return std::nullopt;
  }
  const auto &children = arguments.children();
  for (const auto &tree : children) {
    if (isLeaf(tree, TokenType::COMMA)) {
      report("Compile-time function expects exactly one argument.");
      return std::nullopt;
    }
  }
  auto argument = evaluate(children, scope);
  if (!argument)
    return std::nullopt;
  if (name == "sourceText") {
    if (const auto *fragment = std::get_if<MetaFragment>(&*argument))
      return sourceText(*fragment);
    report("sourceText expects a captured fragment.");
    return std::nullopt;
  }
  if (name == "sourceArguments") {
    const auto *fragment = std::get_if<MetaFragment>(&*argument);
    if (!fragment || !fragment->source) {
      report("sourceArguments expects a source capture.");
      return std::nullopt;
    }
    std::vector<TokenTree> expressions;
    const auto &interpolations = fragment->source->interpolations;
    for (size_t index = 0; index < interpolations.size(); ++index) {
      const auto &interpolation = interpolations[index];
      DiagnosticEngine scratch(diagnostics_.sourceText(),
                               diagnostics_.sourceName());
      auto built = TokenTreeBuilder::build(interpolation.tokens, scratch);
      if (built.hadDelimiterErrors || built.trees.empty()) {
        diagnostics_.report(interpolation.span, DiagnosticLevel::Error,
                            macro_diagnostic::Arguments,
                            "Invalid source interpolation expression.");
        return std::nullopt;
      }
      zap::Parser parser(interpolation.tokens, scratch, nullptr, {},
                         MacroParseMode::ValidateFragmentSyntax);
      if (!parser.parseFragment(FragmentKind::Expression)) {
        diagnostics_.report(interpolation.span, DiagnosticLevel::Error,
                            macro_diagnostic::Arguments,
                            "Invalid source interpolation expression.");
        return std::nullopt;
      }
      if (index) {
        Token comma(TokenType::COMMA, ",", interpolation.span, ",",
                    origin_->mark, origin_);
        expressions.push_back(TokenTree::leaf(std::move(comma)));
      }
      expressions.insert(expressions.end(), built.trees.begin(),
                         built.trees.end());
    }
    auto owner =
        std::make_shared<const std::vector<TokenTree>>(std::move(expressions));
    return MetaFragment{owner.get(), MacroParameterKind::Tokens,
                        std::move(owner), nullptr};
  }
  auto nameValue = string(*argument);
  if (!nameValue)
    return std::nullopt;
  if (!validIdentifier(*nameValue)) {
    report("freshIdent expects a valid identifier spelling.");
    return std::nullopt;
  }
  DiagnosticEngine identifierDiagnostics(*nameValue, invocation_.sourceName);
  Lexer lexer(identifierDiagnostics);
  const auto identifierTokens = lexer.tokenize(*nameValue);
  if (identifierDiagnostics.hadErrors() || identifierTokens.size() != 1 ||
      identifierTokens.front().type != TokenType::ID) {
    report("freshIdent cannot use a reserved keyword.");
    return std::nullopt;
  }
  auto context = freshContext_();
  if (!context) {
    report("freshIdent exhausted syntax contexts.", macro_diagnostic::Limit);
    return std::nullopt;
  }
  auto tokens =
      std::make_shared<const std::vector<TokenTree>>(std::vector<TokenTree>{
          TokenTree::leaf(Token(TokenType::ID, *nameValue, invocation_,
                                *nameValue, *context, origin_))});
  return MetaFragment{tokens.get(), MacroParameterKind::Identifier,
                      std::move(tokens), nullptr};
}

std::string MacroMetaEvaluator::sourceText(const MetaFragment &fragment) const {
  if (fragment.source) {
    std::string result;
    size_t position = 0;
    for (const auto &interpolation : fragment.source->interpolations) {
      const size_t begin = interpolation.bodyBegin - 2;
      result.append(fragment.source->text, position, begin - position);
      result += '?';
      position = interpolation.bodyEnd + 1;
    }
    result.append(fragment.source->text, position, std::string::npos);
    return result;
  }
  if (!fragment.tokens || fragment.tokens->empty())
    return {};
  const auto flattened = flattenTokenTrees(*fragment.tokens);
  const bool generated =
      std::any_of(flattened.begin(), flattened.end(),
                  [](const Token &token) { return token.expansionOrigin; });
  const SourceSpan span = SourceSpan::merge(fragment.tokens->front().span(),
                                            fragment.tokens->back().span());
  const auto &source = diagnostics_.sourceText();
  if (!generated && span.sourceName == diagnostics_.sourceName() &&
      span.offset <= source.size() &&
      span.length <= source.size() - span.offset)
    return source.substr(span.offset, span.length);
  std::string text;
  for (const auto &token : flattened)
    text += token.spelling;
  return text;
}

std::string
MacroMetaEvaluator::fragmentKind(const MetaFragment &fragment) const {
  if (fragment.tokens && fragment.tokens->size() == 1 &&
      fragment.tokens->front().isLeaf()) {
    const TokenType type = fragment.tokens->front().token().type;
    if (type == TokenType::ID)
      return "ident";
    if (type == TokenType::INTEGER || type == TokenType::FLOAT ||
        type == TokenType::STRING || type == TokenType::CHAR ||
        type == TokenType::BOOL || type == TokenType::NULL_LITERAL)
      return "literal";
  }
  switch (fragment.kind) {
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
  case MacroParameterKind::Source:
    return "source";
  }
  return "tokens";
}

TokenTree MacroMetaEvaluator::literal(TokenType type,
                                      const std::string &value) const {
  return TokenTree::leaf(
      Token(type, value, invocation_,
            type == TokenType::STRING ? quoted(value) : value, origin_->mark,
            origin_));
}

void MacroMetaEvaluator::report(const std::string &message, const char *code) {
  SourceSpan span = invocation_;
  span.expansionOrigin = origin_;
  diagnostics_.report(span, DiagnosticLevel::Error,
                      code ? code : macro_diagnostic::Expansion, message);
}

} // namespace zap
