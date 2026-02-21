#pragma once
#include "../token/token.hpp"
#include <string>
#include <vector>
class Lexer {
public:
  size_t _pos;
  size_t _line;
  std::string _input;
  Lexer() {}
  ~Lexer() {}
  std::vector<Token> tokenize(const std::string &input);
  char Peek2();
  char Peek3();
  bool isAtEnd() const;
};
