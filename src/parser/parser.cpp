#include "parser.hpp"
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace zap {
Parser::Parser(
    std::vector<Token> tokens,
    DiagnosticEngine& diag,
    MacroExpander* macroExpander,
    std::string moduleId,
    MacroParseMode macroMode
)
    : _diag(diag),
      _tokens(std::move(tokens)),
      _cursor(_tokens),
      _macroExpander(macroExpander),
      _moduleId(std::move(moduleId)),
      _macroMode(macroMode) {
    for (auto& token : _tokens)
        if (token.type == TokenType::ID)
            token.occurrence = std::make_shared<const SyntaxOccurrence>();
}

Parser::Parser(std::vector<Token> tokens, DiagnosticEngine& diag, size_t begin, size_t end)
    : _diag(diag),
      _tokens(std::move(tokens)),
      _cursor(_tokens, begin, end) {
    for (auto& token : _tokens)
        if (token.type == TokenType::ID)
            token.occurrence = std::make_shared<const SyntaxOccurrence>();
}

Parser::~Parser() {}

void Parser::checkSyntaxDepth(size_t depth) {
    if (depth > MaxSyntaxDepth) {
        _diag.report(
            peek().span,
            DiagnosticLevel::Error,
            "P1006",
            "Syntax nesting limit exceeded (" + std::to_string(MaxSyntaxDepth) + ")."
        );
        // A resource limit aborts this fragment; ordinary recovery would re-enter
        // the same deeply nested input while its parent frames are still alive.
        _cursor.advance(_cursor.end() - _cursor.position());
        throw ParseError();
    }
}

Parser::DepthGuard::DepthGuard(Parser& parser)
    : parser_(parser) {
    parser_.checkSyntaxDepth(parser_._syntaxDepth);
    ++parser_._syntaxDepth;
}

Parser::DepthGuard::~DepthGuard() {
    --parser_._syntaxDepth;
}

const std::vector<MacroDefinition>& Parser::macroDefinitions() const noexcept {
    return _macroDefinitions;
}

std::vector<MacroDefinition> Parser::takeMacroDefinitions() {
    return std::move(_macroDefinitions);
}

std::vector<AttributeNode> Parser::parseAttributes() {
    std::vector<AttributeNode> attributes;

    while (peek().type == TokenType::AT) {
        Token atToken = eat(TokenType::AT);

        if (peek().type == TokenType::LBRACE) {
            eat(TokenType::LBRACE);

            if (peek().type != TokenType::RBRACE) {
                do {
                    attributes.push_back(parseSingleAttribute());
                } while (peek().type == TokenType::COMMA
                    && eat(TokenType::COMMA).type == TokenType::COMMA);
            }

            eat(TokenType::RBRACE);
        } else {
            auto attr = parseSingleAttribute();
            attr.span = SourceSpan::merge(atToken.span, attr.span);
            attributes.push_back(std::move(attr));
        }
    }

    return attributes;
}

AttributeNode Parser::parseSingleAttribute() {
    Token nameToken = eat(TokenType::ID);
    AttributeNode attr;
    attr.name = nameToken.value;
    attr.span = nameToken.span;

    if (peek().type == TokenType::LPAREN) {
        eat(TokenType::LPAREN);

        if (peek().type != TokenType::RPAREN) {
            do {
                AttributeArgument arg;

                if (peek().type == TokenType::ID && peek(1).type == TokenType::COLON) {
                    Token argName = eat(TokenType::ID);
                    eat(TokenType::COLON);
                    arg.kind = AttributeArgumentKind::Named;
                    arg.name = argName.value;
                    arg.value = parseExpression();
                } else {
                    arg.kind = AttributeArgumentKind::Positional;
                    arg.value = parseExpression();
                }

                attr.arguments.push_back(std::move(arg));
            } while (
                peek().type == TokenType::COMMA && eat(TokenType::COMMA).type == TokenType::COMMA);
        }

        Token rparen = eat(TokenType::RPAREN);
        attr.span = SourceSpan::merge(nameToken.span, rparen.span);
    }

    return attr;
}

std::unique_ptr<ImportNode> Parser::parseImportDecl() {
    Token importKeyword = eat(TokenType::IMPORT);
    Token pathToken = eat(TokenType::STRING);
    std::string moduleAlias;
    std::vector<ImportBinding> bindings;

    if (peek().type == TokenType::AS) {
        eat(TokenType::AS);
        moduleAlias = eat(TokenType::ID).value;
    }

    if (peek().type == TokenType::LBRACE) {
        eat(TokenType::LBRACE);
        if (peek().type != TokenType::RBRACE) {
            do {
                Token sourceToken = eat(TokenType::ID);
                std::string localName = sourceToken.value;
                if (peek().type == TokenType::AS) {
                    eat(TokenType::AS);
                    localName = eat(TokenType::ID).value;
                }
                bindings.push_back({sourceToken.value, localName});
            } while (
                peek().type == TokenType::COMMA && eat(TokenType::COMMA).type == TokenType::COMMA);
        }
        eat(TokenType::RBRACE);
    }

    Token semiToken = eat(TokenType::SEMICOLON);
    auto importDecl =
        _builder.makeImport(pathToken.value, std::move(moduleAlias), std::move(bindings));
    _builder.setSpan(importDecl.get(), SourceSpan::merge(importKeyword.span, semiToken.span));
    return importDecl;
}

std::unique_ptr<FunDecl> Parser::parseFunDecl(bool isUnsafe, FunctionContext context) {
    bool isStatic = false;
    while (peek().type == TokenType::STATIC || peek().type == TokenType::UNSAFE) {
        if (peek().type == TokenType::STATIC) {
            eat(TokenType::STATIC);
            isStatic = true;
        } else {
            eat(TokenType::UNSAFE);
            isUnsafe = true;
        }
    }
    Token funKeyword = eat(TokenType::FUN);

    Token funNameToken = eat(TokenType::ID);
    auto funDecl = _builder.makeFunDecl(funNameToken.value);
    funDecl->syntaxName_ = SyntaxName(funNameToken);
    funDecl->isUnsafe_ = isUnsafe;
    funDecl->isStatic_ = isStatic;
    if (peek().type == TokenType::LESS && isTypeStartToken(peek(1).type)) {
        funDecl->genericParams_ = parseGenericParameterList();
    }

    eat(TokenType::LPAREN);

    if (context == FunctionContext::ExtensionMethod && !funDecl->isStatic_) {
        funDecl->extensionReceiverMode_ = ExtensionReceiverMode::Value;
    }

    if (context == FunctionContext::ExtensionMethod && !funDecl->isStatic_
        && peek().type == TokenType::REF && peek(1).type == TokenType::ID
        && peek(1).value == "self") {
        Token refToken = eat(TokenType::REF);
        Token selfToken = eat(TokenType::ID);
        if (peek().type == TokenType::COLON) {
            _diag.report(
                peek().span,
                DiagnosticLevel::Error,
                "Extension receiver 'ref self' must not declare a type."
            );
            throw ParseError();
        }
        if (peek().type != TokenType::COMMA && peek().type != TokenType::RPAREN) {
            _diag.report(
                peek().span,
                DiagnosticLevel::Error,
                "Expected ',' or ')' after extension receiver."
            );
            throw ParseError();
        }
        funDecl->extensionReceiverMode_ = ExtensionReceiverMode::Ref;
        funDecl->extensionReceiverSpan_ = SourceSpan::merge(refToken.span, selfToken.span);
        if (peek().type == TokenType::COMMA) {
            eat(TokenType::COMMA);
        }
    } else if (context == FunctionContext::ExtensionMethod && !funDecl->isStatic_
        && peek().type == TokenType::ID && peek().value == "self") {
        _diag.report(
            peek().span,
            DiagnosticLevel::Error,
            "Extension receiver is implicit; only 'ref self' may be "
            "declared explicitly."
        );
        throw ParseError();
    }

    if (peek().type != TokenType::RPAREN) {
        do {
            funDecl->params_.push_back(parseParameter());
        } while (peek().type == TokenType::COMMA && eat(TokenType::COMMA).type == TokenType::COMMA);
    }

    eat(TokenType::RPAREN);

    if (peek().type == TokenType::REF) {
        eat(TokenType::REF);
        funDecl->returnsRef_ = true;
    }

    if (peek().type != TokenType::LBRACE) {
        funDecl->returnType_ = parseType();
        funDecl->resultBorrowSource_ = parseResultBorrowSource(&funDecl->resultBorrowName_);
    } else {
        funDecl->returnType_.reset();
    }

    funDecl->genericConstraints_ = parseWhereClauses();

    eat(TokenType::LBRACE);

    funDecl->body_ = parseBody();

    Token rbraceToken = eat(TokenType::RBRACE);

    _builder.setSpan(funDecl.get(), SourceSpan::merge(funNameToken.span, rbraceToken.span));

    return funDecl;
}

std::unique_ptr<ExtDecl> Parser::parseExtDecl() {
    Token externKeyword = eat(TokenType::EXTERN);
    Token funKeyword = eat(TokenType::FUN);

    Token funNameToken = eat(TokenType::ID);
    auto extDecl = std::make_unique<ExtDecl>();
    extDecl->name_ = funNameToken.value;

    eat(TokenType::LPAREN);

    if (peek().type != TokenType::RPAREN) {
        do {
            if (peek().type == TokenType::ELLIPSIS) {
                eat(TokenType::ELLIPSIS);
                extDecl->isCVariadic_ = true;
                break;
            }
            extDecl->params_.push_back(parseParameter());
        } while (peek().type == TokenType::COMMA && eat(TokenType::COMMA).type == TokenType::COMMA);
    }

    eat(TokenType::RPAREN);

    if (peek().type != TokenType::SEMICOLON) {
        extDecl->returnType_ = parseType();
        extDecl->resultBorrowSource_ = parseResultBorrowSource(&extDecl->resultBorrowName_);
    } else {
        extDecl->returnType_ = _builder.makeType("Void");
        const auto& nextToken = peek();
        _builder.setSpan(
            extDecl->returnType_.get(),
            SourceSpan(
                nextToken.span.line,
                nextToken.span.column,
                nextToken.span.offset,
                0,
                nextToken.span.sourceName
            )
        );
    }

    Token semiToken = eat(TokenType::SEMICOLON);

    _builder.setSpan(extDecl.get(), SourceSpan::merge(funNameToken.span, semiToken.span));

    return extDecl;
}

std::optional<std::string> Parser::parseResultBorrowSource(SyntaxName* name) {
    if (peek().type != TokenType::ID || peek().value != "borrows") {
        return std::nullopt;
    }
    eat(TokenType::ID);
    eat(TokenType::LPAREN);
    std::string source;
    if (peek().type == TokenType::ID) {
        Token token = eat(TokenType::ID);
        source = token.value;
        if (name)
            *name = SyntaxName(token);
    } else {
        source = eat(TokenType::INTEGER).value;
    }
    eat(TokenType::RPAREN);
    return source;
}

std::unique_ptr<ParameterNode> Parser::parseParameter(bool allowDefault) {
    bool isRef = false;
    if (peek().type == TokenType::REF) {
        eat(TokenType::REF);
        isRef = true;
    }
    Token paramNameToken = eat(TokenType::ID);
    eat(TokenType::COLON);
    bool isSink = false;
    bool isNoEscape = false;
    while (peek().type == TokenType::ID && (peek().value == "sink" || peek().value == "noescape")) {
        if (peek().value == "sink") {
            isSink = true;
        } else {
            isNoEscape = true;
        }
        eat(TokenType::ID);
    }
    auto typeNode = parseType();
    auto* typeNodePtr = typeNode.get();
    bool isVariadic = typeNode && typeNode->isVarArgs;
    std::unique_ptr<ExpressionNode> defaultValue = nullptr;
    if (allowDefault && peek().type == TokenType::ASSIGN) {
        eat(TokenType::ASSIGN);
        defaultValue = parseExpression();
    }
    auto endSpan = defaultValue ? defaultValue->span : typeNodePtr->span;
    auto paramNode = _builder.makeParam(
        paramNameToken.value,
        std::move(typeNode),
        isRef,
        isSink,
        isVariadic,
        isNoEscape,
        std::move(defaultValue)
    );
    paramNode->syntaxName = SyntaxName(paramNameToken);
    _builder.setSpan(paramNode.get(), SourceSpan::merge(paramNameToken.span, endSpan));
    return paramNode;
}

const Token& Parser::peek(size_t offset) const {
    return _cursor.peek(offset);
}

Token Parser::eat(TokenType expectedType) {
    if (isAtEnd()) {
        _diag.report(
            peek().span,
            DiagnosticLevel::Error,
            "Expected " + tokenTypeToString(expectedType) + " but reached end of file."
        );
        throw ParseError();
    }
    Token current = _cursor.peek();
    if (current.type == expectedType) {
        _cursor.advance();
        return current;
    } else {
        _diag.report(
            current.span,
            DiagnosticLevel::Error,
            "Expected " + tokenTypeToString(expectedType) + ", but got '" + current.value + "'"
        );
        throw ParseError();
    }
}

SourceSpan Parser::pointAfter(const SourceSpan& span) const {
    size_t length = std::max<size_t>(span.length, 1);
    return SourceSpan(
        span.line,
        span.column + length,
        span.offset + span.length,
        1,
        span.sourceName
    );
}

void Parser::synchronize(SyncContext context) {
    size_t lastPos = _cursor.position();
    size_t stalledIterations = 0;
    const size_t kMaxStalledIterations = 8;
    const size_t kMaxScanTokens = 4096;
    size_t scannedTokens = 0;

    while (!isAtEnd()) {
        if (_cursor.position() == lastPos) {
            ++stalledIterations;
        } else {
            stalledIterations = 0;
            lastPos = _cursor.position();
        }

        if (stalledIterations > kMaxStalledIterations || scannedTokens > kMaxScanTokens) {
            if (!isAtEnd()) {
                _cursor.advance(); // force progress to avoid anti-recovery infinite
                // cascade
            }
            return;
        }

        ++scannedTokens;

        switch (peek().type) {
            case TokenType::SEMICOLON:
                _cursor.advance();
                return;
            case TokenType::RBRACE:
                return;

            case TokenType::FUN:
            case TokenType::IMPORT:
            case TokenType::ENUM:
            case TokenType::STRUCT:
            case TokenType::RECORD:
            case TokenType::CLASS:
            case TokenType::INTERFACE:
            case TokenType::EXTEND:
            case TokenType::ALIAS:
            case TokenType::EXTERN:
            case TokenType::GLOBAL:
            case TokenType::CONST:
            case TokenType::PUB:
            case TokenType::PRIV:
            case TokenType::PROT:
            case TokenType::AT:
            case TokenType::MACRO:
                if (context == SyncContext::TopLevel) {
                    return;
                }
                _cursor.advance();
                break;

            case TokenType::VAR:
            case TokenType::LET:
            case TokenType::CASE:
            case TokenType::IF:
            case TokenType::WHILE:
            case TokenType::FOR:
            case TokenType::RETURN:
            case TokenType::UNSAFE:
                if (context == SyncContext::TopLevel) {
                    _cursor.advance();
                    break;
                }
                return;

            default:
                _cursor.advance();
                break;
        }
    }
}

void Parser::synchronizeExtensionMember() {
    size_t braceDepth = 0;
    while (!isAtEnd()) {
        switch (peek().type) {
            case TokenType::LBRACE:
                ++braceDepth;
                _cursor.advance();
                break;
            case TokenType::RBRACE:
                if (braceDepth == 0) {
                    return;
                }
                --braceDepth;
                _cursor.advance();
                if (braceDepth == 0) {
                    return;
                }
                break;
            case TokenType::SEMICOLON:
                _cursor.advance();
                if (braceDepth == 0) {
                    return;
                }
                break;
            case TokenType::FUN:
            case TokenType::STATIC:
            case TokenType::UNSAFE:
            case TokenType::PUB:
            case TokenType::PRIV:
            case TokenType::PROT:
            case TokenType::AT:
                if (braceDepth == 0) {
                    return;
                }
                _cursor.advance();
                break;
            default:
                _cursor.advance();
                break;
        }
    }
}

void Parser::synchronizeCaseArm() {
    size_t braceDepth = 0;

    while (!isAtEnd()) {
        switch (peek().type) {
            case TokenType::LBRACE:
                ++braceDepth;
                _cursor.advance();
                break;

            case TokenType::RBRACE:
                if (braceDepth == 0) {
                    return;
                }
                --braceDepth;
                _cursor.advance();
                if (braceDepth == 0) {
                    return;
                }
                break;

            default:
                _cursor.advance();
                break;
        }
    }
}

std::vector<std::string> Parser::parseQualifiedIdentifier() {
    std::vector<std::string> parts;
    parts.push_back(eat(TokenType::ID).value);
    while (peek().type == TokenType::DOT) {
        eat(TokenType::DOT);
        parts.push_back(eat(TokenType::ID).value);
    }
    return parts;
}

bool Parser::isAtEnd() const {
    return _cursor.isAtEnd();
}

std::unique_ptr<EnumDecl> Parser::parseEnumDecl() {
    Token enumKeyword = eat(TokenType::ENUM);
    Token enumNameToken = eat(TokenType::ID);

    std::vector<EnumDecl::Entry> entries;
    eat(TokenType::LBRACE);

    while (peek().type != TokenType::RBRACE) {
        Token entryToken = eat(TokenType::ID);

        if (peek().type == TokenType::LPAREN) {
            eat(TokenType::LPAREN);
            auto payloadType = parseType();
            eat(TokenType::RPAREN);
            entries.emplace_back(entryToken.value, std::move(payloadType));
        } else if (peek().type == TokenType::ASSIGN) {
            Token assignToken = eat(TokenType::ASSIGN);

            bool isNegative = false;
            if (peek().type == TokenType::MINUS) {
                eat(TokenType::MINUS);
                isNegative = true;
            }

            Token valueToken = eat(TokenType::INTEGER);

            int base = 10;
            std::string parseValue = valueToken.value;
            if (valueToken.value.size() > 2 && valueToken.value[0] == '0') {
                if (valueToken.value[1] == 'x' || valueToken.value[1] == 'X') {
                    base = 16;
                } else if (valueToken.value[1] == 'b' || valueToken.value[1] == 'B') {
                    base = 2;
                    parseValue = valueToken.value.substr(2);
                } else if (valueToken.value[1] == 'o' || valueToken.value[1] == 'O') {
                    base = 8;
                    parseValue = valueToken.value.substr(2);
                }
            }

            try {
                uint64_t unsignedValue = std::stoull(parseValue, nullptr, base);
                int64_t signedValue = 0;

                if (isNegative) {
                    const uint64_t minAbs =
                        static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1ULL;
                    if (unsignedValue > minAbs) {
                        _diag.report(
                            valueToken.span,
                            DiagnosticLevel::Error,
                            "Enum value out of range for signed 64-bit integer: -"
                                + valueToken.value
                        );
                        throw ParseError();
                    }

                    if (unsignedValue == minAbs) {
                        signedValue = std::numeric_limits<int64_t>::min();
                    } else {
                        signedValue = -static_cast<int64_t>(unsignedValue);
                    }
                } else {
                    if (unsignedValue
                        > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
                        _diag.report(
                            valueToken.span,
                            DiagnosticLevel::Error,
                            "Enum value out of range for signed 64-bit integer: " + valueToken.value
                        );
                        throw ParseError();
                    }
                    signedValue = static_cast<int64_t>(unsignedValue);
                }

                entries.emplace_back(entryToken.value, signedValue);
            } catch (const ParseError&) {
                throw;
            } catch (const std::exception&) {
                _diag.report(
                    SourceSpan::merge(assignToken.span, valueToken.span),
                    DiagnosticLevel::Error,
                    "Invalid enum value, expected integer literal after '='."
                );
                throw ParseError();
            }
        } else {
            entries.emplace_back(entryToken.value);
        }

        if (peek().type != TokenType::COMMA) {
            break;
        }

        eat(TokenType::COMMA);
    }

    Token rbraceToken = eat(TokenType::RBRACE);

    auto enumDecl = _builder.makeEnumDecl(enumNameToken.value, std::move(entries));
    _builder.setSpan(enumDecl.get(), SourceSpan::merge(enumKeyword.span, rbraceToken.span));
    return enumDecl;
}

std::unique_ptr<TypeAliasDecl> Parser::parseTypeAliasDecl() {
    Token aliasToken = eat(TokenType::ALIAS);
    Token nameToken = eat(TokenType::ID);
    eat(TokenType::ASSIGN);
    auto type = parseType();
    Token semiToken = eat(TokenType::SEMICOLON);

    auto aliasDecl = _builder.makeTypeAliasDecl(nameToken.value, std::move(type));
    _builder.setSpan(aliasDecl.get(), SourceSpan::merge(aliasToken.span, semiToken.span));
    return aliasDecl;
}

std::unique_ptr<RecordDecl> Parser::parseRecordDecl() {
    Token recordKeyword = eat(TokenType::RECORD);
    Token recordNameToken = eat(TokenType::ID);
    std::vector<std::unique_ptr<TypeNode>> genericParams;
    if (peek().type == TokenType::LESS && isTypeStartToken(peek(1).type)) {
        genericParams = parseGenericParameterList();
    }
    auto genericConstraints = parseWhereClauses();

    std::vector<std::unique_ptr<ParameterNode>> fields;
    eat(TokenType::LBRACE);

    while (peek().type != TokenType::RBRACE) {
        fields.push_back(parseParameter());
        if (peek().type == TokenType::COMMA || peek().type == TokenType::SEMICOLON) {
            eat(peek().type);
        }
    }

    Token rbraceToken = eat(TokenType::RBRACE);

    auto recordDecl =
        _builder.makeRecordDecl(recordNameToken.value, std::move(genericParams), std::move(fields));
    recordDecl->genericConstraints_ = std::move(genericConstraints);
    _builder.setSpan(recordDecl.get(), SourceSpan::merge(recordKeyword.span, rbraceToken.span));
    return recordDecl;
}

std::unique_ptr<ClassDecl> Parser::parseClassDecl() {
    Token classKeyword = eat(TokenType::CLASS);
    Token classNameToken = eat(TokenType::ID);

    auto classDecl = _builder.makeClassDecl(classNameToken.value);
    if (peek().type == TokenType::LESS && isTypeStartToken(peek(1).type)) {
        classDecl->genericParams_ = parseGenericParameterList();
    }
    if (peek().type == TokenType::COLON) {
        eat(TokenType::COLON);
        classDecl->implementsList_.push_back(parseType());
        while (peek().type == TokenType::COMMA) {
            eat(TokenType::COMMA);
            classDecl->implementsList_.push_back(parseType());
        }
    }
    classDecl->genericConstraints_ = parseWhereClauses();

    eat(TokenType::LBRACE);

    while (peek().type != TokenType::RBRACE) {
        auto attributes = parseAttributes();
        Visibility memberVisibility = parseMemberVisibility();

        if (peek().type == TokenType::FUN || peek().type == TokenType::STATIC
            || peek().type == TokenType::UNSAFE) {
            auto method = parseMemberMethod(
                std::move(attributes),
                memberVisibility,
                FunctionContext::Regular
            );
            classDecl->methods_.push_back(std::move(method));
        } else {
            for (const auto& attribute : attributes) {
                _diag.report(
                    attribute.span,
                    DiagnosticLevel::Error,
                    "attributes can only be applied to class methods"
                );
            }
            auto field = parseParameter();
            field->visibility_ = memberVisibility;
            classDecl->fields_.push_back(std::move(field));
            if (peek().type == TokenType::COMMA || peek().type == TokenType::SEMICOLON) {
                eat(peek().type);
            }
        }
    }

    Token rbraceToken = eat(TokenType::RBRACE);
    _builder.setSpan(classDecl.get(), SourceSpan::merge(classKeyword.span, rbraceToken.span));
    return classDecl;
}

std::unique_ptr<InterfaceDecl> Parser::parseInterfaceDecl() {
    Token interfaceKeyword = eat(TokenType::INTERFACE);
    Token nameToken = eat(TokenType::ID);

    auto interfaceDecl = _builder.makeInterfaceDecl(nameToken.value);

    eat(TokenType::LBRACE);

    while (peek().type != TokenType::RBRACE) {
        Token funKeyword = eat(TokenType::FUN);
        Token methodNameToken = eat(TokenType::ID);
        auto methodDecl = _builder.makeFunDecl(methodNameToken.value);
        methodDecl->syntaxName_ = SyntaxName(methodNameToken);

        eat(TokenType::LPAREN);
        if (peek().type != TokenType::RPAREN) {
            do {
                methodDecl->params_.push_back(parseParameter());
            } while (
                peek().type == TokenType::COMMA && eat(TokenType::COMMA).type == TokenType::COMMA);
        }
        eat(TokenType::RPAREN);

        if (peek().type != TokenType::SEMICOLON) {
            methodDecl->returnType_ = parseType();
        } else {
            methodDecl->returnType_ = _builder.makeType("Void");
        }

        Token semiToken = eat(TokenType::SEMICOLON);
        _builder.setSpan(methodDecl.get(), SourceSpan::merge(funKeyword.span, semiToken.span));
        interfaceDecl->methods_.push_back(std::move(methodDecl));
    }

    Token rbraceToken = eat(TokenType::RBRACE);
    _builder.setSpan(
        interfaceDecl.get(),
        SourceSpan::merge(interfaceKeyword.span, rbraceToken.span)
    );
    return interfaceDecl;
}

std::unique_ptr<StructDeclarationNode> Parser::parseStructDecl(bool isUnsafe) {
    Token structKeyword = eat(TokenType::STRUCT);
    Token structNameToken = eat(TokenType::ID);
    std::vector<std::unique_ptr<TypeNode>> genericParams;
    if (peek().type == TokenType::LESS && isTypeStartToken(peek(1).type)) {
        genericParams = parseGenericParameterList();
    }
    auto genericConstraints = parseWhereClauses();

    std::vector<std::unique_ptr<ParameterNode>> fields;
    eat(TokenType::LBRACE);

    if (peek().type != TokenType::RBRACE) {
        do {
            fields.push_back(parseParameter(true));

            if (peek().type == TokenType::COMMA || peek().type == TokenType::SEMICOLON) {
                eat(peek().type);
            } else {
                break;
            }
        } while (peek().type != TokenType::RBRACE);
    }

    eat(TokenType::RBRACE);
    auto decl = std::make_unique<StructDeclarationNode>(
        structNameToken.value,
        std::move(genericParams),
        std::move(fields),
        isUnsafe
    );
    decl->genericConstraints_ = std::move(genericConstraints);
    return decl;
}

} // namespace zap
