#include "macro_parser.hpp"
#include "ctfe_interpreter.hpp"
#include "macro_diagnostic_codes.hpp"

#include "../parser/token_cursor.hpp"

#include <string>
#include <unordered_set>
#include <utility>

namespace zap {
namespace {

std::optional<MacroParameterKind> parameterKind(const std::string &name) {
  if (name == "ident")
    return MacroParameterKind::Identifier;
  if (name == "literal")
    return MacroParameterKind::Literal;
  if (name == "expr")
    return MacroParameterKind::Expression;
  if (name == "type")
    return MacroParameterKind::Type;
  if (name == "stmt")
    return MacroParameterKind::Statement;
  if (name == "block")
    return MacroParameterKind::Block;
  if (name == "item")
    return MacroParameterKind::Item;
  if (name == "tokens")
    return MacroParameterKind::Tokens;
  if (name == "source")
    return MacroParameterKind::Source;
  return std::nullopt;
}

class DeclarationParser {
public:
  DeclarationParser(const std::vector<Token> &tokens, size_t begin, size_t end,
                    Visibility visibility, DiagnosticEngine &diagnostics)
      : tokens_(tokens), cursor_(tokens, begin, end), visibility_(visibility),
        diagnostics_(diagnostics) {}

  MacroParseResult parse() {
    const auto keyword = take(TokenType::MACRO);
    const auto name = take(TokenType::ID);
    if (!keyword || !name)
      return failure();
    if (cursor_.peek().type == TokenType::LBRACE)
      return parsePatternArms(*name);
    if (!take(TokenType::LPAREN))
      return failure();

    std::vector<MacroParameter> parameters;
    std::vector<MacroPatternPart> pattern;
    std::unordered_set<std::string> names;
    while (!cursor_.isAtEnd() && cursor_.peek().type != TokenType::RPAREN) {
      const auto dollar = take(TokenType::DOLLAR);
      const auto parameterName = take(TokenType::ID);
      if (!dollar || !parameterName || !take(TokenType::COLON))
        return failure();
      const auto kindToken = take(TokenType::ID);
      if (!kindToken)
        return failure();

      const auto kind = parameterKind(kindToken->value);
      if (!kind) {
        report(kindToken->span,
               "Unknown macro parameter kind '" + kindToken->value + "'.");
      }
      if (!names.insert(parameterName->value).second) {
        report(parameterName->span,
               "Duplicate macro parameter '$" + parameterName->value + "'.");
      }
      bool isVariadic = false;
      SourceSpan parameterEnd = kindToken->span;
      if (cursor_.peek().type == TokenType::ELLIPSIS) {
        parameterEnd = cursor_.peek().span;
        cursor_.advance();
        isVariadic = true;
      }
      if (kind) {
        if (*kind == MacroParameterKind::Source &&
            (isVariadic || !parameters.empty() ||
             cursor_.peek().type != TokenType::RPAREN)) {
          report(kindToken->span,
                 "Source capture must be the only, non-variadic parameter.");
        }
        MacroParameter parameter{*parameterName, *kind,
                                 SourceSpan::merge(dollar->span, parameterEnd),
                                 isVariadic};
        parameters.push_back(parameter);
        pattern.emplace_back(std::move(parameter));
      }

      if (cursor_.peek().type == TokenType::COMMA) {
        Token comma = cursor_.peek();
        cursor_.advance();
        if (cursor_.peek().type != TokenType::RPAREN)
          pattern.emplace_back(TokenTree::leaf(std::move(comma)));
        if (isVariadic && cursor_.peek().type != TokenType::RPAREN) {
          report(parameterEnd, "Variadic macro parameter must be last.");
          return failure();
        }
      } else if (cursor_.peek().type != TokenType::RPAREN) {
        report(cursor_.peek().span,
               "Expected ',' or ')' after macro parameter.");
        return failure();
      }
    }

    if (!take(TokenType::RPAREN))
      return failure();
    if (cursor_.peek().type == TokenType::ID)
      return parseProcedural(*keyword, *name, std::move(parameters),
                             std::move(pattern));
    if (cursor_.peek().type != TokenType::LBRACE) {
      report(cursor_.peek().span, "Expected '{' after macro parameters.");
      return failure();
    }

    auto body = TokenTreeBuilder::buildPrefix(tokens_, cursor_.position(),
                                              cursor_.end(), diagnostics_);
    cursor_.advance(body.nextPosition - cursor_.position());
    if (body.hadDelimiterErrors || body.trees.size() != 1 ||
        !body.trees.front().closing()) {
      return failure();
    }

    TokenTree expansion = std::move(body.trees.front());
    if (invalid_)
      return failure();
    MacroDefinition definition{
        *name,
        visibility_,
        std::move(parameters),
        std::move(pattern),
        false,
        std::move(expansion),
        SourceSpan::merge(keyword->span, cursor_.previous().span)};
    std::vector<MacroDefinition> definitions;
    definitions.push_back(std::move(definition));
    return {std::move(definitions), cursor_.position()};
  }

private:
  MacroParseResult parseProcedural(const Token &keyword, const Token &name,
                                   std::vector<MacroParameter> parameters,
                                   std::vector<MacroPatternPart> pattern) {
    if (invalid_)
      return failure();
    if (parameters.size() != 1 || parameters.front().isVariadic ||
        (parameters.front().kind != MacroParameterKind::Source &&
         parameters.front().kind != MacroParameterKind::Tokens)) {
      report(name.span,
             "Procedural macro requires exactly one non-variadic 'source' or "
             "'tokens' parameter.");
      return failure();
    }
    const auto output = take(TokenType::ID);
    if (!output)
      return failure();
    ProceduralMacroOutput outputKind;
    const char *resultType;
    if (output->value == "expr") {
      outputKind = ProceduralMacroOutput::Expression;
      resultType = "SyntaxExpr";
    } else if (output->value == "item") {
      outputKind = ProceduralMacroOutput::Item;
      resultType = "SyntaxItem";
    } else if (output->value == "stmt") {
      outputKind = ProceduralMacroOutput::Statement;
      resultType = "SyntaxTokens";
    } else if (output->value == "type") {
      outputKind = ProceduralMacroOutput::Type;
      resultType = "SyntaxTokens";
    } else {
      report(output->span,
             "Expected expr, stmt, type or item after macro signature.");
      return failure();
    }
    if (cursor_.peek().type != TokenType::LBRACE) {
      report(cursor_.peek().span, "Expected procedural macro body.");
      return failure();
    }
    auto body = TokenTreeBuilder::buildPrefix(tokens_, cursor_.position(),
                                              cursor_.end(), diagnostics_);
    cursor_.advance(body.nextPosition - cursor_.position());
    if (body.hadDelimiterErrors || body.trees.size() != 1 ||
        !body.trees.front().closing())
      return failure();
    const auto &bodySpan = body.trees.front().span();
    const auto &source = diagnostics_.sourceText();
    if (bodySpan.offset > source.size() ||
        bodySpan.length > source.size() - bodySpan.offset) {
      report(bodySpan, "Procedural macro body has no source text.");
      return failure();
    }
    const auto &parameter = parameters.front();
    std::string functionSource =
        "fun __syntax_macro__(" + parameter.name.value + ": " +
        (parameter.kind == MacroParameterKind::Source ? "SyntaxSource"
                                                      : "SyntaxTokens") +
        ") " + resultType + " " +
        source.substr(bodySpan.offset, bodySpan.length);
    if (!ctfe::CtfeInterpreter::validateDefinition(functionSource, bodySpan,
                                                   diagnostics_))
      return failure();
    MacroDefinition definition{
        name,
        visibility_,
        std::move(parameters),
        std::move(pattern),
        false,
        body.trees.front(),
        SourceSpan::merge(keyword.span, bodySpan),
        ProceduralMacro{outputKind, std::move(functionSource)}};
    return {{std::move(definition)}, cursor_.position()};
  }

  MacroParseResult parsePatternArms(const Token &name) {
    auto body = TokenTreeBuilder::buildPrefix(tokens_, cursor_.position(),
                                              cursor_.end(), diagnostics_);
    cursor_.advance(body.nextPosition - cursor_.position());
    if (body.hadDelimiterErrors || body.trees.size() != 1 ||
        !body.trees.front().closing())
      return failure();
    const auto &arms = body.trees.front().children();
    if (arms.empty() || arms.size() % 2 != 0) {
      report(body.trees.front().span(),
             "Pattern macro requires one or more '(pattern) {template}' arms.");
      return failure();
    }

    std::vector<MacroDefinition> definitions;
    for (size_t index = 0; index < arms.size(); index += 2) {
      if (arms[index].isLeaf() ||
          arms[index].delimiter() != Delimiter::Parenthesis ||
          arms[index + 1].isLeaf() ||
          arms[index + 1].delimiter() != Delimiter::Brace) {
        report(arms[index].span(),
               "Expected '(pattern) {template}' macro arm.");
        return failure();
      }
      std::vector<MacroParameter> parameters;
      std::vector<MacroPatternPart> pattern;
      std::unordered_set<std::string> names;
      const auto &tokens = arms[index].children();
      for (size_t position = 0; position < tokens.size();) {
        if (tokens[position].isLeaf() &&
            tokens[position].token().type == TokenType::DOLLAR) {
          if (position + 3 >= tokens.size() || !tokens[position + 1].isLeaf() ||
              tokens[position + 1].token().type != TokenType::ID ||
              !tokens[position + 2].isLeaf() ||
              tokens[position + 2].token().type != TokenType::COLON ||
              !tokens[position + 3].isLeaf() ||
              tokens[position + 3].token().type != TokenType::ID) {
            report(tokens[position].span(),
                   "Expected '$name: kind' in macro pattern.");
            return failure();
          }
          const Token &parameterName = tokens[position + 1].token();
          const Token &kindToken = tokens[position + 3].token();
          auto kind = parameterKind(kindToken.value);
          if (!kind || !names.insert(parameterName.value).second) {
            report(kindToken.span, !kind ? "Unknown macro capture kind '" +
                                               kindToken.value + "'."
                                         : "Duplicate macro capture '$" +
                                               parameterName.value + "'.");
            return failure();
          }
          if (*kind == MacroParameterKind::Source) {
            report(kindToken.span,
                   "Source captures require a single signature parameter.");
            return failure();
          }
          MacroParameter parameter{
              parameterName, *kind,
              SourceSpan::merge(tokens[position].span(), kindToken.span),
              false};
          parameters.push_back(parameter);
          pattern.emplace_back(std::move(parameter));
          position += 4;
        } else {
          pattern.emplace_back(tokens[position]);
          ++position;
        }
      }
      for (size_t part = 0; part + 1 < pattern.size(); ++part) {
        const auto *capture = std::get_if<MacroParameter>(&pattern[part]);
        if (!capture || (capture->kind != MacroParameterKind::Expression &&
                         capture->kind != MacroParameterKind::Type &&
                         capture->kind != MacroParameterKind::Statement))
          continue;
        const auto *next = std::get_if<TokenTree>(&pattern[part + 1]);
        if (!next || !next->isLeaf()) {
          report(capture->span,
                 "Typed macro capture needs an unambiguous follow token.");
          return failure();
        }
        const TokenType follow = next->token().type;
        const bool pipeline =
            follow == TokenType::BIT_OR && part + 2 < pattern.size() &&
            std::holds_alternative<TokenTree>(pattern[part + 2]) &&
            std::get<TokenTree>(pattern[part + 2]).isLeaf() &&
            std::get<TokenTree>(pattern[part + 2]).token().type ==
                TokenType::GREATER;
        const bool allowed =
            (pipeline && capture->kind == MacroParameterKind::Expression) ||
            follow == TokenType::COMMA || follow == TokenType::SEMICOLON ||
            (capture->kind == MacroParameterKind::Type &&
             (follow == TokenType::ASSIGN || follow == TokenType::GREATER));
        if (!allowed) {
          report(next->span(),
                 "Illegal token after typed macro capture in pattern.");
          return failure();
        }
      }
      definitions.push_back(MacroDefinition{
          name, visibility_, std::move(parameters), std::move(pattern), true,
          arms[index + 1],
          SourceSpan::merge(arms[index].span(), arms[index + 1].span())});
    }
    return {std::move(definitions), cursor_.position()};
  }

  std::optional<Token> take(TokenType expected) {
    if (cursor_.peek().type != expected) {
      report(cursor_.peek().span, "Expected " + tokenTypeToString(expected) +
                                      " in macro declaration.");
      return std::nullopt;
    }
    Token token = cursor_.peek();
    cursor_.advance();
    return token;
  }

  void report(const SourceSpan &span, const std::string &message) {
    diagnostics_.report(span, DiagnosticLevel::Error,
                        macro_diagnostic::Declaration, message);
    invalid_ = true;
  }

  MacroParseResult failure() const { return {{}, cursor_.position()}; }

  const std::vector<Token> &tokens_;
  TokenCursor cursor_;
  Visibility visibility_;
  DiagnosticEngine &diagnostics_;
  bool invalid_ = false;
};

} // namespace

MacroParseResult MacroParser::parse(const std::vector<Token> &tokens,
                                    size_t begin, size_t end,
                                    Visibility visibility,
                                    DiagnosticEngine &diagnostics) {
  return DeclarationParser(tokens, begin, end, visibility, diagnostics).parse();
}

} // namespace zap
