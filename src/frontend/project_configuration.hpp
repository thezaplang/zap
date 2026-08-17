#pragma once

#include "frontend/module_loader.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace zap::frontend {

struct ProjectConfiguration {
  std::filesystem::path manifestPath;
  std::filesystem::path rootDirectory;
  std::optional<std::filesystem::path> entryPath;
  ImportMap importMap;
};

struct ProjectConfigurationResult {
  std::optional<ProjectConfiguration> configuration;
  std::vector<std::string> errors;
};

ProjectConfigurationResult
loadProjectConfiguration(const std::filesystem::path &sourcePath);

} // namespace zap::frontend
