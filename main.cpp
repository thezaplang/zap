#include <iostream>
#include <fstream>
#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#define LEXER_DEBUG false
#define PARSER_DEBUG false
#define COMPILER_DEBUG false

int main(int argc, char *argv[])
{
  // zapc <file1>
  if (argc < 2)
  {
    std::cerr << "Usage: " << argv[0] << " file.vrn " << std::endl;
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
        std::cout << "Token: " << token.type << " Value: " << token.value << std::endl;
      }
    }
    auto root = parser.parse(toks);
  }
  return 0;
}