#include "parser.hpp"

namespace zap {
namespace {
void qualifiedNameFromExpressionImpl(const ExpressionNode* expr, std::string& out) {
    if (auto id = dynamic_cast<const ConstId*>(expr)) {
        out += id->value_;
        return;
    }
    if (auto member = dynamic_cast<const MemberAccessNode*>(expr)) {
        qualifiedNameFromExpressionImpl(member->left_.get(), out);
        if (out.empty())
            return;
        out += '.';
        out += member->member_;
    }
}

std::string qualifiedNameFromExpression(const ExpressionNode* expr) {
    std::string result;
    result.reserve(32);
    qualifiedNameFromExpressionImpl(expr, result);
    return result;
}
} // namespace

std::vector<std::unique_ptr<TypeNode>> Parser::parseGenericTypeArguments() {
    std::vector<std::unique_ptr<TypeNode>> args;
    eat(TokenType::LESS);
    if (peek().type != TokenType::GREATER) {
        do {
            args.push_back(parseType());
        } while (peek().type == TokenType::COMMA && eat(TokenType::COMMA).type == TokenType::COMMA);
    }
    eat(TokenType::GREATER);
    return args;
}

std::vector<std::unique_ptr<TypeNode>> Parser::parseGenericParameterList() {
    std::vector<std::unique_ptr<TypeNode>> params;
    eat(TokenType::LESS);
    if (peek().type != TokenType::GREATER) {
        do {
            auto param = parseType();
            if (peek().type == TokenType::ASSIGN) {
                eat(TokenType::ASSIGN);
                param->defaultType = parseType();
                _builder.setSpan(
                    param.get(),
                    SourceSpan::merge(param->span, param->defaultType->span)
                );
            }
            params.push_back(std::move(param));
        } while (peek().type == TokenType::COMMA && eat(TokenType::COMMA).type == TokenType::COMMA);
    }
    eat(TokenType::GREATER);
    return params;
}

std::vector<GenericConstraint> Parser::parseWhereClauses() {
    std::vector<GenericConstraint> constraints;
    while (peek().type == TokenType::WHERE) {
        eat(TokenType::WHERE);
        Token paramToken = eat(TokenType::ID);
        eat(TokenType::COLON);
        auto boundType = parseType();
        constraints.push_back({paramToken.value, std::move(boundType)});
    }
    return constraints;
}

bool Parser::isTypeStartToken(TokenType type) const {
    return type == TokenType::ID || type == TokenType::MULTIPLY || type == TokenType::ELLIPSIS
        || type == TokenType::SQUARE_LBRACE || type == TokenType::WEAK || type == TokenType::FUN;
}

bool Parser::isTryPostfixContext(TokenType type) const {
    switch (type) {
        case TokenType::SEMICOLON:
        case TokenType::COMMA:
        case TokenType::RPAREN:
        case TokenType::RBRACE:
        case TokenType::SQUARE_RBRACE:
        case TokenType::PLUS:
        case TokenType::MINUS:
        case TokenType::MULTIPLY:
        case TokenType::DIVIDE:
        case TokenType::MODULO:
        case TokenType::POW:
        case TokenType::BIT_OR:
        case TokenType::AND:
        case TokenType::OR:
        case TokenType::EQUAL:
        case TokenType::NOTEQUAL:
        case TokenType::LESS:
        case TokenType::LESSEQUAL:
        case TokenType::GREATER:
        case TokenType::GREATEREQUAL:
        case TokenType::LSHIFT:
        case TokenType::RSHIFT:
        case TokenType::AS:
        case TokenType::DOTDOT:
        case TokenType::IS:
            return true;
        default:
            return false;
    }
}

bool Parser::isGenericCallStart() const {
    return isGenericPostfixStart(TokenType::LPAREN);
}

bool Parser::isGenericStructLiteralStart() const {
    return isGenericPostfixStart(TokenType::LBRACE);
}

bool Parser::isGenericPostfixStart(TokenType following) const {
    if (peek().type != TokenType::LESS || !isTypeStartToken(peek(1).type)) {
        return false;
    }

    size_t i = 1;
    int depth = 0;
    bool sawTypeToken = false;

    while (peek(i).type != TokenType::EOF_TOKEN) {
        TokenType t = peek(i).type;

        if (t == TokenType::NOT && i > 1 && peek(i - 1).type == TokenType::ID
            && (peek(i + 1).type == TokenType::LPAREN || peek(i + 1).type == TokenType::LBRACE
                || peek(i + 1).type == TokenType::SQUARE_LBRACE)) {
            ++i;
            std::vector<TokenType> closing;
            do {
                const TokenType groupToken = peek(i).type;
                if (groupToken == TokenType::EOF_TOKEN)
                    return false;
                if (groupToken == TokenType::LPAREN)
                    closing.push_back(TokenType::RPAREN);
                else if (groupToken == TokenType::LBRACE)
                    closing.push_back(TokenType::RBRACE);
                else if (groupToken == TokenType::SQUARE_LBRACE)
                    closing.push_back(TokenType::SQUARE_RBRACE);
                else if (groupToken == TokenType::RPAREN || groupToken == TokenType::RBRACE
                    || groupToken == TokenType::SQUARE_RBRACE) {
                    if (closing.empty() || closing.back() != groupToken)
                        return false;
                    closing.pop_back();
                }
                ++i;
            } while (!closing.empty());
            continue;
        }

        if (t == TokenType::LESS && isTypeStartToken(peek(i + 1).type)) {
            ++depth;
            sawTypeToken = true;
            ++i;
            continue;
        }

        if (t == TokenType::GREATER) {
            if (depth == 0) {
                if (!sawTypeToken) {
                    return false;
                }
                return peek(i + 1).type == following;
            }
            --depth;
            ++i;
            continue;
        }

        if (t == TokenType::COMMA || t == TokenType::DOT || t == TokenType::ID
            || t == TokenType::MULTIPLY || t == TokenType::ELLIPSIS || t == TokenType::SQUARE_LBRACE
            || t == TokenType::SQUARE_RBRACE || t == TokenType::INTEGER || t == TokenType::WEAK) {
            sawTypeToken = true;
            ++i;
            continue;
        }

        return false;
    }

    return false;
}

std::unique_ptr<TypeNode> Parser::typeNodeFromQualifiedExpression(const ExpressionNode* expr) {
    auto qualified = qualifiedNameFromExpression(expr);
    if (qualified.empty()) {
        return nullptr;
    }

    auto typeNode = _builder.makeType("");
    size_t start = 0;
    while (true) {
        size_t dot = qualified.find('.', start);
        auto part =
            qualified.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (dot == std::string::npos) {
            typeNode->typeName = part;
            break;
        }
        typeNode->qualifiers.push_back(part);
        start = dot + 1;
    }
    return typeNode;
}

std::unique_ptr<TypeNode> Parser::parseType() {
    DepthGuard depth(*this);
    if (peek().type == TokenType::REF) {
        Token refToken = eat(TokenType::REF);
        auto innerType = parseType();
        if (innerType) {
            innerType->isReference = true;
            _builder.setSpan(innerType.get(), SourceSpan::merge(refToken.span, innerType->span));
        }
        return innerType;
    }

    if (peek().type == TokenType::WEAK) {
        Token weakToken = eat(TokenType::WEAK);
        auto innerType = parseType();
        if (innerType) {
            innerType->isWeak = true;
            _builder.setSpan(innerType.get(), SourceSpan::merge(weakToken.span, innerType->span));
        }
        return innerType;
    }

    if (peek().type == TokenType::ELLIPSIS) {
        Token ellipsisToken = eat(TokenType::ELLIPSIS);
        auto baseType = parseType();

        auto variadicType = _builder.makeType("");
        variadicType->isVarArgs = true;
        variadicType->baseType = std::move(baseType);

        _builder.setSpan(
            variadicType.get(),
            SourceSpan::merge(ellipsisToken.span, variadicType->baseType->span)
        );
        return variadicType;
    }

    if (peek().type == TokenType::MULTIPLY) {
        Token starToken = eat(TokenType::MULTIPLY);

        if (peek().type == TokenType::FUN) {
            eat(TokenType::FUN);
            eat(TokenType::LPAREN);
            auto funPtrType = _builder.makeType("");
            funPtrType->isFunPtr = true;
            if (peek().type != TokenType::RPAREN) {
                do {
                    bool isSink = false;
                    bool isNoEscape = false;
                    while (peek().type == TokenType::ID
                        && (peek().value == "sink" || peek().value == "noescape")) {
                        if (peek().value == "sink") {
                            isSink = true;
                        } else {
                            isNoEscape = true;
                        }
                        eat(TokenType::ID);
                    }
                    funPtrType->funPtrParams.push_back(parseType());
                    funPtrType->funPtrParamSinks.push_back(isSink);
                    funPtrType->funPtrParamNoEscapes.push_back(isNoEscape);
                } while (peek().type == TokenType::COMMA
                    && eat(TokenType::COMMA).type == TokenType::COMMA);
            }
            Token rparenToken = eat(TokenType::RPAREN);
            if (peek().type == TokenType::REF) {
                eat(TokenType::REF);
                funPtrType->funPtrReturnsRef = true;
            }
            if (peek().type != TokenType::SEMICOLON && peek().type != TokenType::COMMA
                && peek().type != TokenType::RPAREN && peek().type != TokenType::ASSIGN
                && peek().type != TokenType::RBRACE && peek().type != TokenType::SQUARE_RBRACE) {
                funPtrType->funPtrReturn = parseType();
            } else {
                funPtrType->funPtrReturn = _builder.makeType("Void");
            }
            funPtrType->funPtrResultBorrowSource = parseResultBorrowSource();
            _builder.setSpan(
                funPtrType.get(),
                SourceSpan::merge(starToken.span, funPtrType->funPtrReturn->span)
            );
            return funPtrType;
        }

        auto baseType = parseType();

        auto pointerType = _builder.makeType("");
        pointerType->isPointer = true;
        pointerType->baseType = std::move(baseType);

        _builder.setSpan(
            pointerType.get(),
            SourceSpan::merge(starToken.span, pointerType->baseType->span)
        );
        return pointerType;
    }

    if (peek().type == TokenType::SQUARE_LBRACE
        && (peek(1).type == TokenType::SQUARE_RBRACE || peek(1).type == TokenType::INTEGER
            || peek(1).type == TokenType::ID || peek(1).type == TokenType::SQUARE_LBRACE)) {
        Token lbracket = eat(TokenType::SQUARE_LBRACE);
        std::unique_ptr<ExpressionNode> size = nullptr;
        if (peek().type != TokenType::SQUARE_RBRACE) {
            size = parseExpression();
        }
        Token rbracket = eat(TokenType::SQUARE_RBRACE);
        auto baseType = parseType();

        auto arrayType = _builder.makeType("");
        arrayType->isArray = true;
        arrayType->arraySize = std::move(size);
        arrayType->baseType = std::move(baseType);

        _builder.setSpan(
            arrayType.get(),
            SourceSpan::merge(lbracket.span, arrayType->baseType->span)
        );
        return arrayType;
    }
    if (isMacroInvocationStart()) {
        auto fragment = parseMacroInvocation(FragmentKind::Type);
        return std::move(std::get<std::unique_ptr<TypeNode>>(fragment));
    }
    Token startToken = peek();
    auto identifiers = parseQualifiedIdentifier();
    auto typeNode = _builder.makeType(identifiers.back());
    typeNode->syntaxName = SyntaxName(startToken);
    identifiers.pop_back();
    typeNode->qualifiers = std::move(identifiers);
    if (peek().type == TokenType::LESS && isTypeStartToken(peek(1).type)) {
        typeNode->genericArgs = parseGenericTypeArguments();
    }

    if (peek().type == TokenType::NOT) {
        eat(TokenType::NOT);
        auto errorType = parseType();

        auto failableType = _builder.makeType("");
        failableType->isFailable = true;
        failableType->baseType = std::move(typeNode);
        failableType->errorType = std::move(errorType);

        _builder.setSpan(
            failableType.get(),
            SourceSpan::merge(startToken.span, failableType->errorType->span)
        );
        return failableType;
    }

    _builder.setSpan(typeNode.get(), SourceSpan::merge(startToken.span, _cursor.previous().span));
    return typeNode;
}

} // namespace zap
