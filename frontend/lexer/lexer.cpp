//
// Created by Funcieq on 26.11.2025.
//

#include "lexer.h"
#include "../../utils/utils.h"
#include <cstdlib>
#include <iostream>
#include <string>

std::vector<Token> Lexer::Tokenize(std::string_view content)
{
  std::vector<Token> tokens;

  current_file = std::string(content);
  pos = 0;

  while (true)
  {
    char c = Peek();
    if (c == '\0')
      break;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
    {
      Advance();
      continue;
    }

    if (c == ';')
    {
      tokens.emplace_back(TokenType::Semi, pos,
                          std::string_view(current_file.data() + pos, 1));
      Advance();
      continue;
    }

    if (c == '(')
    {
      tokens.emplace_back(TokenType::LParen, pos,
                          std::string_view(current_file.data() + pos, 1));
      Advance();
      continue;
    }

    if (c == ')')
    {
      tokens.emplace_back(TokenType::RParen, pos,
                          std::string_view(current_file.data() + pos, 1));
      Advance();
      continue;
    }

    if (c == '{')
    {
      tokens.emplace_back(TokenType::LBrace, pos,
                          std::string_view(current_file.data() + pos, 1));
      Advance();
      continue;
    }

    if (c == '}')
    {
      tokens.emplace_back(TokenType::RBrace, pos,
                          std::string_view(current_file.data() + pos, 1));
      Advance();
      continue;
    }

    if (c == '=')
    {
      if (Peek(1) == '=')
      {
        tokens.emplace_back(TokenType::Operator, pos,
                            std::string_view(current_file.data() + pos, 2));
        Advance();
        Advance();
        continue;
      }
      else
      {
        tokens.emplace_back(TokenType::Assign, pos,
                            std::string_view(current_file.data() + pos, 1));
        Advance();
        continue;
      }
    }

    if (c == '<' || c == '>')
    {
      tokens.emplace_back(TokenType::Operator, pos,
                          std::string_view(current_file.data() + pos, 1));
      Advance();
      continue;
    }

    if (c == ':')
    {
      if (Peek(1) == ':')
      {
        tokens.emplace_back(TokenType::DoubleColon, pos,
                            std::string_view(current_file.data() + pos, 2));
        Advance();
        Advance();
      }
      else
      {
        tokens.emplace_back(TokenType::Colon, pos,
                            std::string_view(current_file.data() + pos, 1));
        Advance();
      }
      continue;
    }

    if (c == ',')
    {
      tokens.emplace_back(TokenType::Comma, pos,
                          std::string_view(current_file.data() + pos, 1));
      Advance();
      continue;
    }

    if (c == '-' && Peek(1) == '>')
    {
      tokens.emplace_back(TokenType::Arrow, pos,
                          std::string_view(current_file.data() + pos, 2));
      Advance();
      Advance();
      continue;
    }

    if (c == '.' && Peek(1) == '.' && Peek(2) == '.')
    {
      tokens.emplace_back(TokenType::Ellipsis, pos,
                          std::string_view(current_file.data() + pos, 3));
      Advance();
      Advance();
      Advance();
      continue;
    }

    if (c == '.')
    {
      tokens.emplace_back(TokenType::Dot, pos,
                          std::string_view(current_file.data() + pos, 1));
      Advance();
      continue;
    }

    if (c == '*')
    {
      tokens.emplace_back(TokenType::Star, pos,
                          std::string_view(current_file.data() + pos, 1));
      Advance();
      continue;
    }

    if (c == '&')
    {
      tokens.emplace_back(TokenType::Ampersand, pos,
                          std::string_view(current_file.data() + pos, 1));
      Advance();
      continue;
    }

    if (c == '/')
    {
      if (Peek(1) == '/')
      {
        // Single-line comment
        while (Peek() != '\n' && Peek() != '\0')
        {
          Advance();
        }
        continue;
      }
      else if (Peek(1) == '*')
      {
        // Multi-line comment
        Advance(); // consume /
        Advance(); // consume *
        while (!(Peek() == '*' && Peek(1) == '/') && Peek() != '\0')
        {
          Advance();
        }
        if (Peek() != '\0')
        {
          Advance(); // consume *
          Advance(); // consume /
        }
        continue;
      }
      else
      {
        tokens.emplace_back(TokenType::Operator, pos,
                            std::string_view(current_file.data() + pos, 1));
        Advance();
        continue;
      }
    }

    // + - % (operators, but * is handled separately for pointers, / handled
    // above)
    if (isOperator(c))
    {
      tokens.emplace_back(TokenType::Operator, pos,
                          std::string_view(current_file.data() + pos, 1));
      Advance();
      continue;
    }

    if (isAlpha(c))
    {
      size_t start = pos;
      while (isAlpha(Peek()) || isdigit(Peek()))
        Advance();

      std::string_view id(current_file.data() + start, pos - start);

      if (isKeyword(id))
      {
        tokens.emplace_back(getTokenType(id), start, id);
      }
      else
      {
        tokens.emplace_back(TokenType::Id, start, id);
      }
      continue;
    }
    if (isdigit(Peek()))
    {
      size_t start = pos;

      while (isdigit(Peek()))
      {
        Advance();
      }
      std::string_view num(current_file.data() + start, pos - start);
      tokens.emplace_back(TokenType::ConstInt, start, num);
      continue;
    }

    if (Peek() == '"')
    {
      size_t start = pos;
      Advance(); // consume opening quote
      while (Peek() != '"' && Peek() != '\0')
      {
        Advance();
      }
      if (Peek() == '"')
      {
        size_t length = pos - start - 1; // exclude quotes
        std::string_view strVal(current_file.data() + start + 1, length);
        tokens.emplace_back(TokenType::ConstString, start, strVal);
        Advance(); // consume closing quote
      }
      else
      {
        // Unterminated string literal
        std::cerr << "Unterminated string literal at pos " << start << "\n";
        exit(1);
      }
      continue;
    }

    if (pos >= current_file.size())
    {
      std::cout << "BREAK\n";
      break;
    }
    std::cerr << "Unknown character: " << c << " at pos " << pos << "\n";
    Advance();
  }
  tokens.emplace_back(TokenType::EOF_TOKEN, pos,
                      std::string_view(current_file.data() + pos, 1));
  return tokens;
}

char Lexer::Peek(int offset)
{
  if (pos + offset < current_file.size())
    return current_file[pos + offset];
  return '\0';
}

char Lexer::Consume()
{
  char c = Peek();
  if (c != '\0')
    pos++;
  return c;
}

void Lexer::Advance()
{
  if (pos < current_file.size())
  {
    pos++;
  }
  else
  {
    std::cerr << "cannot advance!\n";
    exit(1);
  }
}