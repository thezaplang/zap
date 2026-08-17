#include "frontend/project_configuration.hpp"

#include <tomlc17.h>

#include <string_view>
#include <utility>

namespace zap::frontend {
namespace {

constexpr std::string_view manifestFileName = "thor.toml";

class ParsedToml {
  toml_result_t result_{};

public:
  explicit ParsedToml(const std::filesystem::path &path)
      : result_(toml_parse_file_ex(path.c_str())) {}
  ~ParsedToml() { toml_free(result_); }

  ParsedToml(const ParsedToml &) = delete;
  ParsedToml &operator=(const ParsedToml &) = delete;

  bool ok() const { return result_.ok; }
  std::string_view error() const { return result_.errmsg; }
  const toml_datum_t &root() const { return result_.toptab; }
};

std::filesystem::path canonicalPath(const std::filesystem::path &path) {
  std::error_code ec;
  auto canonical = std::filesystem::weakly_canonical(path, ec);
  return ec ? std::filesystem::absolute(path).lexically_normal() : canonical;
}

std::optional<std::filesystem::path>
findManifestPath(const std::filesystem::path &sourcePath) {
  std::filesystem::path directory = sourcePath;
  std::error_code ec;
  if (!std::filesystem::is_directory(directory, ec) || ec) {
    directory = directory.parent_path();
  }
  directory = canonicalPath(directory);

  while (true) {
    const auto manifest = directory / manifestFileName;
    if (std::filesystem::is_regular_file(manifest, ec) && !ec) {
      return canonicalPath(manifest);
    }
    const auto parent = directory.parent_path();
    if (parent == directory) {
      return std::nullopt;
    }
    directory = parent;
  }
}

std::string datumLocation(const toml_datum_t &datum) {
  if (datum.lineno <= 0) {
    return "";
  }
  return ":" + std::to_string(datum.lineno) + ":" +
         std::to_string(datum.colno);
}

std::string stringValue(const toml_datum_t &datum) {
  return std::string(datum.u.str.ptr, static_cast<size_t>(datum.u.str.len));
}

} // namespace

std::optional<std::filesystem::path>
findProjectConfigurationManifest(const std::filesystem::path &sourcePath) {
  return findManifestPath(sourcePath);
}

ProjectConfigurationResult
loadProjectConfiguration(const std::filesystem::path &manifestPath) {
  ProjectConfigurationResult result;
  ParsedToml document(manifestPath);
  if (!document.ok()) {
    result.errors.push_back("Could not parse " + manifestPath.string() +
                            ": " + std::string(document.error()));
    return result;
  }
  if (document.root().type != TOML_TABLE) {
    result.errors.push_back(manifestPath.string() +
                            " must contain a TOML table");
    return result;
  }

  ProjectConfiguration configuration;
  configuration.manifestPath = manifestPath;
  configuration.rootDirectory = manifestPath.parent_path();

  const toml_datum_t entry = toml_get(document.root(), "entry");
  if (entry.type != TOML_UNKNOWN) {
    if (entry.type != TOML_STRING || entry.u.str.len == 0) {
      result.errors.push_back("Property 'entry' in " + manifestPath.string() +
                              datumLocation(entry) +
                              " must be a non-empty string");
    } else {
      std::filesystem::path entryPath(stringValue(entry));
      if (entryPath.is_relative()) {
        entryPath = configuration.rootDirectory / entryPath;
      }
      configuration.entryPath = canonicalPath(entryPath);
    }
  }

  const toml_datum_t imports = toml_get(document.root(), "imports");
  if (imports.type != TOML_UNKNOWN) {
    if (imports.type != TOML_TABLE) {
      result.errors.push_back("Property 'imports' in " +
                              manifestPath.string() + datumLocation(imports) +
                              " must be a table");
    } else {
      for (int32_t i = 0; i < imports.u.tab.size; ++i) {
        const std::string alias(imports.u.tab.key[i],
                                static_cast<size_t>(imports.u.tab.len[i]));
        const toml_datum_t &target = imports.u.tab.value[i];
        if (target.type != TOML_STRING || target.u.str.len == 0) {
          result.errors.push_back("Import '" + alias + "' in " +
                                  manifestPath.string() + datumLocation(target) +
                                  " must have a non-empty string path");
          continue;
        }
        std::filesystem::path targetPath(stringValue(target));
        if (targetPath.is_relative()) {
          targetPath = configuration.rootDirectory / targetPath;
        }
        configuration.importMap.emplace(alias,
                                        canonicalPath(targetPath).string());
      }
    }
  }

  if (result.errors.empty()) {
    result.configuration = std::move(configuration);
  }
  return result;
}

} // namespace zap::frontend
