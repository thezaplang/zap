#include "driver/driver.hpp"
#include "codegen/llvm_codegen.hpp"
#include "driver/compiler.hpp"
#include "ir/ir_generator.hpp"
#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#include "sema/binder.hpp"
#include "sema/bound_nodes.hpp"
#include "sema/module_info.hpp"
#include "utils/diagnostics.hpp"
#include "utils/stream.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <unistd.h>

namespace zap {
bool compileSourceZIR(sema::BoundRootNode &node, std::ostream &ofoutput);
bool compileSourceLLVMFromZIR(sema::BoundRootNode &node,
                              std::ostream &ofoutput);
std::unique_ptr<zir::Module> generateZIRModule(sema::BoundRootNode &node);
bool compileObjectFromZIR(sema::BoundRootNode &node,
                          const std::string &output_path,
                          int optimization_level);
namespace {

bool emitRequestedTextOutputs(driver &drv, sema::BoundRootNode &node,
                              const std::filesystem::path &base_output_path) {
  bool direct_output = !drv.is_implicit_output() &&
                       (drv.emits_llvm_text() != drv.emits_zir());

  if (drv.emits_zir()) {
    auto zir_path = direct_output
                        ? base_output_path
                        : std::filesystem::path(base_output_path).replace_extension(
                              driver::format_fileextension(
                                  driver::output_type::ZIR));
    std::ofstream zir_output(zir_path, std::ios::binary);
    if (!zir_output) {
      driver::reportError("couldn't open the provided file: ", zir_path,
                          "\nreason: ", strerror(errno));
      return true;
    }
    if (compileSourceZIR(node, zir_output)) {
      return true;
    }
  }

  if (drv.emits_llvm_text()) {
    auto llvm_path =
        direct_output
            ? base_output_path
            : std::filesystem::path(base_output_path).replace_extension(
                  driver::format_fileextension(driver::output_type::TEXT_LLVM));
    std::ofstream llvm_output(llvm_path, std::ios::binary);
    if (!llvm_output) {
      driver::reportError("couldn't open the provided file: ", llvm_path,
                          "\nreason: ", strerror(errno));
      return true;
    }
    if (compileSourceLLVMFromZIR(node, llvm_output)) {
      return true;
    }
  }

  return false;
}

std::filesystem::path g_executable_path;

std::optional<std::filesystem::path> currentExecutablePath(
    const std::filesystem::path &argv0Hint) {
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

std::filesystem::path stdlibRootPath(const std::filesystem::path &argv0Hint) {
  if (const char *configured = std::getenv("ZAPC_STDLIB_DIR")) {
    if (*configured != '\0') {
      return std::filesystem::path(configured);
    }
  }

  if (auto exePath = currentExecutablePath(argv0Hint)) {
    auto siblingStd = exePath->parent_path() / "std";
    if (std::filesystem::exists(siblingStd) &&
        std::filesystem::is_directory(siblingStd)) {
      return siblingStd;
    }
  }

  return std::filesystem::path(ZAPC_STDLIB_DIR);
}

std::filesystem::path stdlibObjectPath(const std::filesystem::path &argv0Hint) {
  if (const char *configured = std::getenv("ZAPC_STDLIB_PATH")) {
    if (*configured != '\0') {
      return std::filesystem::path(configured);
    }
  }

  if (auto exePath = currentExecutablePath(argv0Hint)) {
    auto siblingObject = exePath->parent_path() / "stdlib.o";
    if (std::filesystem::exists(siblingObject) &&
        std::filesystem::is_regular_file(siblingObject)) {
      return siblingObject;
    }
  }

  return std::filesystem::path(ZAPC_STDLIB_PATH);
}

std::string stripSourceExtension(const std::filesystem::path &path) {
  auto normalized = path.generic_string();
  if (path.extension() == ".zp" && normalized.size() >= 4) {
    normalized.resize(normalized.size() - 4);
  }
  return normalized;
}

std::string computeLogicalModulePath(const std::filesystem::path &canonicalPath) {
  auto stdRoot = std::filesystem::weakly_canonical(stdlibRootPath(g_executable_path));
  auto cwdRoot = std::filesystem::weakly_canonical(std::filesystem::current_path());

  auto buildRelative = [&](const std::filesystem::path &root,
                           const std::string &prefix = "")
      -> std::optional<std::string> {
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

  if (auto logical = buildRelative(stdRoot, "std")) {
    return *logical;
  }
  if (auto logical = buildRelative(cwdRoot)) {
    return *logical;
  }
  return stripSourceExtension(canonicalPath.filename());
}

bool readSourceFile(const std::filesystem::path &path, std::string &content) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    driver::reportError("couldn't open the provided file: ", path,
                        "\nreason: ", strerror(errno));
    return true;
  }

  auto size = file.tellg();
  content.assign(static_cast<size_t>(std::max<std::streamsize>(size, 0)), '\0');

  if (size == 0) {
    err() << "warning: provided file is empty: " << path << '\n';
  } else {
    file.seekg(0);
    file.read(content.data(), size);
  }

  return false;
}

bool resolveImportTargets(const std::filesystem::path &modulePath,
                          const ImportNode &importNode,
                          std::vector<std::filesystem::path> &targets) {
  std::filesystem::path resolvedPath;
  if (importNode.path.rfind("std/", 0) == 0) {
    resolvedPath = stdlibRootPath(g_executable_path) /
                   importNode.path.substr(std::string("std/").size());
  } else {
    resolvedPath = modulePath.parent_path() / importNode.path;
  }
  resolvedPath = resolvedPath.lexically_normal();

  auto tryFile = [&](const std::filesystem::path &path) -> bool {
    if (std::filesystem::exists(path) && std::filesystem::is_regular_file(path)) {
      targets.push_back(std::filesystem::weakly_canonical(path));
      return true;
    }
    return false;
  };

  if (tryFile(resolvedPath)) {
    return false;
  }

  if (resolvedPath.extension() != ".zp" && tryFile(resolvedPath.string() + ".zp")) {
    return false;
  }

  if (std::filesystem::exists(resolvedPath) &&
      std::filesystem::is_directory(resolvedPath)) {
    for (const auto &entry : std::filesystem::directory_iterator(resolvedPath)) {
      if (!entry.is_regular_file()) {
        continue;
      }
      if (entry.path().extension() == ".zp") {
        targets.push_back(std::filesystem::weakly_canonical(entry.path()));
      }
    }
    std::sort(targets.begin(), targets.end());
    return false;
  }

  driver::reportError("import path not found: ", resolvedPath);
  return true;
}

bool loadModuleGraph(
    const std::filesystem::path &entryPath,
    std::map<std::string, std::unique_ptr<sema::ModuleInfo>> &modules,
    std::set<std::string> &visiting) {
  auto canonicalPath = std::filesystem::weakly_canonical(entryPath);
  auto moduleId = canonicalPath.string();

  if (modules.find(moduleId) != modules.end()) {
    return false;
  }
  if (visiting.count(moduleId)) {
    driver::reportError("cyclic import detected involving ", canonicalPath);
    return true;
  }

  visiting.insert(moduleId);

  std::string source;
  if (readSourceFile(canonicalPath, source)) {
    return true;
  }

  DiagnosticEngine diagnostics(source, canonicalPath.string());
  Lexer lex(diagnostics);
  auto tokens = lex.tokenize(source);
  Parser parser(tokens, diagnostics);
  auto ast = parser.parse();

  if (diagnostics.hadErrors() || !ast) {
    diagnostics.printText(err());
    return true;
  }

  auto module = std::make_unique<sema::ModuleInfo>();
  module->moduleId = moduleId;
  module->moduleName = canonicalPath.stem().string();
  module->linkPath = computeLogicalModulePath(canonicalPath);
  module->sourceName = canonicalPath.string();
  module->root = std::move(ast);

  for (const auto &child : module->root->children) {
    auto importNode = dynamic_cast<ImportNode *>(child.get());
    if (!importNode) {
      continue;
    }

    sema::ResolvedImport resolved;
    resolved.rawPath = importNode->path;
    resolved.moduleAlias = importNode->moduleAlias;
    resolved.visibility = importNode->visibility_;
    resolved.span = importNode->span;
    for (const auto &binding : importNode->bindings) {
      resolved.bindings.push_back({binding.sourceName, binding.localName});
    }

    std::vector<std::filesystem::path> importTargets;
    if (resolveImportTargets(canonicalPath, *importNode, importTargets)) {
      return true;
    }

    for (const auto &target : importTargets) {
      resolved.targetModuleIds.push_back(target.string());
    }

    module->imports.push_back(std::move(resolved));
  }

  for (const auto &import : module->imports) {
    for (const auto &targetId : import.targetModuleIds) {
      if (loadModuleGraph(targetId, modules, visiting)) {
        return true;
      }
    }
  }

  visiting.erase(moduleId);
  modules[moduleId] = std::move(module);
  return false;
}

} // namespace

driver::driver() = default;

void driver::setExecutablePath(std::filesystem::path path) {
  executable_path = std::move(path);
  g_executable_path = executable_path;
}

bool compileLoadedModules(driver &drv, const std::filesystem::path &entryPath) {
  std::map<std::string, std::unique_ptr<sema::ModuleInfo>> moduleMap;
  std::set<std::string> visiting;
  if (loadModuleGraph(entryPath, moduleMap, visiting)) {
    return true;
  }

  auto entryId = std::filesystem::weakly_canonical(entryPath).string();
  if (moduleMap.find(entryId) == moduleMap.end()) {
    driver::reportError("failed to load entry module: ", entryPath);
    return true;
  }
  moduleMap[entryId]->isEntry = true;

  std::string entrySource;
  if (readSourceFile(entryPath, entrySource)) {
    return true;
  }
  DiagnosticEngine diagnostics(entrySource, entryPath.string());

  std::vector<sema::ModuleInfo> modules;
  modules.reserve(moduleMap.size());
  for (auto &[_, module] : moduleMap) {
    modules.push_back(std::move(*module));
  }

  sema::Binder binder(diagnostics, drv.allow_unsafe);
  auto boundAst = binder.bind(modules);
  diagnostics.printText(err());

  if (!boundAst) {
    driver::reportError(entryPath, ": semantic analysis failed");
    return true;
  }

  if (drv.binary_output()) {
    std::filesystem::path out_path;

    if (drv.get_output_type() == driver::output_type::EXEC) {
      out_path = entryPath.string() + ".o";
      drv.cleanups.emplace_back(out_path);
    } else if (drv.get_output_type() == driver::output_type::OBJECT) {
      if (drv.is_implicit_output()) {
        out_path = entryPath.string() + ".o";
      } else {
        out_path = drv.get_output();
      }
    }

    if (compileObjectFromZIR(*boundAst, out_path.string(),
                             drv.optimization_level)) {
      return true;
    }

    drv.objects.emplace_back(std::move(out_path));
  } else if (drv.emits_text_output()) {
    std::filesystem::path out_path =
        drv.is_implicit_output() ? entryPath : drv.get_output();
    if (emitRequestedTextOutputs(drv, *boundAst, out_path)) {
      return true;
    }
  } else {
    return true;
  }

  return false;
}

bool driver::parseArgs(int argc, char **argv) {
  std::vector<std::string_view> args;
  for (int i = 1; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }
  linker_args.clear();

  bool emit_llvm = false;
  bool emit_zir = false;
  bool emit_s = false;
  bool nolink = false;
  std::string_view output_str = "a.out";
  implicit_output = true;
  inc_stdlib = true;
  allow_unsafe = false;
  emit_llvm_text = false;
  emit_zir_text = false;
  optimization_level = 0;

  for (size_t i = 0; i < args.size(); ++i) {
    auto arg = args[i];

    if (arg == "--help") {
      out()
          << "Zap Compiler [options] <file>\n"
          << "Zap Compiler\n\n"
          << "Options:\n"
          << "  --help          Display available options\n"
          << "  --version       Print version information\n"
          << "  -o <file>       Write output to <file>\n"
          << "  -nostdlib       Stops the linker from linking the zap stdlib\n"
          << "  -c              Compile and assemble but not link\n"
          << "  -S              Compile only no assembling or linking\n"
          << "  -emit-llvm      Emit LLVM IR instead of final output\n"
          << "  -emit-zir       Emit ZIR instead of final output\n"
          << "  --allow-unsafe  Enable unsafe blocks, unsafe functions, and raw pointers\n"
          << "  -l<name>        Link with library <name> (forwarded to system linker)\n"
          << "  -L<dir>         Add <dir> to library search path (forwarded to linker)\n"
          << "  -l <name>       Same as -l<name>\n"
          << "  -L <dir>        Same as -L<dir>\n"
          << "  -O, -O0..-O3    Set optimization level (default: -O0)\n"
          << "  -O00..-O03      Alias for -O0..-O3\n";
      return false;
    } else if (arg == "--version") {
      out() << "Zap Compiler v" << zap::ZAP_VERSION << '\n';
      return false;
    } else if (arg == "-o") {
      if (i + 1 < args.size()) {
        output_str = args[++i];
        implicit_output = false;
      } else {
        reportError("argument to '-o' is missing");
        return false;
      }
    } else if (arg.substr(0, 2) == "-o") {
      output_str = arg.substr(2);
      implicit_output = false;
    } else if (arg == "-nostdlib") {
      inc_stdlib = false;
    } else if (arg == "-c") {
      nolink = true;
    } else if (arg == "-S") {
      emit_s = true;
    } else if (arg == "-emit-llvm") {
      emit_llvm = true;
    } else if (arg == "-emit-zir") {
      emit_zir = true;
    } else if (arg == "--allow-unsafe") {
      allow_unsafe = true;
    } else if (arg == "-O") {
      optimization_level = 2;
    } else if (arg.size() == 3 && arg.substr(0, 2) == "-O" &&
               arg[2] >= '0' && arg[2] <= '3') {
      optimization_level = static_cast<int>(arg[2] - '0');
    } else if (arg.size() == 4 && arg.substr(0, 2) == "-O" &&
               arg[2] == '0' && arg[3] >= '0' && arg[3] <= '3') {
      optimization_level = static_cast<int>(arg[3] - '0');
    } else if (arg.substr(0, 2) == "-O") {
      reportError("invalid optimization level: ", arg,
                  " (expected -O, -O0..-O3, or -O00..-O03)");
      return false;
    } else if (arg == "-l" || arg == "-L") {
      if (i + 1 < args.size()) {
        linker_args.emplace_back(std::string(arg));
        linker_args.emplace_back(std::string(args[++i]));
      } else {
        reportError("argument to '", arg, "' is missing");
        return false;
      }
    } else if (arg.size() > 2 && arg.substr(0, 2) == "-l") {
      linker_args.emplace_back(std::string(arg));
    } else if (arg.size() > 2 && arg.substr(0, 2) == "-L") {
      linker_args.emplace_back(std::string(arg));
    } else if (arg.substr(0, 1) == "-") {
      reportError("unknown argument: ", arg);
      return false;
    } else {
      inputs.emplace_back(std::string(arg));
    }
  }

  if (emit_s) {
    if (!emit_llvm && !emit_zir)
      out_type = output_type::ASM;
  }

  emit_llvm_text = emit_llvm;
  emit_zir_text = emit_zir;

  if (out_type == output_type::EXEC) {
    if (nolink) {
      out_type = output_type::OBJECT;
    }
  }

  if ((emit_llvm_text || emit_zir_text) && !implicit_output &&
      (emit_llvm_text == emit_zir_text)) {
    reportError("cannot use -o when emitting multiple text outputs");
    return false;
  }

  output = std::filesystem::path(output_str);

  if (!inputs.empty()) {
    return true;
  }

  reportError("no input files");
  return false;
}

bool driver::splitInputs() {
  for (const std::string &input : get_inputs()) {
    std::filesystem::path input_path = input;
    std::string ext = input_path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == ".zp") {
      sources.emplace_back(std::move(input_path));
    } else if (ext == ".a" || ext == ".o") {
      objects.emplace_back(std::move(input_path));
    } else {
      reportError("unknown input type: ", input);
      return true;
    }
  }

  return false;
}

bool driver::verifyOutput() {
  const zap::driver::output_type &emit_type = get_output_type();

  bool per_file_emit = emits_text_output() || (emit_type != output_type::EXEC);

  if (per_file_emit && get_inputs().size() > 1 && !is_implicit_output()) {
    reportError("cannot specify -o with multiple input files");
    return true;
  }

  if (per_file_emit && !objects.empty()) {
    reportError(
        "cannot use object files or archives with the selected output mode");
    return true;
  }

  if (!format_supported()) {
    reportError("chosen file output mode is not yet supported in this version");
    return true;
  }

  return false;
}

bool verifyFile(const std::filesystem::path &input) {
  if (!std::filesystem::exists(input)) {
    driver::reportError("provided file doesn't exist: ", input);
    return true;
  } else if (!std::filesystem::is_regular_file(input)) {
    driver::reportError("provided file isn't a regular file: ", input);
    return true;
  }
  return false;
}

bool driver::verifySources() {
  for (const std::filesystem::path &input : sources) {
    if (verifyFile(input))
      return true;
  }
  for (const std::filesystem::path &input : objects) {
    if (verifyFile(input))
      return true;
  }
  return false;
}

std::unique_ptr<zir::Module> generateZIRModule(sema::BoundRootNode &node) {
  zir::BoundIRGenerator irGen;
  return irGen.generate(node);
}

bool compileSourceZIR(sema::BoundRootNode &node, std::ostream &ofoutput) {
  auto mod = generateZIRModule(node);
  if (mod) {
    ofoutput << mod->toString();
  } else {
    driver::reportError("failed to generate ZIR");
    return true;
  }
  return false;
}

bool compileSourceLLVMFromZIR(sema::BoundRootNode &node,
                              std::ostream &ofoutput) {
  auto mod = generateZIRModule(node);
  if (!mod) {
    driver::reportError("failed to generate ZIR");
    return true;
  }

  codegen::LLVMCodeGen llvmGen;
  llvmGen.generate(*mod);
  std::string ir;
  llvm::raw_string_ostream rs(ir);
  llvmGen.printIR(rs);
  ofoutput << ir;
  return false;
}

bool compileObjectFromZIR(sema::BoundRootNode &node,
                          const std::string &output_path,
                          int optimization_level) {
  auto mod = generateZIRModule(node);
  if (!mod) {
    driver::reportError("failed to generate ZIR");
    return true;
  }

  codegen::LLVMCodeGen llvmGen;
  llvmGen.generate(*mod);
  if (!llvmGen.emitObjectFile(output_path, optimization_level)) {
    driver::reportError("object file emission failed");
    return true;
  }
  return false;
}

bool driver::compileSourceFile(const std::string &source,
                               const std::string &source_name) {
  zap::DiagnosticEngine diagnostics(source, source_name);
  Lexer lex(diagnostics);

  auto tokens = lex.tokenize(source);

  zap::Parser parser(tokens, diagnostics);
  auto ast = parser.parse();

  if (diagnostics.hadErrors()) {
    diagnostics.printText(err());
    return true;
  }

  if (!ast) {
    diagnostics.printText(err());
    reportError(source_name, ": failed parsing the provided file");
    return true;
  }

  sema::Binder binder(diagnostics, allow_unsafe);
  auto boundAst = binder.bind(*ast);
  diagnostics.printText(err());

  if (!boundAst) {
    reportError(source_name, ": semantic analysis failed");
    return true;
  }

  if (binary_output()) {
    std::filesystem::path out_path;

    if (out_type == output_type::EXEC) {
      out_path = source_name + ".o";
      cleanups.emplace_back(out_path);
    } else if (out_type == output_type::OBJECT) {
      if (implicit_output) {
        out_path = source_name + ".o";
      } else {
        out_path = output;
      }
    }

    if (compileObjectFromZIR(*boundAst, out_path.string(),
                             optimization_level)) {
      return true;
    }

    objects.emplace_back(std::move(out_path));
  } else if (emits_text_output()) {
    std::filesystem::path out_path =
        implicit_output ? std::filesystem::path(source_name) : output;
    if (emitRequestedTextOutputs(*this, *boundAst, out_path)) {
      return true;
    }
  } else if (out_type == output_type::ASM) {
      return true; // TODO: Implement assembly emission.
  } else {
    return true;
  }

  return false;
}

bool driver::compile() {
  for (const std::filesystem::path &input : sources) {
    if (compileLoadedModules(*this, input))
      return true;
  }

  return false;
}

bool driver::link() {
  if (!needs_linking())
    return false;

  std::string cmd = "/usr/bin/cc ";

  if (inc_stdlib) {
    cmd += stdlibObjectPath(executable_path).string() + " ";
  } else {
    cmd += "-nostdlib ";
  }

  for (const auto &obj : objects) {
    cmd += obj.string() + " ";
  }

  for (const auto &arg : linker_args) {
    cmd += arg + " ";
  }

  cmd += "-lm ";
  cmd += "-o " + output.string();

  int res = std::system(cmd.c_str());
  if (res != 0) {
    reportError("linking failed with exit code: ", res);
    return true;
  }

  return false;
}

bool driver::cleanup() {
  bool errs = false;

  for (const std::filesystem::path &f : cleanups) {
    std::error_code ec;
    std::filesystem::remove(f, ec);
    if (ec) {
      errs = true;
      reportWarning("failed to remove: ", f, "\nreason: ", ec.message());
    }
  }

  return errs;
}

} // namespace zap
