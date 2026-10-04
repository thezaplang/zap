#pragma once

#include "../token/token_tree.hpp"
#include "../visibility.hpp"

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace zap {

enum class MacroParameterKind {
    Identifier,
    Literal,
    Expression,
    Type,
    Statement,
    Block,
    Item,
    Tokens,
    Source,
};

struct MacroParameter {
    Token name;
    MacroParameterKind kind;
    SourceSpan span;
    bool isVariadic = false;
};

using MacroPatternPart = std::variant<TokenTree, MacroParameter>;

enum class ProceduralMacroOutput {
    Expression,
    Statement,
    Type,
    Item
};

struct ProceduralMacro {
    ProceduralMacroOutput output;
    std::string functionSource;
};

struct MacroDefinition {
    Token name;
    Visibility visibility;
    std::vector<MacroParameter> parameters;
    std::vector<MacroPatternPart> pattern;
    bool customPattern = false;
    TokenTree expansion;
    SourceSpan span;
    std::optional<ProceduralMacro> procedural = std::nullopt;
};

} // namespace zap
