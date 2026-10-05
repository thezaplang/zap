#include "frontend/module_outline.hpp"

#include "macros/macro_parser.hpp"
#include "parser/parser.hpp"
#include "token/token_tree.hpp"

#include <string>
#include <utility>

namespace zap::frontend {
namespace {

struct TreeRange {
    size_t begin;
    size_t end;
};

TokenType leadingType(const TokenTree& tree) {
    return tree.isLeaf() ? tree.token().type : tree.opening().type;
}

bool startsOutlineDeclaration(const std::vector<TokenTree>& trees, size_t index) {
    const TokenType type = leadingType(trees[index]);
    if (type == TokenType::IMPORT || type == TokenType::MACRO)
        return true;
    if ((type == TokenType::PUB || type == TokenType::PRIV) && index + 1 < trees.size()) {
        const TokenType next = leadingType(trees[index + 1]);
        return next == TokenType::IMPORT || next == TokenType::MACRO;
    }
    return false;
}

bool skipAttribute(const std::vector<TokenTree>& trees, size_t& index) {
    ++index;
    if (index >= trees.size())
        return false;
    if (!trees[index].isLeaf() && trees[index].delimiter() == Delimiter::Brace) {
        bool ctfeOnly = false;
        for (const auto& attribute : trees[index].children())
            ctfeOnly |= attribute.isLeaf() && attribute.token().type == TokenType::ID
                && attribute.token().value == "ctfe";
        ++index;
        return ctfeOnly;
    }
    if (leadingType(trees[index]) == TokenType::ID) {
        const bool ctfeOnly = trees[index].token().value == "ctfe";
        ++index;
        if (index < trees.size() && !trees[index].isLeaf()
            && trees[index].delimiter() == Delimiter::Parenthesis) {
            ++index;
        }
        return ctfeOnly;
    }
    return false;
}

size_t treeIndexAtOrAfter(const std::vector<TreeRange>& ranges, size_t from, size_t tokenPosition) {
    while (from < ranges.size() && ranges[from].begin < tokenPosition) {
        ++from;
    }
    return from;
}

void collectImport(
    ModuleOutline& outline,
    const std::vector<Token>& tokens,
    size_t begin,
    size_t end,
    DiagnosticEngine& diagnostics
) {
    DiagnosticEngine importDiagnostics(diagnostics.sourceText(), diagnostics.sourceName());
    std::vector<Token> fragmentTokens(tokens.begin() + begin, tokens.begin() + end);
    Parser parser(std::move(fragmentTokens), importDiagnostics);
    auto fragment = parser.parseFragment(FragmentKind::Item);
    for (const Diagnostic& diagnostic : importDiagnostics.diagnostics()) {
        diagnostics.report(diagnostic.span, diagnostic.level, diagnostic.code, diagnostic.message);
    }
    if (!fragment || !std::holds_alternative<ItemFragment>(*fragment))
        return;

    auto& node = std::get<ItemFragment>(*fragment).node;
    if (auto* import = dynamic_cast<ImportNode*>(node.get())) {
        node.release();
        outline.imports.emplace_back(import);
    }
}

} // namespace

bool ModuleOutline::hasImportPath(const std::string& path) const {
    for (const auto& import : imports) {
        if (import->path == path)
            return true;
    }
    return false;
}

ModuleOutline ModuleOutline::scan(const std::vector<Token>& tokens, DiagnosticEngine& diagnostics) {
    ModuleOutline outline;
    auto result = TokenTreeBuilder::build(tokens, diagnostics);
    const auto& trees = result.trees;
    std::vector<TreeRange> ranges;
    ranges.reserve(trees.size());
    size_t position = 0;
    for (const TokenTree& tree : trees) {
        const size_t next = position + tree.tokenCount();
        ranges.push_back({position, next});
        position = next;
    }

    for (size_t index = 0; index < trees.size();) {
        const size_t itemStart = index;
        bool ctfeOnly = false;
        while (index < trees.size() && leadingType(trees[index]) == TokenType::AT) {
            ctfeOnly |= skipAttribute(trees, index);
        }

        Visibility visibility = Visibility::Private;
        if (index < trees.size() && trees[index].isLeaf()
            && (trees[index].token().type == TokenType::PUB
                || trees[index].token().type == TokenType::PRIV)) {
            visibility = trees[index].token().type == TokenType::PUB ? Visibility::Public
                                                                     : Visibility::Private;
            ++index;
        }
        if (index == trees.size())
            break;

        const TokenType type = leadingType(trees[index]);
        if (type == TokenType::FUN
            || (type == TokenType::UNSAFE && index + 1 < trees.size()
                && leadingType(trees[index + 1]) == TokenType::FUN)) {
            const size_t funIndex = type == TokenType::FUN ? index : index + 1;
            size_t end = funIndex;
            while (end < trees.size()
                && !(!trees[end].isLeaf() && trees[end].delimiter() == Delimiter::Brace)
                && leadingType(trees[end]) != TokenType::SEMICOLON)
                ++end;
            if (end < trees.size() && funIndex + 1 < trees.size() && trees[funIndex + 1].isLeaf()
                && leadingType(trees[funIndex + 1]) == TokenType::ID) {
                const size_t beginToken = ranges[itemStart].begin;
                const size_t endToken = ranges[end].end;
                const SourceSpan span =
                    SourceSpan::merge(tokens[beginToken].span, tokens[endToken - 1].span);
                const auto& source = diagnostics.sourceText();
                if (span.offset <= source.size() && span.length <= source.size() - span.offset)
                    outline.functions.push_back(
                        {trees[funIndex + 1].token(),
                            visibility,
                            ctfeOnly,
                            std::vector<Token>(
                                tokens.begin() + beginToken,
                                tokens.begin() + endToken
                            ),
                            source.substr(span.offset, span.length)}
                    );
            }
            index = end < trees.size() ? end + 1 : end;
            continue;
        }
        if (type == TokenType::MACRO) {
            auto parsed = MacroParser::parse(
                tokens,
                ranges[index].begin,
                tokens.size(),
                visibility,
                diagnostics
            );
            for (auto& definition : parsed.definitions)
                outline.macros.push_back(std::move(definition));
            index = treeIndexAtOrAfter(ranges, index + 1, parsed.nextPosition);
            continue;
        }

        if (type == TokenType::IMPORT) {
            size_t semicolon = index;
            while (semicolon < trees.size()
                && !(
                    trees[semicolon].isLeaf()
                    && trees[semicolon].token().type == TokenType::SEMICOLON
                )) {
                if (semicolon > index && startsOutlineDeclaration(trees, semicolon))
                    break;
                ++semicolon;
            }
            if (semicolon < trees.size() && trees[semicolon].isLeaf()
                && trees[semicolon].token().type == TokenType::SEMICOLON) {
                collectImport(
                    outline,
                    tokens,
                    ranges[itemStart].begin,
                    ranges[semicolon].end,
                    diagnostics
                );
                index = semicolon + 1;
            } else if (semicolon < trees.size()) {
                index = semicolon;
            } else {
                break;
            }
            continue;
        }

        while (index < trees.size()) {
            const bool endsAtSemicolon =
                trees[index].isLeaf() && trees[index].token().type == TokenType::SEMICOLON;
            const bool endsAtBody =
                !trees[index].isLeaf() && trees[index].delimiter() == Delimiter::Brace;
            ++index;
            if (endsAtSemicolon || endsAtBody)
                break;
        }
    }
    return outline;
}

} // namespace zap::frontend
