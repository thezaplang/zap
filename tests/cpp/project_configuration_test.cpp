#include "frontend/project_configuration.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

void writeFile(const std::filesystem::path &path, const std::string &contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  output << contents;
}

struct TemporaryDirectory {
  std::filesystem::path path;

  TemporaryDirectory() {
    const auto suffix =
        std::chrono::high_resolution_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
           ("zap-project-configuration-" + std::to_string(suffix));
    std::filesystem::create_directories(path);
  }

  ~TemporaryDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

} // namespace

int main() {
  TemporaryDirectory temporary;
  const auto project = temporary.path / "project";
  const auto source = project / "src" / "nested" / "main.zp";
  writeFile(source, "fun main() Int { return 0; }\n");
  writeFile(project / "thor.toml", R"(entry = "src/main.zp"

[imports]
"@vendor" = "./vendor/package/src"
local = "../shared"
)");

  const auto manifest = zap::frontend::findProjectConfigurationManifest(source);
  require(manifest.has_value(),
          "project configuration was not discovered from a nested source file");
  auto configured = zap::frontend::loadProjectConfiguration(*manifest);
  require(configured.errors.empty(),
          "valid project configuration was rejected");
  require(configured.configuration.has_value(),
          "valid configuration was not loaded");
  const auto &configuration = *configured.configuration;
  require(configuration.manifestPath == project / "thor.toml",
          "manifest path was not canonicalized");
  require(configuration.rootDirectory == project,
          "project root was not derived from thor.toml");
  require(configuration.entryPath == project / "src" / "main.zp",
          "entry was not resolved relative to thor.toml");
  require(configuration.importMap.at("@vendor") ==
              (project / "vendor" / "package" / "src").string(),
          "import alias was not resolved relative to thor.toml");
  require(configuration.importMap.at("local") ==
              (temporary.path / "shared").string(),
          "parent-relative import alias was not normalized");

  writeFile(project / "thor.toml", R"([imports]
invalid = 42
)");
  auto invalid = zap::frontend::loadProjectConfiguration(*manifest);
  require(!invalid.configuration.has_value(),
          "invalid import configuration was accepted");
  require(!invalid.errors.empty(), "invalid import configuration was silent");

  auto absent = zap::frontend::findProjectConfigurationManifest(
      temporary.path / "standalone" / "main.zp");
  require(!absent.has_value(),
          "missing thor.toml unexpectedly produced configuration");
}
