#pragma once

#include "macro_definition.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace zap {

struct MacroParseResult {
    std::vector<MacroDefinition> definitions;
    size_t nextPosition;
};

class MacroParser {
public:
    static MacroParseResult parse(
        const std::vector<Token>& tokens,
        size_t begin,
        size_t end,
        Visibility visibility,
        DiagnosticEngine& diagnostics
    );
};

} // namespace zap
