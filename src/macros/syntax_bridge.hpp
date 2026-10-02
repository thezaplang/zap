#pragma once

#include "macros/syntax_protocol.hpp"
#include "token/source_fragment.hpp"
#include "token/token_tree.hpp"

#include <memory>
#include <optional>
#include <vector>

namespace zap::ctfe_bridge {

struct SyntaxInput {
  ctfe::SyntaxValue value;
  // References stay valid while the invocation's captures are alive.
  std::vector<const Token *> capturedTokens;
};

ctfe::SyntaxSpan syntaxSpan(const SourceSpan &span);
SourceSpan sourceSpan(const ctfe::SyntaxSpan &span);

std::optional<SyntaxInput>
syntaxTokens(const std::vector<TokenTree> &trees,
             size_t maxBytes = ctfe::MaxSyntaxMessageBytes);
std::optional<SyntaxInput>
syntaxSource(const SourceFragment &fragment,
             size_t maxBytes = ctfe::MaxSyntaxMessageBytes);

std::optional<std::vector<Token>>
compilerTokens(const ctfe::SyntaxTokens &syntax, const SourceSpan &invocation,
               const std::shared_ptr<const ExpansionOrigin> &origin,
               const SyntaxInput &input);

} // namespace zap::ctfe_bridge
