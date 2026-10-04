#include "macros/ctfe_runtime.hpp"
#include "macros/ctfe_semantics.hpp"

#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#include "token/source_fragment.hpp"

#include <limits>

namespace zap::ctfe {
namespace {

int64_t checkedInt(uint64_t value) {
    if (value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        throw CtfeFailure{"M3003", "Syntax position exceeds CTFE Int range."};
    return static_cast<int64_t>(value);
}

size_t index(const CtfeValue& value, size_t size, bool allowEnd = false) {
    auto number = std::get<int64_t>(value);
    if (number < 0 || static_cast<uint64_t>(number) > size
        || (!allowEnd && static_cast<uint64_t>(number) == size))
        throw CtfeFailure{"M3001", "Syntax index is out of range."};
    return static_cast<size_t>(number);
}

const SyntaxValue& syntax(const CtfeValue& value) {
    return *std::get<SyntaxHandle>(value);
}

void reserveToken(const SyntaxToken& token, CtfeSyntaxBudget& budget) {
    budget.reserve(
        sizeof(SyntaxToken) + token.value.size() + token.spelling.size()
            + token.span.sourceName.size(),
        3
    );
}

void reserveTokens(const SyntaxTokens& tokens, CtfeSyntaxBudget& budget) {
    if (tokens.tokens.size() > budget.limits.maxDefinitionTokens)
        throw CtfeFailure{"M3003", "CTFE output token limit exceeded."};
    for (const auto& token : tokens.tokens)
        reserveToken(token, budget);
}

std::vector<Token> validationTokens(
    const SyntaxTokens& syntax,
    CtfeSyntaxBudget& budget,
    size_t depth = 0
) {
    if (depth > MaxSyntaxNesting)
        throw CtfeFailure{"M3003", "Syntax nesting limit exceeded."};
    reserveTokens(syntax, budget);
    budget.reserve(syntax.tokens.size(), 256);
    std::vector<Token> tokens;
    tokens.reserve(syntax.tokens.size());
    for (const auto& entry : syntax.tokens) {
        const auto& span = entry.span;
        tokens.emplace_back(
            static_cast<TokenType>(entry.type),
            entry.value,
            SourceSpan(span.line, span.column, span.offset, span.length, span.sourceName),
            entry.spelling,
            entry.context
        );
        if (entry.sourceFragment) {
            const auto& source = *entry.sourceFragment;
            budget.reserve(sizeof(SourceFragment) + source.text.size() + source.sourceName.size());
            budget.reserve(source.offsets.size(), sizeof(SourceOffset));
            budget.reserve(source.interpolations.size(), sizeof(SourceInterpolation));
            auto fragment = std::make_shared<SourceFragment>();
            fragment->text = source.text;
            fragment->sourceName = source.sourceName;
            fragment->offsetMap.reserve(source.offsets.size());
            for (const auto& offset : source.offsets)
                fragment->offsetMap.push_back({offset.line, offset.column, offset.offset});
            for (const auto& interpolation : source.interpolations) {
                const auto& position = interpolation.span;
                budget.reserve(position.sourceName.size());
                fragment->interpolations.push_back(
                    {SourceSpan(
                         position.line,
                         position.column,
                         position.offset,
                         position.length,
                         position.sourceName
                     ),
                        interpolation.bodyBegin,
                        interpolation.bodyEnd,
                        validationTokens(interpolation.expression, budget, depth + 1)}
                );
            }
            tokens.back().sourceFragment = std::move(fragment);
        }
    }
    return tokens;
}

void validate(
    const SyntaxTokens& tokens,
    FragmentKind kind,
    const SyntaxMacroRequest& request,
    CtfeSyntaxBudget& budget
) {
    auto compilerTokens = validationTokens(tokens, budget);
    const std::string source;
    zap::DiagnosticEngine diagnostics(source, request.invocation.sourceName);
    zap::Parser parser(
        std::move(compilerTokens),
        diagnostics,
        nullptr,
        {},
        MacroParseMode::ValidateFragmentSyntax
    );
    if (!parser.parseFragment(kind) || diagnostics.hadErrors()) {
        for (const auto& error : diagnostics.diagnostics())
            if (error.code == "P1006")
                throw CtfeFailure{"M3003", error.message};
        throw CtfeFailure{"M3001", "Invalid structured syntax fragment."};
    }
}

CtfeValue wrap(SyntaxTokens tokens, ValueType type) {
    if (type == ValueType::SyntaxExpr)
        return std::make_shared<const SyntaxValue>(SyntaxExpr{std::move(tokens)});
    if (type == ValueType::SyntaxItem)
        return std::make_shared<const SyntaxValue>(SyntaxItem{std::move(tokens)});
    return std::make_shared<const SyntaxValue>(std::move(tokens));
}

CtfeValue construct(
    const std::string& name,
    const std::string& source,
    const SyntaxMacroRequest& request,
    CtfeSyntaxBudget& budget
) {
    if (source.size() > budget.limits.maxDefinitionBytes)
        throw CtfeFailure{"M3003", "Generated CTFE syntax size limit exceeded."};
    budget.reserve(source.size(), 256);
    zap::DiagnosticEngine diagnostics(source, request.invocation.sourceName);
    Lexer lexer(diagnostics);
    auto tokens = lexer.tokenize(source);
    if (diagnostics.hadErrors() || tokens.size() > budget.limits.maxDefinitionTokens)
        throw CtfeFailure{"M3001", "Invalid or oversized generated syntax."};
    size_t depth = 0;
    SyntaxTokens result;
    budget.reserve(tokens.size(), sizeof(SyntaxToken));
    result.tokens.reserve(tokens.size());
    for (const auto& token : tokens) {
        if (token.type == TokenType::LPAREN || token.type == TokenType::LBRACE
            || token.type == TokenType::SQUARE_LBRACE) {
            if (depth >= budget.limits.maxSyntaxDepth)
                throw CtfeFailure{"M3003", "CTFE syntax nesting limit exceeded."};
            ++depth;
        } else if ((token.type == TokenType::RPAREN || token.type == TokenType::RBRACE
                       || token.type == TokenType::SQUARE_RBRACE)
            && depth)
            --depth;
        if (token.sourceFragment)
            throw CtfeFailure{"M3002",
                "Nested generated source groups require captured structured syntax."};
        budget.reserve(
            sizeof(SyntaxToken) + token.value.size() + token.spelling.size()
                + request.invocation.sourceName.size(),
            3
        );
        result.tokens.push_back(
            {static_cast<uint32_t>(token.type),
                token.value,
                token.spelling,
                request.invocation,
                GeneratedSyntaxContext,
                nullptr}
        );
    }
    auto type = name == "syntaxExpr" ? ValueType::SyntaxExpr
        : name == "syntaxItem"       ? ValueType::SyntaxItem
                                     : ValueType::SyntaxTokens;
    if (type != ValueType::SyntaxTokens)
        validate(
            result,
            type == ValueType::SyntaxExpr ? FragmentKind::Expression : FragmentKind::ItemList,
            request,
            budget
        );
    return wrap(std::move(result), type);
}

} // namespace

std::optional<CtfeValue> evaluateSyntaxBuiltin(
    const std::string& name,
    const std::vector<CtfeValue>& arguments,
    const SyntaxMacroRequest& request,
    CtfeSyntaxBudget& budget
) {
    if (!builtinTypes().count(name))
        return std::nullopt;
    if (name == "syntaxExpr" || name == "syntaxItem" || name == "syntaxTokens")
        return construct(name, std::get<std::string>(arguments[0]), request, budget);
    if (name == "sourceInterpolation") {
        const auto& source = std::get<SyntaxSource>(syntax(arguments[0]));
        const auto& tokens =
            source.interpolations[index(arguments[1], source.interpolations.size())].expression;
        validate(tokens, FragmentKind::Expression, request, budget);
        reserveTokens(tokens, budget);
        return wrap(tokens, ValueType::SyntaxExpr);
    }
    if (name == "syntaxExprTokens" || name == "syntaxItemTokens") {
        const auto& value = syntax(arguments[0]);
        const auto& tokens = name == "syntaxExprTokens" ? std::get<SyntaxExpr>(value).syntax
                                                        : std::get<SyntaxItem>(value).syntax;
        reserveTokens(tokens, budget);
        return wrap(tokens, ValueType::SyntaxTokens);
    }
    if (name == "syntaxExprFromTokens" || name == "syntaxItemFromTokens") {
        const auto& tokens = std::get<SyntaxTokens>(syntax(arguments[0]));
        const bool expression = name == "syntaxExprFromTokens";
        validate(
            tokens,
            expression ? FragmentKind::Expression : FragmentKind::ItemList,
            request,
            budget
        );
        reserveTokens(tokens, budget);
        return wrap(tokens, expression ? ValueType::SyntaxExpr : ValueType::SyntaxItem);
    }
    if (name == "syntaxConcat" || name == "syntaxSlice") {
        const auto& left = std::get<SyntaxTokens>(syntax(arguments[0]));
        SyntaxTokens result;
        if (name == "syntaxConcat") {
            const auto& right = std::get<SyntaxTokens>(syntax(arguments[1]));
            if (left.tokens.size() > budget.limits.maxDefinitionTokens
                || right.tokens.size() > budget.limits.maxDefinitionTokens - left.tokens.size())
                throw CtfeFailure{"M3003", "CTFE output token limit exceeded."};
            reserveTokens(left, budget);
            reserveTokens(right, budget);
            result.tokens.reserve(left.tokens.size() + right.tokens.size());
            result.tokens.insert(result.tokens.end(), left.tokens.begin(), left.tokens.end());
            result.tokens.insert(result.tokens.end(), right.tokens.begin(), right.tokens.end());
        } else {
            const auto begin = index(arguments[1], left.tokens.size(), true);
            const auto end = index(arguments[2], left.tokens.size(), true);
            if (begin > end)
                throw CtfeFailure{"M3001", "Syntax slice range is reversed."};
            if (end - begin > budget.limits.maxDefinitionTokens)
                throw CtfeFailure{"M3003", "CTFE output token limit exceeded."};
            for (size_t i = begin; i < end; ++i)
                reserveToken(left.tokens[i], budget);
            result.tokens.assign(left.tokens.begin() + begin, left.tokens.begin() + end);
        }
        return wrap(std::move(result), ValueType::SyntaxTokens);
    }
    if (name == "syntaxCount")
        return checkedInt(std::get<SyntaxTokens>(syntax(arguments[0])).tokens.size());
    if (name.compare(0, 11, "syntaxToken") != 0)
        return std::nullopt;
    const auto& tokens = std::get<SyntaxTokens>(syntax(arguments[0]));
    const auto& token = tokens.tokens[index(arguments[1], tokens.tokens.size())];
    if (name == "syntaxTokenText" || name == "syntaxTokenSpelling") {
        const auto& text = name == "syntaxTokenText" ? token.value : token.spelling;
        budget.reserve(text.size());
        return text;
    }
    if (name == "syntaxTokenKind") {
        budget.reserve(64);
        return tokenTypeToString(static_cast<TokenType>(token.type));
    }
    budget.observesLocations = true;
    if (name == "syntaxTokenSourceName") {
        budget.reserve(token.span.sourceName.size());
        return token.span.sourceName;
    }
    if (name == "syntaxTokenLine")
        return checkedInt(token.span.line);
    if (name == "syntaxTokenColumn")
        return checkedInt(token.span.column);
    if (name == "syntaxTokenOffset")
        return checkedInt(token.span.offset);
    if (name == "syntaxTokenLength")
        return checkedInt(token.span.length);
    throw CtfeFailure{"M3002", "Unknown structured syntax operation."};
}

} // namespace zap::ctfe
