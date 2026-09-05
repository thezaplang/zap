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
