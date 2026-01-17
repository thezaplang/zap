#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#include "compiler/ast_to_ir.hpp"
#include "sema/sema.hpp"
#include <fstream>
#include <iostream>
#include <string>
#include <cstring>

#define ZAP_VERSION "0.1.0"

using namespace zap;

void printHelp(const char *programName)
{
  std::cout << "Zap Compiler v" << ZAP_VERSION << "\n\n";
  std::cout << "Usage: " << programName << " [options] <file.zap>\n\n";
  std::cout << "Options:\n";
  std::cout << "  -h, --help              Show this help message\n";
  std::cout << "  -v, --version           Show version information\n";
  std::cout << "  -o, --output <file>     Specify output file name\n";
  std::cout << "  --debug                 Enable debug output\n";
  std::cout << "\nExample:\n";
  std::cout << "  " << programName << " main.zap\n";
  std::cout << "  " << programName << " -o myprogram main.zap\n";
}

void printVersion()
{
  std::cout << "Zap Compiler v" << ZAP_VERSION << "\n";
}

int main(int argc, char *argv[])
{
  std::string inputFile;
  std::string outputFile;
  bool debugMode = false;

  // Parse command line arguments
  if (argc < 2)
  {
    std::cerr << "Error: No input file specified\n";
    std::cerr << "Try '" << argv[0] << " --help' for more information\n";
    return 1;
  }

  for (int i = 1; i < argc; i++)
  {
    std::string arg(argv[i]);

    if (arg == "-h" || arg == "--help")
    {
      printHelp(argv[0]);
      return 0;
    }
    else if (arg == "-v" || arg == "--version")
    {
      printVersion();
      return 0;
    }
    else if (arg == "--debug")
    {
      debugMode = true;
    }
    else if (arg == "-o" || arg == "--output")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "Error: -o/--output requires an argument\n";
        return 1;
      }
      outputFile = argv[++i];
    }
    else if (arg[0] == '-')
    {
      std::cerr << "Error: Unknown option '" << arg << "'\n";
      std::cerr << "Try '" << argv[0] << " --help' for more information\n";
      return 1;
    }
    else
    {
      inputFile = arg;
    }
  }

  if (inputFile.empty())
  {
    std::cerr << "Error: No input file specified\n";
    std::cerr << "Try '" << argv[0] << " --help' for more information\n";
    return 1;
  }

  // Check if file exists and is readable
  std::ifstream file(inputFile);
  if (!file.is_open())
  {
    std::cerr << "Error: Cannot open file '" << inputFile << "': ";
    std::perror("");
    return 1;
  }

  // Read file content
  std::string fileContent;
  std::string line;
  while (std::getline(file, line))
  {
    fileContent += line + '\n';
  }
  file.close();

  if (fileContent.empty())
  {
    std::cerr << "Warning: Input file '" << inputFile << "' is empty\n";
  }

  // Determine output file name
  if (outputFile.empty())
  {
    outputFile = inputFile;
    size_t lastDot = outputFile.find_last_of(".");
    if (lastDot != std::string::npos)
    {
      outputFile = outputFile.substr(0, lastDot);
    }
  }

  if (debugMode)
  {
    std::cout << "Debug mode enabled\n";
    std::cout << "Input file: " << inputFile << "\n";
    std::cout << "Output file: " << outputFile << "\n";
  }

  try
  {
    // Tokenization
    Lexer lex;
    auto toks = lex.tokenize(fileContent);

    if (debugMode)
    {
      std::cout << "\nTokens:\n";
      for (const auto &token : toks)
      {
        std::cout << "  Token: " << token.type << " Value: " << token.value
                  << "\n";
      }
    }

    // Create symbol table and add built-in functions
    auto symTable = std::make_shared<sema::SymbolTable>();
    zap::sema::FunctionSymbol printlnSymbol{
        "println",
        false, // isExtern
        false, // isStatic
        true,  // isPublic
        zap::sema::Scope()};
    symTable->addFunction(std::move(printlnSymbol));

    // Parsing
    Parser parser(symTable);
    auto root = parser.parse(toks);

    // AST to IR conversion
    zap::compiler::ASTToIRConverter converter;
    auto irModule = converter.convert(root.get());

    // Output IR module
    std::string irOutput = irModule->toString();

    if (!outputFile.empty())
    {
      std::ofstream irStream(outputFile);
      irStream << irOutput;
      irStream.close();
      std::cout << "IR output: " << outputFile << "\n";
    }
    else
    {
      std::cout << irOutput;
    }

    std::cout << "Compilation successful!\n";
    std::cout << "Output: " << outputFile << "\n";
  }
  catch (const std::exception &e)
  {
    std::cerr << "Error: Compilation failed\n";
    std::cerr << "  " << e.what() << "\n";
    return 1;
  }
  catch (...)
  {
    std::cerr << "Error: Unknown compilation error\n";
    return 1;
  }

  return 0;
}