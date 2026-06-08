#pragma once
#include "../token/token.hpp"
#include "../utils/diagnostics.hpp"
#include <string>
#include <vector>

class Lexer {
public:
  zap::DiagnosticEngine &_diag;
  size_t _pos;
  size_t _line;
  size_t _column;
  std::string _input;

  Lexer(zap::DiagnosticEngine &diag) noexcept(
      std::is_nothrow_default_constructible<std::string>::value)
      : _diag(diag) {}
  ~Lexer() noexcept {}
  std::vector<Token> tokenize(const std::string &input);
  char Peek2();
  char Peek3();
  bool isAtEnd() const noexcept;
};
