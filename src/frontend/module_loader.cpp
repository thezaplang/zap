#include "frontend/module_loader.hpp"

#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace zap::frontend {

std::optional<std::filesystem::path>
currentExecutablePath(const std::filesystem::path &argv0Hint) {
  std::error_code ec;
  auto procPath = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (!ec && !procPath.empty()) {
    return std::filesystem::weakly_canonical(procPath);
  }

  if (!argv0Hint.empty()) {
    auto resolved = argv0Hint.is_absolute()
                        ? argv0Hint
                        : std::filesystem::current_path() / argv0Hint;
    return std::filesystem::weakly_canonical(resolved, ec);
  }

  return std::nullopt;
}

std::filesystem::path stdlibRootPath(const RuntimePaths &paths) {
  if (const char *configured = std::getenv("ZAPC_STDLIB_DIR")) {
    if (*configured != '\0') {
      return std::filesystem::path(configured);
    }
  }

  if (auto exePath = currentExecutablePath(paths.executablePath)) {
    auto siblingStd = exePath->parent_path() / "std";
    if (std::filesystem::exists(siblingStd) &&
        std::filesystem::is_directory(siblingStd)) {
      return siblingStd;
    }
  }

  return paths.configuredStdlibDir;
}

std::filesystem::path coreRootPath(const RuntimePaths &paths) {
  if (const char *configured = std::getenv("ZAPC_CORE_DIR")) {
    if (*configured != '\0') {
      return std::filesystem::path(configured);
    }
  }

  if (auto exePath = currentExecutablePath(paths.executablePath)) {
    auto siblingCore = exePath->parent_path() / "core";
    if (std::filesystem::exists(siblingCore) &&
        std::filesystem::is_directory(siblingCore)) {
      return siblingCore;
    }
  }

  return paths.configuredCoreDir;
}

std::filesystem::path stdlibObjectPath(const RuntimePaths &paths) {
  if (const char *configured = std::getenv("ZAPC_STDLIB_PATH")) {
    if (*configured != '\0') {
      return std::filesystem::path(configured);
    }
  }

  if (auto exePath = currentExecutablePath(paths.executablePath)) {
    auto siblingObject = exePath->parent_path() / "stdlib.o";
    if (std::filesystem::exists(siblingObject) &&
        std::filesystem::is_regular_file(siblingObject)) {
      return siblingObject;
    }
  }

  return paths.configuredStdlibObject;
}

std::string stripSourceExtension(const std::filesystem::path &path) {
  auto normalized = path.generic_string();
  if (path.extension() == ".zp" && normalized.size() >= 3) {
    normalized.resize(normalized.size() - 3);
  }
  return normalized;
}

std::string computeLogicalModulePath(const std::filesystem::path &canonicalPath,
                                     const RuntimePaths &paths) {
  auto stdRoot = std::filesystem::weakly_canonical(stdlibRootPath(paths));
  auto coreRoot = std::filesystem::weakly_canonical(coreRootPath(paths));
  auto cwdRoot =
      std::filesystem::weakly_canonical(std::filesystem::current_path());

  auto buildRelative = [&](const std::filesystem::path &root,
                           const std::string &prefix =
                               "") -> std::optional<std::string> {
    auto rootText = root.generic_string();
    auto pathText = canonicalPath.generic_string();
    if (pathText == rootText || pathText.rfind(rootText + "/", 0) != 0) {
      return std::nullopt;
    }

    auto rel = std::filesystem::relative(canonicalPath, root);
    auto stripped = stripSourceExtension(rel);
    if (prefix.empty()) {
      return stripped;
    }
    return prefix + "/" + stripped;
  };

  if (auto logical = buildRelative(coreRoot, "core")) {
    if (*logical == "core/core") {
      return "core";
    }
    return *logical;
  }
  if (auto logical = buildRelative(stdRoot, "std")) {
    return *logical;
  }
  if (auto logical = buildRelative(cwdRoot)) {
    return *logical;
  }
  return stripSourceExtension(canonicalPath.filename());
}

bool hasImplicitImport(const RootNode &root, std::string_view path) {
  for (const auto &child : root.children) {
    auto importNode = dynamic_cast<ImportNode *>(child.get());
    if (importNode && importNode->path == path) {
      return true;
    }
  }
  return false;
}

void injectImplicitPreludeImportIfNeeded(sema::ModuleInfo &module,
                                         bool includePrelude) {
  if (!includePrelude) {
    return;
  }
  if (module.linkPath == "core" || module.linkPath.rfind("core/", 0) == 0 ||
      module.linkPath.rfind("std/", 0) == 0) {
    return;
  }
  if (!module.root || hasImplicitImport(*module.root, "std/prelude")) {
    return;
  }

  auto import = std::make_unique<ImportNode>("std/prelude");
  module.root->children.insert(module.root->children.begin(),
                               std::move(import));
}

static void setImportError(std::string *errorMessage,
                           const std::filesystem::path &path) {
  if (!errorMessage) {
    return;
  }
  std::ostringstream out;
  out << "import path not found: " << path;
  *errorMessage = out.str();
}

bool resolveImportTargets(const std::filesystem::path &modulePath,
                          const ImportNode &importNode,
                          std::vector<std::filesystem::path> &targets,
                          const ImportMap &importMap, const RuntimePaths &paths,
                          std::string *errorMessage) {
  std::filesystem::path resolvedPath;
  const std::string &importPath = importNode.path;

  bool resolvedViaMap = false;
  for (const auto &[alias, target] : importMap) {
    if (importPath.rfind(alias, 0) == 0) {
      std::string rest = importPath.substr(alias.size());
      if (!rest.empty() && rest[0] == '/') {
        rest = rest.substr(1);
      }
      resolvedPath = std::filesystem::path(target) / rest;
      resolvedViaMap = true;
      break;
    }
  }

  if (!resolvedViaMap) {
    if (importPath == "core") {
      resolvedPath = coreRootPath(paths) / "core";
    } else if (importPath.rfind("core/", 0) == 0) {
      resolvedPath =
          coreRootPath(paths) / importPath.substr(std::string("core/").size());
    } else if (importPath.rfind("std/", 0) == 0) {
      resolvedPath =
          stdlibRootPath(paths) / importPath.substr(std::string("std/").size());
    } else {
      resolvedPath = modulePath.parent_path() / importPath;
    }
  }
  resolvedPath = resolvedPath.lexically_normal();

  auto tryFile = [&](const std::filesystem::path &path) -> bool {
    if (std::filesystem::exists(path) &&
        std::filesystem::is_regular_file(path)) {
      targets.push_back(std::filesystem::weakly_canonical(path));
      return true;
    }
    return false;
  };

  if (tryFile(resolvedPath)) {
    return true;
  }
  if (resolvedPath.extension() != ".zp" &&
      tryFile(resolvedPath.string() + ".zp")) {
    return true;
  }

  if (std::filesystem::exists(resolvedPath) &&
      std::filesystem::is_directory(resolvedPath)) {
    for (const auto &entry :
         std::filesystem::directory_iterator(resolvedPath)) {
      if (entry.is_regular_file() && entry.path().extension() == ".zp") {
        targets.push_back(std::filesystem::weakly_canonical(entry.path()));
      }
    }
    std::sort(targets.begin(), targets.end());
    return !targets.empty();
  }

  setImportError(errorMessage, resolvedPath);
  return false;
}

sema::ResolvedImport
makeResolvedImport(const ImportNode &importNode,
                   const std::vector<std::filesystem::path> &targets) {
  sema::ResolvedImport resolved;
  resolved.rawPath = importNode.path;
  resolved.moduleAlias = importNode.moduleAlias;
  resolved.visibility = importNode.visibility_;
  resolved.span = importNode.span;
  for (const auto &binding : importNode.bindings) {
    resolved.bindings.push_back({binding.sourceName, binding.localName});
  }
  for (const auto &target : targets) {
    resolved.targetModuleIds.push_back(target.string());
  }
  return resolved;
}

} // namespace zap::frontend
