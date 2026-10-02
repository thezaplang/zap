#include "macros/macro_diagnostic_codes.hpp"
#include "macros/macro_parser.hpp"
#include "parser.hpp"

#include <utility>

namespace zap {

std::unique_ptr<RootNode> Parser::parse() {
  auto root = _builder.makeRoot();
  while (!isAtEnd()) {
    try {
      const size_t declarationStart = _cursor.position();
      auto attributes = parseAttributes();

      Visibility visibility = Visibility::Private;
      const bool hasVisibility =
          peek().type == TokenType::PUB || peek().type == TokenType::PRIV;
      if (peek().type == TokenType::PUB || peek().type == TokenType::PRIV) {
        visibility = (eat(peek().type).type == TokenType::PUB)
                         ? Visibility::Public
                         : Visibility::Private;
      }

      auto applyMetadata =
          [&visibility](Node *node, std::vector<AttributeNode> attrs = {}) {
            if (auto topLevel = dynamic_cast<TopLevel *>(node)) {
              topLevel->visibility_ = visibility;
              topLevel->attributes_ = std::move(attrs);
            }
          };

      if (isMacroInvocationStart()) {
        if (!attributes.empty() || hasVisibility) {
          _diag.report(peek().span, DiagnosticLevel::Error,
                       "Attributes and visibility cannot decorate a macro "
                       "invocation.");
        }
        auto generated = parseMacroItems();
        for (auto &item : generated->children)
          root->addChild(std::move(item));
      } else if (peek().type == TokenType::MACRO) {
        const bool hasAttributes = !attributes.empty();
        if (hasAttributes) {
          _diag.report(
              peek().span, DiagnosticLevel::Error,
              "Attributes on macro declarations are not supported yet.");
        }
        const size_t start = _cursor.position();
        auto result = MacroParser::parse(_tokens, start, _cursor.end(),
                                         visibility, _diag);
        _cursor.advance(result.nextPosition - start);
        recordExpansion(declarationStart, {});
        if (!result.definitions.empty() && !hasAttributes) {
          for (auto &definition : result.definitions)
            _macroDefinitions.push_back(std::move(definition));
        } else if (result.definitions.empty()) {
          synchronize(SyncContext::TopLevel);
        }
      } else if (peek().type == TokenType::IMPORT) {
        auto importDecl = parseImportDecl();
        applyMetadata(importDecl.get(), std::move(attributes));
        root->addChild(std::move(importDecl));
      } else if (peek().type == TokenType::UNSAFE &&
                 peek(1).type == TokenType::FUN) {
        eat(TokenType::UNSAFE);
        auto decl = parseFunDecl(true);
        applyMetadata(decl.get(), std::move(attributes));
        root->addChild(std::move(decl));
      } else if (peek().type == TokenType::FUN) {
        auto decl = parseFunDecl();
        applyMetadata(decl.get(), std::move(attributes));
        root->addChild(std::move(decl));
      } else if (peek().type == TokenType::EXTERN) {
        if (peek(1).type == TokenType::VAR) {
          Token externToken = eat(TokenType::EXTERN);
          eat(TokenType::VAR);
          Token nameToken = eat(TokenType::ID);
          eat(TokenType::COLON);
          auto typeNode = parseType();
          Token semiToken = eat(TokenType::SEMICOLON);
          auto varDecl =
              _builder.makeBindingDecl(nameToken.value, std::move(typeNode),
                                       nullptr, BindingKind::Mutable);
          varDecl->syntaxName_ = SyntaxName(nameToken);
          varDecl->isGlobal_ = true;
          varDecl->isExternal_ = true;
          _builder.setSpan(varDecl.get(),
                           SourceSpan::merge(externToken.span, semiToken.span));
          applyMetadata(varDecl.get(), std::move(attributes));
          root->addChild(std::move(varDecl));
        } else {
          auto decl = parseExtDecl();
          applyMetadata(decl.get(), std::move(attributes));
          root->addChild(std::move(decl));
        }
      } else if (peek().type == TokenType::ENUM) {
        auto decl = parseEnumDecl();
        applyMetadata(decl.get(), std::move(attributes));
        root->addChild(std::move(decl));
      } else if (peek().type == TokenType::ALIAS) {
        auto decl = parseTypeAliasDecl();
        applyMetadata(decl.get(), std::move(attributes));
        root->addChild(std::move(decl));
      } else if (peek().type == TokenType::UNSAFE &&
                 peek(1).type == TokenType::STRUCT) {
        eat(TokenType::UNSAFE);
        auto decl = parseStructDecl(true);
        applyMetadata(decl.get(), std::move(attributes));
        root->addChild(std::move(decl));
      } else if (peek().type == TokenType::STRUCT) {
        auto decl = parseStructDecl();
        applyMetadata(decl.get(), std::move(attributes));
        root->addChild(std::move(decl));
      } else if (peek().type == TokenType::RECORD) {
        auto decl = parseRecordDecl();
        applyMetadata(decl.get(), std::move(attributes));
        root->addChild(std::move(decl));
      } else if (peek().type == TokenType::CLASS) {
        auto decl = parseClassDecl();
        applyMetadata(decl.get(), std::move(attributes));
        root->addChild(std::move(decl));
      } else if (peek().type == TokenType::INTERFACE) {
        auto decl = parseInterfaceDecl();
        applyMetadata(decl.get(), std::move(attributes));
        root->addChild(std::move(decl));
      } else if (peek().type == TokenType::EXTEND) {
        auto decl = parseExtensionDecl();
        applyMetadata(decl.get(), std::move(attributes));
        root->addChild(std::move(decl));
      } else if (peek().type == TokenType::CONST) {
        auto decl = parseBindingDecl(BindingKind::CompileTimeConstant);
        applyMetadata(decl.get(), std::move(attributes));
        root->addChild(std::move(decl));
      } else if (peek().type == TokenType::GLOBAL) {
        Token globalToken = eat(TokenType::GLOBAL);
        if (peek().type == TokenType::VAR) {
          auto varDecl = parseBindingDecl(BindingKind::Mutable);
          varDecl->isGlobal_ = true;
          applyMetadata(varDecl.get(), std::move(attributes));
          _builder.setSpan(varDecl.get(),
                           SourceSpan::merge(globalToken.span, varDecl->span));
          root->addChild(std::move(varDecl));
        } else {
          _diag.report(peek().span, DiagnosticLevel::Error,
                       "Expected 'var' after 'global'");
          _cursor.advance();
          synchronize(SyncContext::TopLevel);
        }
      } else {
        _diag.report(peek().span, DiagnosticLevel::Error,
                     "Unexpected token " + peek().value);
        _cursor.advance();
        synchronize(SyncContext::TopLevel);
      }
    } catch (const ParseError &e) {
      synchronize(SyncContext::TopLevel);
    }
  }
  return root;
}

std::unique_ptr<RootNode> Parser::parseItemFragmentRoot() {
  auto root = parse();
  if (!_macroDefinitions.empty())
    _diag.report(_macroDefinitions.front().span, DiagnosticLevel::Error,
                 macro_diagnostic::Fragment,
                 "Macro expansion cannot define macros.");
  for (const auto &item : root->children)
    if (dynamic_cast<const ImportNode *>(item.get()))
      _diag.report(item->span, DiagnosticLevel::Error,
                   macro_diagnostic::Fragment,
                   "Macro expansion cannot generate imports.");
  return root;
}

} // namespace zap
