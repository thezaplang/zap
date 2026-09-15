#include "frontend/frontend_session.hpp"

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
} // namespace

int main() {
  zap::frontend::FrontendSessionConfig config{
      zap::frontend::RuntimePaths({}, {}, {}, {},
                                  zap::frontend::EnvironmentOverrides::Ignore),
      {}};
  config.includePrelude = false;
  const auto entry =
      std::filesystem::current_path() / "frontend_session_test.zp";
  std::string source = "fun main() Int { return 42; }";
  zap::frontend::FrontendSession session(
      config,
      [&](const std::filesystem::path &path) -> std::optional<std::string> {
        return path == entry ? std::optional<std::string>(source)
                             : std::nullopt;
      });

  auto project = session.load(entry);
  require(project.loaded, "in-memory source did not load");
  require(session.bind(project), "valid source did not bind");
  require(project.diagnostics.empty(), "valid source produced diagnostics");
  require(project.boundRoot != nullptr, "bound root was not retained");

  source = R"(
extend Int {
  pub fun identity() Int { return self; }
}

fun main() Int { return 42; }
  )";
  auto extension = session.load(entry);
  require(extension.loaded, "extension source did not load");
  require(session.bind(extension), "concrete extension did not bind");
  require(extension.diagnostics.empty(),
          "concrete extension produced diagnostics");
  require(extension.boundRoot != nullptr,
          "concrete extension did not retain a bound root");
  require(extension.boundRoot->functions.size() == 2,
          "extension method was not emitted as a bound function");

  source = R"(
extend Int {
  pub fun increment(amount: Int) Int { return self + amount; }
}

fun main() Int {
  var value: Int = 40;
  return value.increment(2);
}
  )";
  auto extensionCall = session.load(entry);
  require(extensionCall.loaded, "extension call source did not load");
  require(session.bind(extensionCall), "extension call did not bind");
  require(extensionCall.diagnostics.empty(),
          "extension call produced diagnostics");
  const auto *mainFunction = extensionCall.boundRoot->functions.back().get();
  require(mainFunction && mainFunction->body,
          "extension call did not produce a main body");
  const auto *returnStatement = dynamic_cast<sema::BoundReturnStatement *>(
      mainFunction->body->statements.back().get());
  const auto *extensionFunctionCall =
      returnStatement
          ? dynamic_cast<sema::BoundFunctionCall *>(
                returnStatement->expression.get())
          : nullptr;
  require(extensionFunctionCall &&
              extensionFunctionCall->symbol->isExtensionMethod,
          "member call did not resolve to an extension method");
  require(extensionFunctionCall->arguments.size() == 2,
          "extension call did not prepend its receiver");

  source = R"(
extend Int {
  pub fun increment(amount: Int) Int { return self + amount; }
}

fun main() Int { return (40).increment(2); }
  )";
  auto literalExtensionCall = session.load(entry);
  require(literalExtensionCall.loaded,
          "literal extension call source did not load");
  require(session.bind(literalExtensionCall),
          "literal extension call did not bind");
  require(literalExtensionCall.diagnostics.empty(),
          "literal extension call produced diagnostics");

  source = R"(
extend Int {
  fun mutate(ref self) {}
}

fun main() Int { return 42; }
  )";
  auto unsupportedExtension = session.load(entry);
  require(unsupportedExtension.loaded,
          "ref extension source did not load");
  require(session.bind(unsupportedExtension),
          "ref extension receiver did not bind");
  require(unsupportedExtension.diagnostics.empty(),
          "ref extension receiver produced diagnostics");

  source = R"(
extend Int {
  fun identity() Int { return self; }
}

extend Int {
  fun identity() Int { return self; }
}

fun main() Int { return 42; }
  )";
  auto duplicateExtension = session.load(entry);
  require(duplicateExtension.loaded,
          "duplicate extension source did not load");
  require(!session.bind(duplicateExtension),
          "duplicate extension method unexpectedly bound");

  source = "fun main() Int { return missing_name; }";
  auto invalid = session.load(entry);
  require(invalid.loaded, "syntactically valid source did not load");
  session.bind(invalid);
  bool hasError = false;
  for (const auto &diagnostic : invalid.diagnostics) {
    hasError |= diagnostic.level == zap::DiagnosticLevel::Error;
  }
  require(hasError, "undefined name did not produce a semantic error");
}
