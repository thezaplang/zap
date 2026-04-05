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

namespace zap {
bool compileSourceZIR(sema::BoundRootNode &node, std::ostream &ofoutput);
namespace {

std::string stripZapExtension(const std::filesystem::path &path) {
  auto normalized = path.generic_string();
  if (path.extension() == ".zap" && normalized.size() >= 4) {
    normalized.resize(normalized.size() - 4);
  }
  return normalized;
}

std::string computeLogicalModulePath(const std::filesystem::path &canonicalPath) {
  auto stdRoot =
      std::filesystem::weakly_canonical(std::filesystem::path(ZAPC_STDLIB_DIR));
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
    auto stripped = stripZapExtension(rel);
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
  return stripZapExtension(canonicalPath.filename());
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
    resolvedPath = std::filesystem::path(ZAPC_STDLIB_DIR) /
                   importNode.path.substr(std::string("std/").size());
  } else {
    resolvedPath = modulePath.parent_path() / importNode.path;
  }

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

  if (resolvedPath.extension() != ".zap" && tryFile(resolvedPath.string() + ".zap")) {
    return false;
  }

  if (std::filesystem::exists(resolvedPath) &&
      std::filesystem::is_directory(resolvedPath)) {
    for (const auto &entry : std::filesystem::directory_iterator(resolvedPath)) {
      if (!entry.is_regular_file()) {
        continue;
      }
      if (entry.path().extension() == ".zap") {
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

  sema::Binder binder(diagnostics);
  auto boundAst = binder.bind(modules);

  if (!boundAst) {
    driver::reportError(entryPath, ": semantic analysis failed");
    return true;
  }

  if (drv.binary_output()) {
    if (drv.get_output_type() == driver::output_type::LLVM)
      return true;

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

    codegen::LLVMCodeGen llvmGen;
    llvmGen.generate(*boundAst);

    if (!llvmGen.emitObjectFile(out_path.string())) {
      driver::reportError("object file emission failed");
      return true;
    }

    drv.objects.emplace_back(std::move(out_path));
  } else {
    std::filesystem::path out_path =
        drv.is_implicit_output()
            ? std::filesystem::path(entryPath.string() +
                                    driver::format_fileextension(drv.get_output_type()))
            : drv.get_output();

    std::ofstream ofoutput(out_path, std::ios::binary);

    if (!ofoutput) {
      driver::reportError("couldn't open the provided file: ", out_path,
                          "\nreason: ", strerror(errno));
      return true;
    }

    if (drv.get_output_type() == driver::output_type::ZIR) {
      if (compileSourceZIR(*boundAst, ofoutput))
        return true;
    } else if (drv.get_output_type() == driver::output_type::TEXT_LLVM) {
      codegen::LLVMCodeGen llvmGen;
      llvmGen.generate(*boundAst);
      std::string ir;
      llvm::raw_string_ostream rs(ir);
      llvmGen.printIR(rs);
      ofoutput << ir;
    } else {
      return true;
    }
  }

  return false;
}

driver::driver() {}

bool driver::parseArgs(int argc, char **argv) {
  std::vector<std::string_view> args;
  for (int i = 1; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }

  bool emit_llvm = false;
  bool emit_zir = false;
  bool emit_s = false;
  bool nolink = false;
  std::string_view output_str = "a.out";
  implicit_output = true;
  inc_stdlib = true;

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
          << "  -emit-zir       Emit ZIR instead of final output\n";
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
    } else if (arg.substr(0, 1) == "-") {
      reportError("unknown argument: ", arg);
      return false;
    } else {
      inputs.emplace_back(std::string(arg));
    }
  }

  if (emit_llvm && emit_zir) {
    reportError("choosing multiple emit modes isn't allowed");
    return false;
  }

  if (emit_s) {
    if (emit_llvm)
      out_type = output_type::TEXT_LLVM;
    else
      out_type = output_type::ASM;
  } else if (emit_llvm)
    out_type = output_type::LLVM;

  if (emit_zir)
    out_type = output_type::ZIR;

  if (out_type == output_type::EXEC) {
    if (nolink) {
      out_type = output_type::OBJECT;
    }
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

    if (ext == ".zap") {
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

  bool per_file_emit = (emit_type != output_type::EXEC);

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

bool compileSourceZIR(sema::BoundRootNode &node, std::ostream &ofoutput) {
  zir::BoundIRGenerator irGen;
  auto mod = irGen.generate(node);
  if (mod) {
    ofoutput << mod->toString();
  } else {
    driver::reportError("failed to generate ZIR");
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
    return true;
  }

  if (!ast) {
    reportError(source_name, ": failed parsing the provided file");
    return true;
  }

  sema::Binder binder(diagnostics);
  auto boundAst = binder.bind(*ast);

  if (!boundAst) {
    reportError(source_name, ": semantic analysis failed");
    return true;
  }

  if (binary_output()) {
    if (out_type == output_type::LLVM)
      return true; // TODO: Implement LLVM bitcode emission.

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

    codegen::LLVMCodeGen llvmGen;
    llvmGen.generate(*boundAst);

    if (!llvmGen.emitObjectFile(out_path.string())) {
      reportError("object file emission failed");
      return true;
    }

    objects.emplace_back(std::move(out_path));
  } else {
    std::filesystem::path out_path =
        implicit_output ? std::filesystem::path(source_name +
                                                format_fileextension(out_type))
                        : output;

    std::ofstream ofoutput(out_path, std::ios::binary);

    if (!ofoutput) {
      reportError("couldn't open the provided file: ", out_path,
                  "\nreason: ", strerror(errno));
      return true;
    }

    if (out_type == output_type::ZIR) {
      if (compileSourceZIR(*boundAst, ofoutput))
        return true;
    } else if (out_type == output_type::TEXT_LLVM) {
      codegen::LLVMCodeGen llvmGen;
      llvmGen.generate(*boundAst);
      // TODO: Avoid using LLVM types like raw_string_ostream here.
      std::string ir;
      llvm::raw_string_ostream rs(ir);
      llvmGen.printIR(rs);
      ofoutput << ir;
    } else if (out_type == output_type::ASM) {
      return true; // TODO: Implement assembly emission.
    } else {
      return true;
    }
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
    cmd += std::string(ZAPC_STDLIB_PATH) + " ";
  } else {
    cmd += "-nostdlib ";
  }

  for (const auto &obj : objects) {
    cmd += obj.string() + " ";
  }

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
