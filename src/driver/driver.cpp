#include "driver/driver.hpp"
#include "codegen/llvm_codegen.hpp"
#include "driver/compiler.hpp"
#include "ir/ir_generator.hpp"
#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#include "sema/binder.hpp"
#include "sema/bound_nodes.hpp"
#include "utils/diagnostics.hpp"
#include <cerrno>
#include <cstring>
#include <fstream>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/raw_ostream.h>
#include <system_error>

namespace zap {

namespace opts {
using namespace llvm::opt;

#define OPTTABLE_STR_TABLE_CODE
#include "Options.inc"
#undef OPTTABLE_STR_TABLE_CODE

#define OPTTABLE_PREFIXES_TABLE_CODE
#include "Options.inc"
#undef OPTTABLE_PREFIXES_TABLE_CODE

static const OptTable::Info InfoTable[] = {
#define OPTION(...) LLVM_CONSTRUCT_OPT_INFO(__VA_ARGS__),
#include "Options.inc"
#undef OPTION
};

ZapcOptTable::ZapcOptTable()
    : GenericOptTable(OptionStrTable, OptionPrefixesTable, InfoTable) {}

} // namespace opts

driver::driver() {}

enum class ColorState : uint8_t { None, Available, Unavailable };

ColorState colors = ColorState::None;

/// This should be changed ASAP.
void print_red(llvm::StringRef msg) {
  if (colors == ColorState::None) {
    colors = llvm::errs().has_colors() ? ColorState::Available
                                       : ColorState::Unavailable;
  }

  switch (colors) {
  case ColorState::Available:
    llvm::errs().changeColor(llvm::raw_ostream::RED, true);
    llvm::errs() << msg;
    llvm::errs().resetColor();
    break;
  case ColorState::Unavailable:
    llvm::errs() << msg;
    break;
  default:
    break;
  }
}

bool driver::parseArgs(int argc, char **argv) {
  opts::ZapcOptTable table;
  unsigned missingIndex, missingCount;
  auto argsArr = llvm::ArrayRef<const char *>(argv, (size_t)argc).slice(1);

  auto args = table.ParseArgs(argsArr, missingIndex, missingCount);

  /// --help should be a priority above --version.
  if (args.hasArg(opts::OPT_help)) {
    table.printHelp(llvm::outs(), ZAP_NAME_MACRO " [options] <file>",
                    "Zap Compiler");
    return false;
  }

  /// --version should still have priority over regular compilation.
  if (args.hasArg(opts::OPT_version)) {
    llvm::outs() << "Zap Compiler v" << zap::ZAP_VERSION << '\n';
    return false;
  }

  if (missingCount) {
    print_red("error: ");
    llvm::errs() << "argument to '" << args.getArgString(missingIndex)
                 << "' is missing\n";
    return false;
  }

  /// Check the output type flags.
  bool emit_llvm = args.hasArg(opts::OPT_emitLLVM);
  bool emit_zir = args.hasArg(opts::OPT_emitZIR);
  bool emit_s = args.hasArg(opts::OPT_emitS);

  /// Cannot mix emit modes.
  if (emit_llvm && emit_zir) {
    print_red("error: ");
    llvm::errs() << "choosing multiple emit modes isn't allowed\n";
    return false;
  }

  /// If -S is present choose either textual LLVM IR or assembly.
  /// If -S is NOT present either choose LLVM BC or continue.
  if (emit_s) {
    if (emit_llvm)
      out_type = output_type::TEXT_LLVM;
    else
      out_type = output_type::ASM;
  } else if (emit_llvm)
    out_type = output_type::LLVM;

  if (emit_zir)
    out_type = output_type::ZIR;

  /// If we are on the default emit mode and -c is present, we should not link.
  if (out_type == output_type::EXEC) {
    if (args.hasArg(opts::OPT_nolink)) {
      out_type = output_type::OBJECT;
    }
  }

  inputs = args.getAllArgValues(opts::OPT_INPUT);

  auto output_str = args.getLastArgValue(opts::OPT_output, "a.out");
  output = std::string_view(output_str.data(), output_str.size());

  implicit_output = !args.hasArg(opts::OPT_output);

  inc_stdlib = !args.hasArg(opts::OPT_nostdlib);

  if (!inputs.empty()) {
    return true;
  }

  llvm::errs() << "zapc: ";
  print_red("error: ");
  llvm::errs() << "no input files\n";
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
      print_red("error: ");
      llvm::errs() << "unknown input type: " << input << '\n';
      return true;
    }
  }

  return false;
}

bool driver::verifyOutput() {
  const zap::driver::output_type &emit_type = get_output_type();

  bool per_file_emit = (emit_type != output_type::EXEC);

  if (per_file_emit && get_inputs().size() > 1 && !is_implicit_output()) {
    print_red("error: ");
    llvm::errs() << "cannot specify -o with multiple input files\n";
    return true;
  }

  if (per_file_emit && !objects.empty()) {
    print_red("error: ");
    llvm::errs() << "cannot use object files or archives with the selected "
                    "output mode\n";
    return true;
  }

  if (!format_supported()) {
    print_red("error: ");
    llvm::errs()
        << "chosen file output mode is not yet supported in this version\n";
    return true;
  }

  return false;
}
bool verifyFile(const std::filesystem::path &input) {
  if (!std::filesystem::exists(input)) {
    print_red("error: ");
    llvm::errs() << "provided file doesn't exist: " << input << '\n';
    return true;
  } else if (!std::filesystem::is_regular_file(input)) {
    print_red("error: ");
    llvm::errs() << "provided file isn't a regular file: " << input << '\n';
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

bool compileSourceZIR(sema::BoundRootNode &node, llvm::raw_ostream &ofoutput) {
  zir::BoundIRGenerator irGen;
  auto mod = irGen.generate(node);
  if (mod) {
    ofoutput << mod->toString();
  } else {
    print_red("error: ");
    llvm::errs() << "failed to generate ZIR\n";
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
    print_red("error: ");
    llvm::errs() << source_name << ": " << "failed parsing the provided file\n";
    return true;
  }

  sema::Binder binder(diagnostics);
  auto boundAst = binder.bind(*ast);

  if (!boundAst) {
    print_red("error: ");
    llvm::errs() << source_name << ": " << "semantic analysis failed\n";
    return true;
  }

  if (binary_output()) {
    if (out_type == output_type::LLVM)
      return true; // Not yet supported.

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

    if (!llvmGen.emitObjectFile(out_path)) {
      print_red("error: ");
      llvm::errs() << "object file emission failed\n";
      return true;
    }

    objects.emplace_back(std::move(out_path));
  } else {
    std::filesystem::path out_path =
        implicit_output ? std::filesystem::path(source_name +
                                                format_fileextension(out_type))
                        : output;

    std::error_code ec;
    llvm::raw_fd_ostream ofoutput(out_path.string(), ec,
                                  llvm::sys::fs::OF_None);

    if (ec) {
      print_red("error: ");
      llvm::errs() << "couldn't open the provided file: " << output << '\n';
      llvm::errs() << "reason: " << ec.message() << '\n';
      return true;
    }

    if (out_type == output_type::ZIR) {
      if (compileSourceZIR(*boundAst, ofoutput))
        return true;
    } else if (out_type == output_type::TEXT_LLVM) {
      codegen::LLVMCodeGen llvmGen;
      llvmGen.generate(*boundAst);
      llvmGen.printIR(ofoutput);
    } else if (out_type == output_type::ASM) {
      /// Not yet supported.
      return true;
    } else {
      return true;
    }
  }

  return false;
}

bool driver::compile() {
  for (const std::filesystem::path &input : sources) {
    std::ifstream file(input, std::ios::binary | std::ios::ate);
    if (!file) {
      print_red("error: ");
      llvm::errs() << "couldn't open the provided file: " << input << '\n';
      llvm::errs() << "reason: " << strerror(errno) << '\n';
      return true;
    }

    auto size = file.tellg();
    std::string content(size, '\0');

    if (size == 0) {
      llvm::errs() << "warning: provided file is empty: " << input << '\n';
    } else {
      file.seekg(0);
      file.read(content.data(), size);
    }

    file.close();

    if (compileSourceFile(content, input))
      return true;
  }

  return false;
}

bool driver::link() {
  if (!needs_linking())
    return false;

  std::vector<std::string> args;

  for (const auto &obj : objects) {
    args.emplace_back(obj);
  }

  args.push_back("-o");
  args.push_back(output);

  std::vector<llvm::StringRef> argsllvm;
  argsllvm.push_back("cc");

  if(inc_stdlib){
    argsllvm.push_back(ZAPC_STDLIB_PATH);
  }

  for (const auto &s : args) {
    argsllvm.push_back(s);
  }

  int res = llvm::sys::ExecuteAndWait("/usr/bin/cc", argsllvm);
  if (res != 0) {
    print_red("error: ");
    llvm::errs() << "linking failed with exit code: " << res << '\n';
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
      llvm::errs() << "warning: failed to remove: " << f
                   << "\nreason: " << ec.message() << '\n';
    }
  }

  return errs;
}

} // namespace zap