#pragma once

#include "token/token.hpp"

#include <string>
#include <utility>

struct SyntaxName {
  std::string text;
  SyntaxContextId context = ROOT_SYNTAX_CONTEXT;
  SyntaxContextId expansionMark = ROOT_SYNTAX_CONTEXT;
  std::string definitionModuleId;

  SyntaxName() = default;
  explicit SyntaxName(std::string spelling) : text(std::move(spelling)) {}
  explicit SyntaxName(const Token &token)
      : text(token.value), context(token.syntaxContext),
        expansionMark(token.expansionOrigin ? token.expansionOrigin->mark
                                            : ROOT_SYNTAX_CONTEXT),
        definitionModuleId(token.expansionOrigin
                               ? token.expansionOrigin->definitionModuleId
                               : std::string{}) {}
};
