#include "ast/extension_decl.hpp"
#include "ast/root_node.hpp"
#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#include "utils/diagnostics.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

struct ParseResult {
    std::shared_ptr<std::string> source;
    std::unique_ptr<RootNode> root;
    std::unique_ptr<zap::DiagnosticEngine> diagnostics;
};

ParseResult parse(const std::string& source) {
    auto ownedSource = std::make_shared<std::string>(source);
    auto diagnostics = std::make_unique<zap::DiagnosticEngine>(*ownedSource);
    Lexer lexer(*diagnostics);
    auto tokens = lexer.tokenize(*ownedSource);
    zap::Parser parser(tokens, *diagnostics);
    return {std::move(ownedSource), parser.parse(), std::move(diagnostics)};
}

void testBasicExtension() {
    const std::string source = R"(
extend Int {
  pub fun abs() Int { return self; }
  fun increment(ref self, amount: Int) { self = self + amount; }
  static fun identity(value: Int) Int { return value; }
}
)";
    auto result = parse(source);
    require(!result.diagnostics->hadErrors(), "valid extension produced parser diagnostics");
    require(result.root->children.size() == 1, "extension was not added to the root");

    auto* extension = dynamic_cast<ExtensionDecl*>(result.root->children.front().get());
    require(extension != nullptr, "root child is not an ExtensionDecl");
    require(
        extension->targetType_ && extension->targetType_->typeName == "Int",
        "extension target was not parsed"
    );
    require(extension->methods_.size() == 3, "extension methods were not parsed");
    require(
        extension->methods_[0]->visibility_ == Visibility::Public,
        "extension method visibility was not retained"
    );
    require(
        extension->methods_[0]->extensionReceiverMode_ == ExtensionReceiverMode::Value,
        "ordinary extension method did not get an implicit receiver"
    );
    require(
        extension->methods_[1]->extensionReceiverMode_ == ExtensionReceiverMode::Ref,
        "ref extension receiver was not parsed"
    );
    require(
        extension->methods_[1]->params_.size() == 1
            && extension->methods_[1]->params_[0]->name == "amount",
        "parameters following ref self were not parsed"
    );
    require(
        extension->methods_[2]->isStatic_
            && extension->methods_[2]->extensionReceiverMode_ == ExtensionReceiverMode::None,
        "static extension method acquired an implicit receiver"
    );
}

void testGenericQualifiedExtension() {
    const std::string source = R"(
extend<T> collections.List<T> where T: Comparable {
  pub unsafe fun first() T { return self.at(0); }
}
)";
    auto result = parse(source);
    require(
        !result.diagnostics->hadErrors(),
        "valid generic extension produced parser diagnostics"
    );
    auto* extension = dynamic_cast<ExtensionDecl*>(result.root->children.front().get());
    require(extension != nullptr, "generic extension was not parsed");
    require(extension->genericParams_.size() == 1, "extension generic parameter was not parsed");
    require(
        extension->targetType_->qualifiers.size() == 1
            && extension->targetType_->qualifiers[0] == "collections"
            && extension->targetType_->typeName == "List"
            && extension->targetType_->genericArgs.size() == 1,
        "qualified generic target was not parsed"
    );
    require(extension->genericConstraints_.size() == 1, "extension where clause was not parsed");
    require(
        extension->methods_.front()->isUnsafe_,
        "unsafe extension method flag was not retained"
    );
}

void testPointerAndArrayTargets() {
    const std::string source = R"(
extend *Char {
  unsafe fun next() *Char { return self; }
}
extend [4]Int {
  fun first() Int { return self[0]; }
}
extend<T> []T {
  fun count() Int { return 0; }
}
)";
    auto result = parse(source);
    require(
        !result.diagnostics->hadErrors(),
        "pointer or array extension target produced parser diagnostics"
    );
    require(result.root->children.size() == 3, "pointer and array extensions were not parsed");

    auto* pointer = dynamic_cast<ExtensionDecl*>(result.root->children[0].get());
    auto* array = dynamic_cast<ExtensionDecl*>(result.root->children[1].get());
    auto* slice = dynamic_cast<ExtensionDecl*>(result.root->children[2].get());
    require(
        pointer && pointer->targetType_->isPointer
            && pointer->targetType_->baseType->typeName == "Char",
        "pointer extension target was not retained"
    );
    require(
        array && array->targetType_->isArray && array->targetType_->arraySize != nullptr,
        "fixed array extension target was not retained"
    );
    require(
        slice && slice->targetType_->isArray && slice->targetType_->arraySize == nullptr
            && slice->genericParams_.size() == 1,
        "generic slice extension target was not retained"
    );
}

void testImplIsAnIdentifier() {
    auto result = parse("fun impl() Int { return 1; }");
    require(!result.diagnostics->hadErrors(), "impl remained reserved after introducing extend");
    require(result.root->children.size() == 1, "function named impl was not parsed");
    auto* function = dynamic_cast<FunDecl*>(result.root->children[0].get());
    require(
        function && function->extensionReceiverMode_ == ExtensionReceiverMode::None,
        "ordinary function acquired an extension receiver"
    );
}

void testIntegerLiteralMemberAccess() {
    const std::string source = "fun text() String { return 1.toString(); }";
    zap::DiagnosticEngine diagnostics(source);
    Lexer lexer(diagnostics);
    const auto tokens = lexer.tokenize(source);

    bool foundIntegerMemberAccess = false;
    for (size_t index = 0; index + 2 < tokens.size(); ++index) {
        foundIntegerMemberAccess |= tokens[index].type == TokenType::INTEGER
            && tokens[index].value == "1" && tokens[index + 1].type == TokenType::DOT
            && tokens[index + 2].type == TokenType::ID && tokens[index + 2].value == "toString";
    }
    require(foundIntegerMemberAccess, "integer literal member access was lexed as a float literal");

    const auto decimalTokens = lexer.tokenize("1.5 1.");
    require(
        decimalTokens.size() == 2 && decimalTokens[0].type == TokenType::FLOAT
            && decimalTokens[0].value == "1.5" && decimalTokens[1].type == TokenType::FLOAT
            && decimalTokens[1].value == "1.",
        "decimal literals no longer retain their previous tokenization"
    );

    auto result = parse(source);
    require(
        !result.diagnostics->hadErrors(),
        "integer literal member access produced parser diagnostics"
    );
}

void testInvalidReceiverRecovers() {
    const std::string source = R"(
extend Int {
  fun broken(ref self: Int) Int { return self; }
  pub fun valid() Int { return self; }
}
fun after() Int { return 1; }
)";
    auto result = parse(source);
    require(result.diagnostics->hadErrors(), "typed extension receiver did not produce an error");
    bool foundReceiverError = false;
    for (const auto& diagnostic : result.diagnostics->diagnostics()) {
        foundReceiverError |=
            diagnostic.message.find("must not declare a type") != std::string::npos;
    }
    require(foundReceiverError, "typed receiver diagnostic was not emitted");
    require(
        result.root->children.size() == 2,
        "parser did not recover cleanly after invalid extension receiver"
    );
    auto* extension = dynamic_cast<ExtensionDecl*>(result.root->children[0].get());
    require(
        extension && extension->methods_.size() == 1 && extension->methods_[0]->name_ == "valid",
        "parser did not resume at the next extension method"
    );
    auto* function = dynamic_cast<FunDecl*>(result.root->children[1].get());
    require(
        function && function->name_ == "after",
        "parser did not resume at the next top-level declaration"
    );
}

} // namespace

int main() {
    testBasicExtension();
    testGenericQualifiedExtension();
    testPointerAndArrayTargets();
    testImplIsAnIdentifier();
    testIntegerLiteralMemberAccess();
    testInvalidReceiverRecovers();
}
