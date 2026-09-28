#include "frontend/expanded_syntax.hpp"
#include "frontend/frontend_session.hpp"
#include "lexer/lexer.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

std::optional<std::string> readFile(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return std::nullopt;
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

void compareSnapshot(const std::filesystem::path &sourcePath,
                     const std::filesystem::path &snapshotPath) {
  zap::frontend::FrontendSessionConfig config{
      zap::frontend::RuntimePaths({}, {}, {}, {},
                                  zap::frontend::EnvironmentOverrides::Ignore),
      {}};
  config.includePrelude = false;
  zap::frontend::FrontendSession session(config, readFile);
  auto project = session.load(sourcePath);
  require(project.loaded, "expanded syntax fixture did not parse");

  std::string actual;
  for (const auto &[moduleId, module] : project.modules) {
    zap::DiagnosticEngine diagnostics(module->sourceText, moduleId);
    Lexer lexer(diagnostics);
    auto tokens = lexer.tokenize(module->sourceText);
    zap::frontend::ExpandedSyntaxEmitter emitter(project.macros, diagnostics);
    auto expanded = emitter.expand(moduleId, tokens);
    require(expanded && !diagnostics.hadErrors(),
            "expanded syntax emitter rejected a parsed module");
    actual += zap::frontend::ExpandedSyntaxEmitter::render(
        project.entryModuleId, moduleId, *expanded);
  }

  const auto expected = readFile(snapshotPath);
  require(expected.has_value(), "expanded syntax snapshot is missing");
  if (actual != *expected) {
    std::cerr << "snapshot: " << snapshotPath << "\nexpected:\n"
              << *expected << "\nactual:\n"
              << actual;
    std::exit(1);
  }
}

} // namespace

int main() {
  const auto sourceRoot =
      std::filesystem::weakly_canonical(std::filesystem::path(__FILE__))
          .parent_path()
          .parent_path();
  compareSnapshot(sourceRoot / "macro_import" / "main.zp",
                  sourceRoot / "snapshots" / "macro_import.expanded.snap");
  compareSnapshot(sourceRoot / "macro_expression_type_test.zp",
                  sourceRoot / "snapshots" /
                      "macro_expression_type.expanded.snap");
}
