#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#include "compiler/codegen.hpp"
#include <fstream>
#include <iostream>
#define LEXER_DEBUG false
#define PARSER_DEBUG false
#define COMPILER_DEBUG false

int main(int argc, char *argv[])
{
  // zapc <file1>
  if (argc < 2)
  {
    std::cerr << "Usage: " << argv[0] << " zap " << std::endl;
    return 1;
  }
  else
  {
    std::string fileContent;
    std::ifstream file(argv[1]);
    if (!file)
    {
      printf("%s didnt exsist \n", argv[1]);
    }
    std::string line;

    while (std::getline(file, line))
    {
      fileContent += line + '\n';
    }
    file.close();
    printf("%s \n", fileContent.c_str());
    Lexer lex;
    auto toks = lex.tokenize(fileContent);
    Parser parser;
    if (LEXER_DEBUG)
    {
      for (const auto &token : toks)
      {
        std::cout << "Token: " << token.type << " Value: " << token.value
                  << std::endl;
      }
    }
    auto root = parser.parse(toks);

    zap::Compiler compiler;
    compiler.compile(root);

    // Extract output filename from input without extension
    std::string outputName = argv[1];
    size_t lastDot = outputName.find_last_of(".");
    if (lastDot != std::string::npos)
    {
      outputName = outputName.substr(0, lastDot);
    }

    // Emit IR to .ll file
    std::string irFile = outputName + ".ll";
    compiler.emitIRToFile(irFile);

    compiler.compileIR(irFile, outputName);

    if (COMPILER_DEBUG)
    {
      std::cout << "Compilation successful!" << std::endl;
    }
  }
  return 0;
}