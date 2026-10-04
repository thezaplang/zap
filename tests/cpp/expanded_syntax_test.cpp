#include "frontend/expanded_syntax.hpp"
#include "frontend/frontend_session.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <unordered_set>

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
  require(session.bind(project), "expanded syntax fixture did not bind");

  const auto actual =
      zap::frontend::ExpandedSyntaxEmitter::renderProject(project);
  const auto expected = readFile(snapshotPath);
  require(expected.has_value(), "expanded syntax snapshot is missing");
  if (actual != *expected) {
    std::cerr << "snapshot: " << snapshotPath << "\nexpected:\n"
              << *expected << "\nactual:\n"
              << actual;
    std::exit(1);
  }
}

std::string checkBoundRoundTrip(const std::filesystem::path &sourcePath) {
  zap::frontend::FrontendSessionConfig config{
      zap::frontend::RuntimePaths({}, {}, {}, {},
                                  zap::frontend::EnvironmentOverrides::Ignore),
      {}};
  config.includePrelude = false;
  zap::frontend::FrontendSession session(config, readFile);
  auto project = session.load(sourcePath);
  require(project.loaded && session.bind(project),
          "hygiene fixture did not bind");
  std::unordered_set<const SyntaxOccurrence *> occurrences;
  for (const auto &[id, module] : project.modules) {
    for (const auto &token : module->expandedTokens) {
      if (token.type == TokenType::ID)
        require(token.occurrence &&
                    occurrences.insert(token.occurrence.get()).second,
                "copied macro identifiers shared an occurrence identity");
    }
  }
  const auto text =
      zap::frontend::ExpandedSyntaxEmitter::renderProject(project);
  require(text == zap::frontend::ExpandedSyntaxEmitter::renderProject(project),
          "expanded names were not deterministic");
  const auto expandedPath = sourcePath.parent_path() / "expanded-roundtrip.zp";
  zap::frontend::FrontendSession roundTripSession(
      config,
      [&](const std::filesystem::path &path) -> std::optional<std::string> {
        return path == expandedPath ? std::optional<std::string>(text)
                                    : readFile(path);
      });
  auto roundTrip = roundTripSession.load(expandedPath);
  require(roundTrip.loaded && roundTripSession.bind(roundTrip),
          "ordinary expanded syntax did not bind");
  return text;
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
  checkBoundRoundTrip(sourceRoot / "macro_hygiene" / "main.zp");
  checkBoundRoundTrip(sourceRoot / "macro_expanded_names_test.zp");
  const auto imports =
      checkBoundRoundTrip(sourceRoot / "macro_expanded_imports" / "main.zp");
  require(imports.find("ext var optind") != std::string::npos &&
              imports.find("ext var optind") == imports.rfind("ext var optind"),
          "duplicate external globals were retained");
  require(imports.find("pub import") == std::string::npos,
          "public import visibility leaked into flattened declarations");
}
