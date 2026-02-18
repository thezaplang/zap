#include "lexer.hpp"
#include <cstdlib>
std::vector<Token> Lexer::tokenize(const std::string &input)
{
  std::vector<Token> tokens;
  _pos = 0;
  _input = input;
  while (!isAtEnd())
  {
    char _cur = _input[_pos];
    unsigned int startPos = _pos;

    if (_cur == '(')
    {
      tokens.emplace_back(startPos, TokenType::LPAREN, "(");
      ++_pos;
      continue;
    }
    else if (_cur == ')')
    {
      tokens.emplace_back(startPos, TokenType::RPAREN, ")");
      ++_pos;
      continue;
    }
    else if (_cur == '{')
    {
      tokens.emplace_back(startPos, TokenType::LBRACE, "{");
      ++_pos;
      continue;
    }
    else if (_cur == '}')
    {
      tokens.emplace_back(startPos, TokenType::RBRACE, "}");
      ++_pos;
      continue;
    }
    else if (_cur == '[')
    {
      tokens.emplace_back(startPos, TokenType::SQUARE_LBRACE, "[");
      ++_pos;
      continue;
    }
    else if (_cur == ']')
    {
      tokens.emplace_back(startPos, TokenType::SQUARE_RBRACE, "]");
      ++_pos;
      continue;
    }
    else if (_cur == ';')
    {
      tokens.emplace_back(startPos, TokenType::SEMICOLON, ";");
      ++_pos;
      continue;
    }
    else if (_cur == ',')
    {
      tokens.emplace_back(startPos, TokenType::COMMA, ",");
      ++_pos;
      continue;
    }
    else if (_cur == ':')
    {
      if (Peek2() == ':')
      {
        tokens.emplace_back(startPos, TokenType::DOUBLECOLON, "::");
        _pos += 2;
        continue;
      }
      else
      {
        tokens.emplace_back(startPos, TokenType::COLON, ":");
        ++_pos;
        continue;
      }
    }
    else if (_cur == '.')
    {
      if (Peek2() == '.' && Peek3() == '.')
      {
        tokens.emplace_back(startPos, TokenType::ELLIPSIS, "...");
        _pos += 3;
        continue;
      }
      else
      {
        tokens.emplace_back(startPos, TokenType::DOT, ".");
        ++_pos;
        continue;
      }
    }
    else if (_cur == '?')
    {
      tokens.emplace_back(startPos, TokenType::QUESTION, "?");
      ++_pos;
      continue;
    }
    else if (_cur == '+')
    {
      tokens.emplace_back(startPos, TokenType::PLUS, "+");
      ++_pos;
      continue;
    }
    else if (_cur == '*')
    {
      tokens.emplace_back(startPos, TokenType::MULTIPLY, "*");
      ++_pos;
      continue;
    }
    else if (_cur == '-')
    {
      if (Peek2() == '>')
      {
        tokens.emplace_back(startPos, TokenType::ARROW, "->");
        _pos += 2;
        continue;
      }
      tokens.emplace_back(startPos, TokenType::MINUS, "-");
      ++_pos;
      continue;
    }
    else if (_cur == '/')
    {
      tokens.emplace_back(startPos, TokenType::DIVIDE, "/");
      ++_pos;
      continue;
    }
    else if (_cur == '%')
    {
      tokens.emplace_back(startPos, TokenType::MODULO, "%");
      ++_pos;
      continue;
    }
    else if (_cur == '^')
    {
      tokens.emplace_back(startPos, TokenType::POW, "^");
      ++_pos;
      continue;
    }
    else if (_cur == '&')
    {
      if (Peek2() == '&')
      {
        tokens.emplace_back(startPos, TokenType::AND, "&&");
        _pos += 2;
        continue;
      }
      else
      {
        tokens.emplace_back(startPos, TokenType::REFERENCE, "&");
        ++_pos;
        continue;
      }
    }
    else if (_cur == '|')
    {
      if (Peek2() == '|')
      {
        tokens.emplace_back(startPos, TokenType::OR, "||");
        _pos += 2;
        continue;
      }
      else
      {
        tokens.emplace_back(startPos, TokenType::OR, "|");
        ++_pos;
        continue;
      }
    }
    else if (_cur == '~')
    {
      tokens.emplace_back(startPos, TokenType::CONCAT, "~");
      ++_pos;
      continue;
    }
    else if (_cur == '=')
    {
      if (Peek2() == '=')
      {
        tokens.emplace_back(startPos, TokenType::EQUAL, "==");
        _pos += 2;
        continue;
      }
      else
      {
        tokens.emplace_back(startPos, TokenType::ASSIGN, "=");
        ++_pos;
        continue;
      }
    }
    else if (_cur == '!')
    {
      if (Peek2() == '=')
      {
        tokens.emplace_back(startPos, TokenType::NOTEQUAL, "!=");
        _pos += 2;
        continue;
      }
      else
      {
        tokens.emplace_back(startPos, TokenType::NOT, "!");
        ++_pos;
        continue;
      }
    }
    else if (_cur == '<')
    {
      if (Peek2() == '=')
      {
        tokens.emplace_back(startPos, TokenType::LESSEQUAL, "<=");
        _pos += 2;
        continue;
      }
      else
      {
        tokens.emplace_back(startPos, TokenType::LESS, "<");
        ++_pos;
        continue;
      }
    }
    else if (_cur == '>')
    {
      if (Peek2() == '=')
      {
        tokens.emplace_back(startPos, TokenType::GREATEREQUAL, ">=");
        _pos += 2;
        continue;
      }
      else
      {
        tokens.emplace_back(startPos, TokenType::GREATER, ">");
        ++_pos;
        continue;
      }
    }
    else if (std::isdigit(_cur))
    {
      std::string numStr;
      bool isFloat = false;
      unsigned int numStart = _pos;
      while (!isAtEnd() && std::isdigit(_input[_pos]))
      {
        numStr += _input[_pos++];
      }
      if (!isAtEnd() && _input[_pos] == '.')
      {
        isFloat = true;
        numStr += _input[_pos++];
        while (!isAtEnd() && std::isdigit(_input[_pos]))
        {
          numStr += _input[_pos++];
        }
      }
      if (isFloat)
      {
        tokens.emplace_back(numStart, TokenType::FLOAT, numStr);
      }
      else
      {
        tokens.emplace_back(numStart, TokenType::INTEGER, numStr);
      }
      continue;
    }
    else if (std::isalpha(_cur) || _cur == '_')
    {
      std::string identStr;
      unsigned int idStart = _pos;
      while (!isAtEnd() && (std::isalnum(_input[_pos]) || _input[_pos] == '_'))
      {
        identStr += _input[_pos++];
      }
      if (identStr == "if")
        tokens.emplace_back(idStart, TokenType::IF, identStr);
      else if (identStr == "else")
        tokens.emplace_back(idStart, TokenType::ELSE, identStr);
      else if (identStr == "while")
        tokens.emplace_back(idStart, TokenType::WHILE, identStr);
      else if (identStr == "for")
        tokens.emplace_back(idStart, TokenType::FOR, identStr);
      else if (identStr == "return" || identStr == "ret")
        tokens.emplace_back(idStart, TokenType::RETURN, identStr);
      else if (identStr == "true" || identStr == "false")
        tokens.emplace_back(idStart, TokenType::BOOL, identStr);
      else if (identStr == "fun")
        tokens.emplace_back(idStart, TokenType::FUN, identStr);
      else if (identStr == "import")
        tokens.emplace_back(idStart, TokenType::IMPORT, identStr);
      else if (identStr == "match")
        tokens.emplace_back(idStart, TokenType::MATCH, identStr);
      else if (identStr == "var")
        tokens.emplace_back(idStart, TokenType::VAR, identStr);
      else if (identStr == "ext")
        tokens.emplace_back(idStart, TokenType::EXTERN, identStr);
      else if (identStr == "module")
        tokens.emplace_back(idStart, TokenType::MODULE, identStr);
      else if (identStr == "pub")
        tokens.emplace_back(idStart, TokenType::PUB, identStr);
      else if (identStr == "priv")
        tokens.emplace_back(idStart, TokenType::PRIV, identStr);
      else if (identStr == "struct")
        tokens.emplace_back(idStart, TokenType::STRUCT, identStr);
      else if (identStr == "impl")
        tokens.emplace_back(idStart, TokenType::IMPL, identStr);
      else if (identStr == "static")
        tokens.emplace_back(idStart, TokenType::STATIC, identStr);
      else if (identStr == "enum")
        tokens.emplace_back(idStart, TokenType::ENUM, identStr);
      else if (identStr == "break")
        tokens.emplace_back(idStart, TokenType::BREAK, identStr);
      else if (identStr == "continue")
        tokens.emplace_back(idStart, TokenType::CONTINUE, identStr);
      else
        tokens.emplace_back(idStart, TokenType::ID, identStr);
      continue;
    }
    else if (std::isspace(_cur))
    {
      ++_pos;
      continue;
    }
    else if (_cur == '"')
    {
      std::string strVal;
      unsigned int strStart = _pos;
      ++_pos; // skip opening quote
      while (!isAtEnd() && _input[_pos] != '"')
      {
        if (_input[_pos] == '\\')
        {
          ++_pos;
          if (isAtEnd())
            break;
          switch (_input[_pos])
          {
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
        }
        else
        {
          strVal += _input[_pos];
        }
        ++_pos;
      }
      if (!isAtEnd() && _input[_pos] == '"')
      {
        ++_pos; // skip closing quote
        tokens.emplace_back(strStart, TokenType::STRING, strVal);
        continue;
      }
      else
      {
        printf("Lexer Error: Unterminated string at position %u\n", strStart);
        exit(EXIT_FAILURE);
      }
    }
    else
    {
      printf("Lexer Error: Unexpected character '%c' at position %u\n", _cur,
             _pos);
      exit(EXIT_FAILURE);
    }
  }
  return tokens;
}

char Lexer::Peek2()
{
  if (_pos + 1 < _input.size())
  {
    return _input[_pos + 1];
  }
  return '\0';
}

char Lexer::Peek3()
{
  if (_pos + 2 < _input.size())
  {
    return _input[_pos + 2];
  }
  return '\0';
}

bool Lexer::isAtEnd() const { return _pos >= _input.size(); }
