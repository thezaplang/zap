#include "parser.hpp"

#include <utility>

namespace zap {

std::optional<ParsedFragmentPrefix> Parser::parseFragmentPrefix(FragmentKind kind) {
    if (kind != FragmentKind::Expression && kind != FragmentKind::Type) {
        _diag.report(
            peek().span,
            DiagnosticLevel::Error,
            "Only expression and type fragments support prefix parsing."
        );
        return std::nullopt;
    }
    const size_t begin = _cursor.position();
    try {
        ParsedFragment fragment = kind == FragmentKind::Expression
            ? ParsedFragment(parseExpression())
            : ParsedFragment(parseType());
        if (_diag.hadErrors() || _cursor.position() == begin)
            return std::nullopt;
        return ParsedFragmentPrefix{std::move(fragment), _cursor.position() - begin};
    } catch (const ParseError&) {
        return std::nullopt;
    }
}

std::optional<ParsedFragment> Parser::parseFragment(FragmentKind kind) {
    if (isAtEnd() && kind != FragmentKind::StatementList && kind != FragmentKind::ItemList) {
        _diag.report(peek().span, DiagnosticLevel::Error, "Expected a non-empty syntax fragment.");
        return std::nullopt;
    }

    std::optional<ParsedFragment> fragment;
    try {
        switch (kind) {
            case FragmentKind::Expression:
            case FragmentKind::Type: {
                auto prefix = parseFragmentPrefix(kind);
                if (!prefix)
                    return std::nullopt;
                fragment.emplace(std::move(prefix->fragment));
                break;
            }
            case FragmentKind::Statement: {
                auto body = parseBody();
                if (body->statements.size() != 1 || body->result) {
                    _diag.report(
                        peek().span,
                        DiagnosticLevel::Error,
                        "Expected exactly one statement fragment."
                    );
                    return std::nullopt;
                }
                fragment.emplace(StatementFragment{std::move(body->statements.front())});
                break;
            }
            case FragmentKind::StatementList:
                fragment.emplace(parseBody(true));
                break;
            case FragmentKind::Block: {
                const Token opening = eat(TokenType::LBRACE);
                auto body = parseBody();
                const Token closing = eat(TokenType::RBRACE);
                _builder.setSpan(body.get(), SourceSpan::merge(opening.span, closing.span));
                fragment.emplace(std::move(body));
                break;
            }
            case FragmentKind::Item: {
                auto root = parse();
                if (root->children.size() != 1) {
                    _diag.report(
                        peek().span,
                        DiagnosticLevel::Error,
                        "Expected exactly one item fragment."
                    );
                    return std::nullopt;
                }
                fragment.emplace(ItemFragment{std::move(root->children.front())});
                break;
            }
            case FragmentKind::ItemList:
                fragment.emplace(parseItemFragmentRoot());
                break;
        }
    } catch (const ParseError&) {
        return std::nullopt;
    }

    if (!isAtEnd()) {
        _diag
            .report(peek().span, DiagnosticLevel::Error, "Unexpected token after syntax fragment.");
    }
    if (_diag.hadErrors()) {
        return std::nullopt;
    }
    return fragment;
}

} // namespace zap
