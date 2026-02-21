#include "codegen/llvm_codegen.hpp"
#include "ir/ir_generator.hpp"
#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#include "sema/binder.hpp"
#include "utils/diagnostics.hpp"
#include <cstring>
#include <fstream>
#include <iostream>
#include <llvm/Support/raw_ostream.h>
#include <string>

#define ZAP_VERSION "0.1.0"

void printHelp(const char *programName) {
  std::cout << "Zap Compiler v" << ZAP_VERSION << "\n\n";
  std::cout << "Usage: " << programName << " [options] <file.zap>\n\n";
  std::cout << "Options:\n";
  std::cout << "  -h, --help              Show this help message\n";
  std::cout << "  -v, --version           Show version information\n";
  std::cout << "  -o, --output <file>     Specify output file name\n";
  std::cout << "  --debug                 Enable debug output\n";
  std::cout
      << "  --zir                   Display Zap Intermediate Representation\n";
  std::cout << "  --llvm                  Display LLVM IR\n";
  std::cout << "\nExample:\n";
  std::cout << "  " << programName << " main.zap\n";
  std::cout << "  " << programName << " -o myprogram main.zap\n";
}

void printVersion() { std::cout << "Zap Compiler v" << ZAP_VERSION << "\n"; }

int main(int argc, char *argv[]) {
  std::string inputFile;
  std::string outputFile;
  bool debugMode = false;
  bool displayZIR = false;
  bool displayLLVM = false;

  if (argc < 2) {
    std::cerr << "Error: No input file specified\n";
    std::cerr << "Try '" << argv[0] << " --help' for more information\n";
    return 1;
  }

  for (int i = 1; i < argc; i++) {
    std::string arg(argv[i]);

    if (arg == "-h" || arg == "--help") {
      printHelp(argv[0]);
      return 0;
    } else if (arg == "-v" || arg == "--version") {
      printVersion();
      return 0;
    } else if (arg == "--debug") {
      debugMode = true;
    } else if (arg == "--zir") {
      displayZIR = true;
    } else if (arg == "--llvm") {
      displayLLVM = true;
    } else if (arg == "-o" || arg == "--output") {
      if (i + 1 >= argc) {
        std::cerr << "Error: -o/--output requires an argument\n";
        return 1;
      }
      outputFile = argv[++i];
    } else if (arg[0] == '-') {
      std::cerr << "Error: Unknown option '" << arg << "'\n";
      std::cerr << "Try '" << argv[0] << " --help' for more information\n";
      return 1;
    } else {
      inputFile = arg;
    }
  }

  if (inputFile.empty()) {
    std::cerr << "Error: No input file specified\n";
    std::cerr << "Try '" << argv[0] << " --help' for more information\n";
    return 1;
  }

  std::ifstream file(inputFile);
  if (!file.is_open()) {
    std::cerr << "Error: Cannot open file '" << inputFile << "': ";
    std::perror("");
    return 1;
  }

  std::string fileContent;
  std::string line;
  while (std::getline(file, line)) {
    fileContent += line + '\n';
  }
  file.close();

  if (fileContent.empty()) {
    std::cerr << "Warning: Input file '" << inputFile << "' is empty\n";
  }

  if (outputFile.empty()) {
    outputFile = inputFile;
    size_t lastDot = outputFile.find_last_of(".");
    if (lastDot != std::string::npos) {
      outputFile = outputFile.substr(0, lastDot);
    }
  }

  if (debugMode) {
    std::cout << "Debug mode enabled\n";
    std::cout << "Input file: " << inputFile << "\n";
    std::cout << "Output file: " << outputFile << "\n";
  }

  zap::DiagnosticEngine diagnostics(fileContent, inputFile);

  Lexer lex(diagnostics);
  auto toks = lex.tokenize(fileContent);

  if (debugMode) {
    std::cout << "\nTokens:\n";
    for (const auto &token : toks) {
      std::cout << "  Token: " << token.type << " Value: " << token.value
                << "\n";
    }
  }

  zap::Parser parser(toks, diagnostics);
  auto ast = parser.parse();

  if (diagnostics.hadErrors()) {
    return 1;
  }

  if (!ast) {
    std::cerr << "Error: Parsing failed\n";
    return 1;
  }

  if (debugMode) {
    std::cout << "\nAST built successfully.\n";
  }

  sema::Binder binder(diagnostics);
  auto boundAst = binder.bind(*ast);

  if (!boundAst) {
    std::cerr << "Error: Semantic analysis failed\n";
    return 1;
  }

  if (debugMode) {
    std::cout << "Semantic analysis successful.\n";
  }

  if (displayZIR) {
    zir::BoundIRGenerator irGen;
    auto module = irGen.generate(*boundAst);
    if (module) {
      std::cout << "\nZap Intermediate Representation (ZIR):\n";
      std::cout << module->toString() << "\n";
    } else {
      std::cerr << "Error: IR generation failed\n";
      return 1;
    }
  }

  if (displayLLVM) {
    codegen::LLVMCodeGen llvmGen;
    llvmGen.generate(*boundAst);
    std::cout << "\nLLVM IR:\n";
    llvmGen.printIR();
  } else {
    codegen::LLVMCodeGen llvmGen;
    llvmGen.generate(*boundAst);

    const std::string objFile = outputFile + ".o";
    if (!llvmGen.emitObjectFile(objFile)) {
      std::cerr << "Error: object file emission failed\n";
      return 1;
    }

    const std::string linkCmd = "cc " + objFile + " -o " + outputFile;
    int ret = std::system(linkCmd.c_str());
    if (ret != 0) {
      std::cerr << "Error: linking failed\n";
      return 1;
    }

    std::remove(objFile.c_str());

    if (debugMode)
      std::cout << "Binary written to: " << outputFile << "\n";
  }

  return 0;
}
