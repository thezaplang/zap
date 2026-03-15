#include "driver/driver.hpp"
#include "codegen/llvm_codegen.hpp"
#include "driver/compiler.hpp"
#include "ir/ir_generator.hpp"
#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#include "sema/binder.hpp"
#include "sema/bound_nodes.hpp"
#include "utils/diagnostics.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string_view>

namespace zap {

driver::driver() {}

void print_red(std::string_view msg) {
  // Simple ANSI colors
  std::cerr << "\033[1;31m" << msg << "\033[0m";
}

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
      std::cout << "Zap Compiler [options] <file>\n"
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
      std::cout << "Zap Compiler v" << zap::ZAP_VERSION << '\n';
      return false;
    } else if (arg == "-o") {
      if (i + 1 < args.size()) {
        output_str = args[++i];
        implicit_output = false;
      } else {
        print_red("error: ");
        std::cerr << "argument to '-o' is missing\n";
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
      print_red("error: ");
      std::cerr << "unknown argument: " << arg << "\n";
      return false;
    } else {
      inputs.emplace_back(std::string(arg));
    }
  }

  if (emit_llvm && emit_zir) {
    print_red("error: ");
    std::cerr << "choosing multiple emit modes isn't allowed\n";
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

  std::cerr << "zapc: ";
  print_red("error: ");
  std::cerr << "no input files\n";
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
      std::cerr << "unknown input type: " << input << '\n';
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
    std::cerr << "cannot specify -o with multiple input files\n";
    return true;
  }

  if (per_file_emit && !objects.empty()) {
    print_red("error: ");
    std::cerr << "cannot use object files or archives with the selected "
                    "output mode\n";
    return true;
  }

  if (!format_supported()) {
    print_red("error: ");
    std::cerr
        << "chosen file output mode is not yet supported in this version\n";
    return true;
  }

  return false;
}

bool verifyFile(const std::filesystem::path &input) {
  if (!std::filesystem::exists(input)) {
    print_red("error: ");
    std::cerr << "provided file doesn't exist: " << input << '\n';
    return true;
  } else if (!std::filesystem::is_regular_file(input)) {
    print_red("error: ");
    std::cerr << "provided file isn't a regular file: " << input << '\n';
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
    print_red("error: ");
    std::cerr << "failed to generate ZIR\n";
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
    std::cerr << source_name << ": " << "failed parsing the provided file\n";
    return true;
  }

  sema::Binder binder(diagnostics);
  auto boundAst = binder.bind(*ast);

  if (!boundAst) {
    print_red("error: ");
    std::cerr << source_name << ": " << "semantic analysis failed\n";
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
      print_red("error: ");
      std::cerr << "object file emission failed\n";
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
      print_red("error: ");
      std::cerr << "couldn't open the provided file: " << out_path << '\n';
      std::cerr << "reason: " << strerror(errno) << '\n';
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
    std::ifstream file(input, std::ios::binary | std::ios::ate);
    if (!file) {
      print_red("error: ");
      std::cerr << "couldn't open the provided file: " << input << '\n';
      std::cerr << "reason: " << strerror(errno) << '\n';
      return true;
    }

    auto size = file.tellg();
    std::string content(size, '\0');

    if (size == 0) {
      std::cerr << "warning: provided file is empty: " << input << '\n';
    } else {
      file.seekg(0);
      file.read(content.data(), size);
    }

    file.close();

    if (compileSourceFile(content, input.string()))
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
    print_red("error: ");
    std::cerr << "linking failed with exit code: " << res << '\n';
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
      std::cerr << "warning: failed to remove: " << f
                   << "\nreason: " << ec.message() << '\n';
    }
  }

  return errs;
}

} // namespace zap
