#include "frontend/frontend_session.hpp"
#include "utils/diagnostics.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

struct Fixture {
  std::filesystem::path entry = std::filesystem::weakly_canonical(
      std::filesystem::path(ZAP_TEST_SOURCE_DIR) / "tests" / "macro_import" /
      "main.zp");
  std::filesystem::path helper = entry.parent_path() / "helper.zp";
  std::map<std::filesystem::path, std::string> sources;

  zap::frontend::FrontendProject load() const {
    zap::frontend::FrontendSessionConfig config{
        zap::frontend::RuntimePaths(
            {}, {}, {}, {}, zap::frontend::EnvironmentOverrides::Ignore),
        {}};
    config.includePrelude = false;
    zap::frontend::FrontendSession session(
        config,
        [this](
            const std::filesystem::path &path) -> std::optional<std::string> {
          auto found = sources.find(path);
          return found == sources.end()
                     ? std::nullopt
                     : std::optional<std::string>(found->second);
        });
    auto project = session.load(entry);
    if (project.loaded)
      session.bind(project);
    return project;
  }
};

const zap::Diagnostic *
findDiagnostic(const zap::frontend::FrontendProject &project,
               const std::string &code, zap::DiagnosticLevel level) {
  for (const auto &diagnostic : project.diagnostics) {
    if (diagnostic.code == code && diagnostic.level == level)
      return &diagnostic;
  }
  return nullptr;
}

void testParserError() {
  Fixture fixture;
  fixture.sources[fixture.entry] =
      "macro broken() { 1 + }\nfun main() Int { return broken!(); }\n";
  auto project = fixture.load();
  const auto *error =
      findDiagnostic(project, "P1002", zap::DiagnosticLevel::Error);
  const auto *definition =
      findDiagnostic(project, "M2002", zap::DiagnosticLevel::Note);
  require(error && error->fileName == fixture.entry.string() &&
              error->span.line == 2 &&
              error->sourceText == fixture.sources[fixture.entry],
          "macro parser error did not point to the invocation");
  require(definition && definition->span.line == 1,
          "macro parser error lost its definition note");
  std::ostringstream rendered;
  zap::DiagnosticTextFormatter::print(rendered, project.diagnostics);
  require(
      rendered.str().find(" --> " + fixture.entry.string() + ":2:25\n") !=
              std::string::npos &&
          rendered.str().find("2 | fun main() Int { return broken!(); }") !=
              std::string::npos &&
          rendered.str().find("1 | macro broken() { 1 + }") !=
              std::string::npos,
      "macro parser diagnostic text lost its invocation caret or definition");
}

void testSemanticError() {
  Fixture fixture;
  fixture.sources[fixture.entry] = "macro broken() { missing_name }\n"
                                   "fun main() Int { return broken!(); }\n";
  auto project = fixture.load();
  const auto *error =
      findDiagnostic(project, "S2001", zap::DiagnosticLevel::Error);
  const auto *definition =
      findDiagnostic(project, "M2002", zap::DiagnosticLevel::Note);
  require(error && error->span.line == 2 && error->span.column == 25,
          "macro semantic error did not point to the invocation");
  require(definition && definition->span.line == 1,
          "macro semantic error lost its definition note");
}

void testCrossModuleNestedExpansion() {
  Fixture fixture;
  fixture.sources[fixture.entry] = "import \"helper.zp\" as helper { inner };\n"
                                   "macro outer() { inner!() }\n"
                                   "fun main() Int { return outer!(); }\n";
  fixture.sources[fixture.helper] = "pub macro inner() { missing_name }\n";
  auto project = fixture.load();
  const auto *error =
      findDiagnostic(project, "S2001", zap::DiagnosticLevel::Error);
  require(error && error->fileName == fixture.entry.string() &&
              error->span.line == 3,
          "nested macro error did not point to the outer invocation");
  bool helperDefinition = false;
  bool outerDefinition = false;
  bool nestedInvocation = false;
  for (const auto &diagnostic : project.diagnostics) {
    helperDefinition |=
        diagnostic.code == "M2002" &&
        diagnostic.fileName == fixture.helper.string() &&
        diagnostic.sourceText == fixture.sources[fixture.helper];
    outerDefinition |= diagnostic.code == "M2002" &&
                       diagnostic.fileName == fixture.entry.string() &&
                       diagnostic.span.line == 2;
    nestedInvocation |= diagnostic.code == "M2001";
  }
  require(helperDefinition && outerDefinition && nestedInvocation,
          "nested cross-module expansion lost trace frames or source text");
  std::ostringstream rendered;
  zap::DiagnosticTextFormatter::print(rendered, project.diagnostics);
  require(rendered.str().find("1 | pub macro inner() { missing_name }") !=
              std::string::npos,
          "cross-module macro definition note used the caller source text");
}

void testTraceLimit() {
  const std::string source = "call!()\n";
  zap::DiagnosticEngine diagnostics(source, "call.zp");
  diagnostics.registerSource("definition.zp", "abcdefghijkl\n");
  std::shared_ptr<const ExpansionOrigin> origin;
  for (size_t index = 0; index < 12; ++index) {
    origin = std::make_shared<ExpansionOrigin>(ExpansionOrigin{
        SourceSpan(1, 1, 0, 7, "call.zp"),
        SourceSpan(1, index + 1, index, 1, "definition.zp"), origin,
        "definition.zp", static_cast<SyntaxContextId>(index + 1)});
  }
  SourceSpan generated(1, 1, 0, 7, "call.zp");
  generated.expansionOrigin = origin;
  diagnostics.report(generated, zap::DiagnosticLevel::Error, "M1004",
                     "Macro expansion depth limit exceeded.");
  size_t definitions = 0;
  size_t omissions = 0;
  for (const auto &diagnostic : diagnostics.diagnostics()) {
    definitions += diagnostic.code == "M2002";
    omissions += diagnostic.code == "M2003";
  }
  require(definitions == 8 && omissions == 1,
          "macro trace did not cap frames and report omissions");
  std::ostringstream rendered;
  diagnostics.printText(rendered);
  require(rendered.str().find(
              "error: M1004 Macro expansion depth limit exceeded.") !=
                  std::string::npos &&
              rendered.str().find("4 more macro expansion frame(s) omitted") !=
                  std::string::npos,
          "macro depth diagnostic rendering changed");
}

} // namespace

int main() {
  testParserError();
  testSemanticError();
  testCrossModuleNestedExpansion();
  testTraceLimit();
}
