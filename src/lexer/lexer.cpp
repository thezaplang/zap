#include "lexer.hpp"
#include <cctype>
#include <cstdlib>
#include <stdexcept>

std::vector<Token> Lexer::tokenize(const std::string &input) {
  std::vector<Token> tokens;
  _pos = 0;
  _line = 1;
  _column = 1;
  _input = input;

  while (!isAtEnd()) {
    char _cur = _input[_pos];
    size_t startPos = _pos;
    size_t startLine = _line;
    size_t startColumn = _column;

    if (_cur == '(') {
      tokens.emplace_back(TokenType::LPAREN, "(", startLine, startColumn,
                          startPos, 1);
      ++_pos;
      ++_column;
      continue;
    } else if (_cur == ')') {
      tokens.emplace_back(TokenType::RPAREN, ")", startLine, startColumn,
                          startPos, 1);
      ++_pos;
      ++_column;
      continue;
    } else if (_cur == '{') {
      tokens.emplace_back(TokenType::LBRACE, "{", startLine, startColumn,
                          startPos, 1);
      ++_pos;
      ++_column;
      continue;
    } else if (_cur == '}') {
      tokens.emplace_back(TokenType::RBRACE, "}", startLine, startColumn,
                          startPos, 1);
      ++_pos;
      ++_column;
      continue;
    } else if (_cur == '[') {
      tokens.emplace_back(TokenType::SQUARE_LBRACE, "[", startLine, startColumn,
                          startPos, 1);
      ++_pos;
      ++_column;
      continue;
    } else if (_cur == ']') {
      tokens.emplace_back(TokenType::SQUARE_RBRACE, "]", startLine, startColumn,
                          startPos, 1);
      ++_pos;
      ++_column;
      continue;
    } else if (_cur == ';') {
      tokens.emplace_back(TokenType::SEMICOLON, ";", startLine, startColumn,
                          startPos, 1);
      ++_pos;
      ++_column;
      continue;
    } else if (_cur == ',') {
      tokens.emplace_back(TokenType::COMMA, ",", startLine, startColumn,
                          startPos, 1);
      ++_pos;
      ++_column;
      continue;
    } else if (_cur == ':') {
      if (Peek2() == ':') {
        tokens.emplace_back(TokenType::DOUBLECOLON, "::", startLine,
                            startColumn, startPos, 2);
        _pos += 2;
        _column += 2;
        continue;
      } else {
        tokens.emplace_back(TokenType::COLON, ":", startLine, startColumn,
                            startPos, 1);
        ++_pos;
        ++_column;
        continue;
      }
    } else if (_cur == '.') {
      if (Peek2() == '.' && Peek3() == '.') {
        tokens.emplace_back(TokenType::ELLIPSIS, "...", startLine, startColumn,
                            startPos, 3);
        _pos += 3;
        _column += 3;
        continue;
      } else {
        tokens.emplace_back(TokenType::DOT, ".", startLine, startColumn,
                            startPos, 1);
        ++_pos;
        ++_column;
        continue;
      }
    } else if (_cur == '?') {
      tokens.emplace_back(TokenType::QUESTION, "?", startLine, startColumn,
                          startPos, 1);
      ++_pos;
      ++_column;
      continue;
    } else if (_cur == '+') {
      tokens.emplace_back(TokenType::PLUS, "+", startLine, startColumn,
                          startPos, 1);
      ++_pos;
      ++_column;
      continue;
    } else if (_cur == '*') {
      tokens.emplace_back(TokenType::MULTIPLY, "*", startLine, startColumn,
                          startPos, 1);
      ++_pos;
      ++_column;
      continue;
    } else if (_cur == '-') {
      if (Peek2() == '>') {
        tokens.emplace_back(TokenType::ARROW, "->", startLine, startColumn,
                            startPos, 2);
        _pos += 2;
        _column += 2;
        continue;
      }
      tokens.emplace_back(TokenType::MINUS, "-", startLine, startColumn,
                          startPos, 1);
      ++_pos;
      ++_column;
      continue;
    } else if (_cur == '/') {
      if (Peek2() == '/') {
        while (!isAtEnd() && _input[_pos] != '\n') {
          ++_pos;
        }
        continue;
      } else {
        tokens.emplace_back(TokenType::DIVIDE, "/", startLine, startColumn,
                            startPos, 1);
        ++_pos;
        ++_column;
        continue;
      }
    } else if (_cur == '%') {
      tokens.emplace_back(TokenType::MODULO, "%", startLine, startColumn,
                          startPos, 1);
      ++_pos;
      ++_column;
      continue;
    } else if (_cur == '^') {
      tokens.emplace_back(TokenType::POW, "^", startLine, startColumn, startPos,
                          1);
      ++_pos;
      ++_column;
      continue;
    } else if (_cur == '&') {
      if (Peek2() == '&') {
        tokens.emplace_back(TokenType::AND, "&&", startLine, startColumn,
                            startPos, 2);
        _pos += 2;
        _column += 2;
        continue;
      } else {
        tokens.emplace_back(TokenType::REFERENCE, "&", startLine, startColumn,
                            startPos, 1);
        ++_pos;
        ++_column;
        continue;
      }
    } else if (_cur == '|') {
      if (Peek2() == '|') {
        tokens.emplace_back(TokenType::OR, "||", startLine, startColumn,
                            startPos, 2);
        _pos += 2;
        _column += 2;
        continue;
      } else {
        tokens.emplace_back(TokenType::BIT_OR, "|", startLine, startColumn,
                            startPos, 1);
        ++_pos;
        ++_column;
        continue;
      }
    } else if (_cur == '~') {
      tokens.emplace_back(TokenType::CONCAT, "~", startLine, startColumn,
                          startPos, 1);
      ++_pos;
      ++_column;
      continue;
    } else if (_cur == '=') {
      if (Peek2() == '=') {
        tokens.emplace_back(TokenType::EQUAL, "==", startLine, startColumn,
                            startPos, 2);
        _pos += 2;
        _column += 2;
        continue;
      } else {
        tokens.emplace_back(TokenType::ASSIGN, "=", startLine, startColumn,
                            startPos, 1);
        ++_pos;
        ++_column;
        continue;
      }
    } else if (_cur == '!') {
      if (Peek2() == '=') {
        tokens.emplace_back(TokenType::NOTEQUAL, "!=", startLine, startColumn,
                            startPos, 2);
        _pos += 2;
        _column += 2;
        continue;
      } else {
        tokens.emplace_back(TokenType::NOT, "!", startLine, startColumn,
                            startPos, 1);
        ++_pos;
        ++_column;
        continue;
      }
    } else if (_cur == '<') {
      if (Peek2() == '=') {
        tokens.emplace_back(TokenType::LESSEQUAL, "<=", startLine, startColumn,
                            startPos, 2);
        _pos += 2;
        _column += 2;
        continue;
      } else {
        tokens.emplace_back(TokenType::LESS, "<", startLine, startColumn,
                            startPos, 1);
        ++_pos;
        ++_column;
        continue;
      }
    } else if (_cur == '>') {
      if (Peek2() == '=') {
        tokens.emplace_back(TokenType::GREATEREQUAL, ">=", startLine,
                            startColumn, startPos, 2);
        _pos += 2;
        _column += 2;
        continue;
      } else {
        tokens.emplace_back(TokenType::GREATER, ">", startLine, startColumn,
                            startPos, 1);
        ++_pos;
        ++_column;
        continue;
      }
    } else if (std::isdigit(_cur)) {
      std::string numStr;
      bool isFloat = false;

      if (_cur == '0' &&
          (Peek2() == 'x' || Peek2() == 'X' || Peek2() == 'b' ||
           Peek2() == 'B' || Peek2() == 'o' || Peek2() == 'O')) {
        const char prefix = Peek2();
        int base = 10;
        auto isValidDigit = [&](char ch) {
          if (ch == '_')
            return true;
          if (prefix == 'x' || prefix == 'X')
            return std::isxdigit(static_cast<unsigned char>(ch)) != 0;
          if (prefix == 'b' || prefix == 'B')
            return ch == '0' || ch == '1';
          if (prefix == 'o' || prefix == 'O')
            return ch >= '0' && ch <= '7';
          return false;
        };

        if (prefix == 'x' || prefix == 'X')
          base = 16;
        else if (prefix == 'b' || prefix == 'B')
          base = 2;
        else if (prefix == 'o' || prefix == 'O')
          base = 8;

        numStr += _input[_pos++];
        _column++;
        numStr += _input[_pos++];
        _column++;

        std::string numericPart;
        size_t digitsStart = _pos;

        while (!isAtEnd() && isValidDigit(_input[_pos])) {
          if (_input[_pos] != '_') {
            numericPart += _input[_pos];
          }
          numStr += _input[_pos++];
          _column++;
        }

        if (_pos == digitsStart || numericPart.empty()) {
          _diag.report(SourceSpan(startLine, startColumn, startPos, _pos - startPos),
                       zap::DiagnosticLevel::Error, "Invalid integer literal");
          return tokens;
        }

        std::string parsed = "0";
        parsed += prefix;
        parsed += numericPart;

        try {
          (void)std::stoull(parsed, nullptr, base);
        } catch (const std::exception &) {
          _diag.report(SourceSpan(startLine, startColumn, startPos, _pos - startPos),
                       zap::DiagnosticLevel::Error, "Integer literal out of range");
          return tokens;
        }

        size_t len = numStr.length();
        tokens.emplace_back(TokenType::INTEGER, parsed, startLine, startColumn,
                            startPos, len);
        continue;
      }

      while (!isAtEnd() && (std::isdigit(_input[_pos]) || _input[_pos] == '_')) {
        if (_input[_pos] != '_') {
          numStr += _input[_pos];
        }
        _pos++;
        _column++;
      }
      if (!isAtEnd() && _input[_pos] == '.') {
        isFloat = true;
        numStr += _input[_pos++];
        _column++;
        while (!isAtEnd() && (std::isdigit(_input[_pos]) || _input[_pos] == '_')) {
          if (_input[_pos] != '_') {
            numStr += _input[_pos];
          }
          _pos++;
          _column++;
        }
      }
      size_t len = numStr.length();
      if (isFloat) {
        tokens.emplace_back(TokenType::FLOAT, numStr, startLine, startColumn,
                            startPos, len);
      } else {
        tokens.emplace_back(TokenType::INTEGER, numStr, startLine, startColumn,
                            startPos, len);
      }
      continue;
    } else if (std::isalpha(_cur) || _cur == '_') {
      std::string identStr;
      while (!isAtEnd() &&
             (std::isalnum(_input[_pos]) || _input[_pos] == '_')) {
        identStr += _input[_pos++];
        _column++;
      }
      size_t len = identStr.length();

      TokenType type = TokenType::ID;
      if (identStr == "if")
        type = TokenType::IF;
      else if (identStr == "else")
        type = TokenType::ELSE;
      else if (identStr == "while")
        type = TokenType::WHILE;
      else if (identStr == "for")
        type = TokenType::FOR;
      else if (identStr == "return" || identStr == "ret")
        type = TokenType::RETURN;
      else if (identStr == "true" || identStr == "false")
        type = TokenType::BOOL;
      else if (identStr == "fun")
        type = TokenType::FUN;
      else if (identStr == "import")
        type = TokenType::IMPORT;
      else if (identStr == "match")
        type = TokenType::MATCH;
      else if (identStr == "var")
        type = TokenType::VAR;
      else if (identStr == "ext")
        type = TokenType::EXTERN;
      else if (identStr == "module")
        type = TokenType::MODULE;
      else if (identStr == "pub")
        type = TokenType::PUB;
      else if (identStr == "priv")
        type = TokenType::PRIV;
      else if (identStr == "record")
        type = TokenType::RECORD;
      else if (identStr == "impl")
        type = TokenType::IMPL;
      else if (identStr == "static")
        type = TokenType::STATIC;
      else if (identStr == "enum")
        type = TokenType::ENUM;
      else if (identStr == "struct")
        type = TokenType::STRUCT;
      else if (identStr == "break")
        type = TokenType::BREAK;
      else if (identStr == "continue")
        type = TokenType::CONTINUE;
      else if (identStr == "global")
        type = TokenType::GLOBAL;
      else if (identStr == "const")
        type = TokenType::CONST;
      else if (identStr == "unsafe")
        type = TokenType::UNSAFE;
      else if (identStr == "null")
        type = TokenType::NULL_LITERAL;
      else if (identStr == "alias")
        type = TokenType::ALIAS;
      else if (identStr == "ref")
        type = TokenType::REF;
      else if (identStr == "as")
        type = TokenType::AS;
      else if (identStr == "where")
        type = TokenType::WHERE;
      else if (identStr == "iftype")
        type = TokenType::IFTYPE;
      else if (identStr == "class")
        type = TokenType::CLASS;
      else if (identStr == "prot")
        type = TokenType::PROT;
      else if (identStr == "new")
        type = TokenType::NEW;
      else if (identStr == "weak")
        type = TokenType::WEAK;

      tokens.emplace_back(type, identStr, startLine, startColumn, startPos,
                          len);
      continue;
    } else if (std::isspace(_cur)) {
      if (_cur == '\n') {
        ++_line;
        _column = 1;
      } else {
        ++_column;
      }
      ++_pos;
      continue;
    } else if (_cur == '"') {
      std::string strVal;
      size_t strStart = _pos;
      ++_pos;
      _column++;

      while (!isAtEnd() && _input[_pos] != '"') {
        if (_input[_pos] == '\n') {
          ++_line;
          _column = 1;
        } else {
          _column++;
        }

        if (_input[_pos] == '\\') {
          ++_pos;
          if (isAtEnd())
            break;

          switch (_input[_pos]) {
          case 'n':
            strVal += '\n';
            break;
          case 't':
            strVal += '\t';
            break;
          case 'r':
            strVal += '\r';
            break;
          case '\\':
            strVal += '\\';
            break;
          case '"':
            strVal += '"';
            break;
          case '0':
            strVal += '\0';
            break;
          case 'w':
            strVal += ' ';
            break;
          default:
            strVal += _input[_pos];
            break;
          }
        } else {
          strVal += _input[_pos];
        }
        ++_pos;
      }

      if (!isAtEnd() && _input[_pos] == '"') {
        ++_pos;
        _column++;
        size_t len = _pos - strStart;
        tokens.emplace_back(TokenType::STRING, strVal, startLine, startColumn,
                            startPos, len);
        continue;
      } else {
        _diag.report(
            SourceSpan(startLine, startColumn, strStart, _pos - strStart),
            zap::DiagnosticLevel::Error, "Unterminated string literal");
        return tokens;
      }
    } else if (_cur == '\'') {
      // char literal
      std::string charVal;
      size_t charStart = _pos;
      ++_pos;
      _column++;
      if (isAtEnd()) {
        _diag.report(SourceSpan(startLine, startColumn, charStart, 1),
                     zap::DiagnosticLevel::Error, "Unterminated char literal");
        return tokens;
      }
      if (_input[_pos] == '\\') {
        ++_pos;
        if (isAtEnd()) {
          _diag.report(SourceSpan(startLine, startColumn, charStart, 1),
                       zap::DiagnosticLevel::Error,
                       "Unterminated char literal");
          return tokens;
        }
        switch (_input[_pos]) {
        case 'n':
          charVal += '\n';
          break;
        case 't':
          charVal += '\t';
          break;
        case 'r':
          charVal += '\r';
          break;
        case '\\':
          charVal += '\\';
          break;
        case '\'':
          charVal += '\'';
          break;
        case '0':
          charVal += '\0';
          break;
        default:
          charVal += _input[_pos];
          break;
        }
      } else {
        charVal += _input[_pos];
      }
      ++_pos;
      _column++;
      if (isAtEnd() || _input[_pos] != '\'') {
        _diag.report(SourceSpan(startLine, startColumn, charStart, 1),
                     zap::DiagnosticLevel::Error, "Unterminated char literal");
        return tokens;
      }
      ++_pos;
      _column++;
      tokens.emplace_back(TokenType::CHAR, charVal, startLine, startColumn,
                          startPos, 3);
      continue;
    } else {
      _diag.report(SourceSpan(startLine, startColumn, _pos, 1),
                   zap::DiagnosticLevel::Error,
                   "Unexpected character '" + std::string(1, _cur) + "'");
      _pos++;
      _column++;
    }
  }
  return tokens;
}

char Lexer::Peek2() {
  if (_pos + 1 < _input.size()) {
    return _input[_pos + 1];
  }
  return '\0';
}

char Lexer::Peek3() {
  if (_pos + 2 < _input.size()) {
    return _input[_pos + 2];
  }
  return '\0';
}

bool Lexer::isAtEnd() const noexcept { return _pos >= _input.size(); }
