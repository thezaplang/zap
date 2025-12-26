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
        if (_cur == '(')
        {
            tokens.push_back(Token(_pos, TokenType::LPAREN, "("));
            ++_pos;
            continue;
        }
        else if (_cur == ')')
        {
            tokens.push_back(Token(_pos, TokenType::RPAREN, ")"));
            ++_pos;
            continue;
        }
        else if (_cur == '{')
        {
            tokens.push_back(Token(_pos, TokenType::LBRACE, "{"));
            ++_pos;
            continue;
        }
        else if (_cur == '}')
        {
            tokens.push_back(Token(_pos, TokenType::RBRACE, "}"));
            ++_pos;
            continue;
        }
        else if (_cur == '[')
        {
            tokens.push_back(Token(_pos, TokenType::SQUARE_LBRACE, "["));
            ++_pos;
            continue;
        }
        else if (_cur == ']')
        {
            tokens.push_back(Token(_pos, TokenType::SQUARE_RBRACE, "]"));
            ++_pos;
            continue;
        }
        else if (_cur == ';')
        {
            tokens.push_back(Token(_pos, TokenType::SEMICOLON, ";"));
            ++_pos;
            continue;
        }
        else if (_cur == ',')
        {
            tokens.push_back(Token(_pos, TokenType::COMMA, ","));
            ++_pos;
            continue;
        }
        else if (_cur == ':')
        {
            if (Peek2() == ':')
            {
                _pos++;
                tokens.push_back(Token(_pos, TokenType::DOUBLECOLON, "::"));
                ++_pos;
                continue;
            }
            else
            {
                tokens.push_back(Token(_pos, TokenType::COLON, ":"));
                ++_pos;
                continue;
            }
        }
        else if (_cur == '.')
        {
            if (Peek2() == '.' && Peek3() == '.')
            {
                _pos += 2;
                tokens.push_back(Token(_pos, TokenType::ELLIPSIS, "..."));
                ++_pos;
                continue;
            }
            else
            {
                tokens.push_back(Token(_pos, TokenType::DOT, "."));
                ++_pos;
                continue;
            }
            tokens.push_back(Token(_pos, TokenType::DOT, "."));
            ++_pos;
            continue;
        }
        else if (_cur == '?')
        {
            tokens.push_back(Token(_pos, TokenType::QUESTION, "?"));
            ++_pos;
            continue;
        }
        else if (_cur == '+')
        {
            tokens.push_back(Token(_pos, TokenType::PLUS, "+"));
            ++_pos;
            continue;
        }
        else if (_cur == '*')
        {
            tokens.push_back(Token(_pos, TokenType::MULTIPLY, "*"));
            ++_pos;
            continue;
        }
        else if (_cur == '-')
        {
            tokens.push_back(Token(_pos, TokenType::MINUS, "-"));
            ++_pos;
            continue;
        }
        else if (_cur == '/')
        {
            tokens.push_back(Token(_pos, TokenType::DIVIDE, "/"));
            ++_pos;
            continue;
        }
        else if (_cur == '%')
        {
            tokens.push_back(Token(_pos, TokenType::MODULO, "%"));
            ++_pos;
            continue;
        }
        else if (_cur == '^')
        {
            tokens.push_back(Token(_pos, TokenType::POW, "^"));
            ++_pos;
            continue;
        }
        else if (_cur == '&')
        {
            if (Peek2() == '&')
            {
                _pos++;
                tokens.push_back(Token(_pos, TokenType::AND, "&&"));
            }
            else
            {
                tokens.push_back(Token(_pos, TokenType::REFERENCE, "&"));
            }
        }
        else if (_cur == '|')
        {
            if (Peek2() == '|')
            {
                _pos++;
                tokens.push_back(Token(_pos, TokenType::OR, "||"));
                ++_pos;
                continue;
            }
            else
            {
                tokens.push_back(Token(_pos, TokenType::OR, "|"));
                ++_pos;
                continue;
            }
        }
        else if (_cur == '=')
        {
            if (Peek2() == '=')
            {
                _pos++;
                tokens.push_back(Token(_pos, TokenType::EQUAL, "=="));
            }
            else
            {
                tokens.push_back(Token(_pos, TokenType::ASSIGN, "="));
            }
        }
        else if (_cur == '!')
        {
            if (Peek2() == '=')
            {
                _pos++;
                tokens.push_back(Token(_pos, TokenType::LESSEQUAL, "!="));
            }
            else
            {
                tokens.push_back(Token(_pos, TokenType::NOT, "!"));
            }
        }
        else if (_cur == '<')
        {
            if (Peek2() == '=')
            {
                _pos++;
                tokens.push_back(Token(_pos, TokenType::LESSEQUAL, "<="));
            }
            else
            {
                tokens.push_back(Token(_pos, TokenType::LESS, "<"));
            }
        }
        else if (_cur == '>')
        {
            if (Peek2() == '=')
            {
                _pos++;
                tokens.push_back(Token(_pos, TokenType::GREATEREQUAL, ">="));
            }
            else
            {
                tokens.push_back(Token(_pos, TokenType::GREATER, ">"));
            }
        }
        else if (std::isdigit(_cur))
        {
            std::string numStr;
            bool isFloat = false;
            unsigned int startPos = _pos;
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
                tokens.push_back(Token(startPos, TokenType::FLOAT, numStr));
            }
            else
            {
                tokens.push_back(Token(startPos, TokenType::INTEGER, numStr));
            }
            // Don't increment _pos here, main loop will handle next char
            continue;
        }
        else if (std::isalpha(_cur) || _cur == '_')
        {
            std::string identStr;
            unsigned int startPos = _pos;
            while (!isAtEnd() && (std::isalnum(_input[_pos]) || _input[_pos] == '_'))
            {
                identStr += _input[_pos++];
            }
            // Check for keywords
            if (identStr == "if")
            {
                tokens.push_back(Token(startPos, TokenType::IF, identStr));
            }
            else if (identStr == "else")
            {
                tokens.push_back(Token(startPos, TokenType::ELSE, identStr));
            }
            else if (identStr == "while")
            {
                tokens.push_back(Token(startPos, TokenType::WHILE, identStr));
            }
            else if (identStr == "for")
            {
                tokens.push_back(Token(startPos, TokenType::FOR, identStr));
            }
            else if (identStr == "return" || identStr == "ret")
            {
                tokens.push_back(Token(startPos, TokenType::RETURN, identStr));
            }
            else if (identStr == "true")
            {
                tokens.push_back(Token(startPos, TokenType::BOOL, identStr));
            }
            else if (identStr == "false")
            {
                tokens.push_back(Token(startPos, TokenType::BOOL, identStr));
            }
            else if (identStr == "fun")
            {
                tokens.push_back(Token(startPos, TokenType::FUN, identStr));
            }
            else if (identStr == "import")
            {
                tokens.push_back(Token(startPos, TokenType::IMPORT, identStr));
            }
            else if (identStr == "match")
            {
                tokens.push_back(Token(startPos, TokenType::MATCH, identStr));
            }
            else if (identStr == "let")
            {
                tokens.push_back(Token(startPos, TokenType::LET, identStr));
            }
            else if (identStr == "ext")
            {
                tokens.push_back(Token(startPos, TokenType::EXTERN, identStr));
            }
            else if (identStr == "module")
            {
                tokens.push_back(Token(startPos, TokenType::MODULE, identStr));
            }
            else if (identStr == "pub")
            {
                tokens.push_back(Token(startPos, TokenType::PUB, identStr));
            }
            else if (identStr == "priv")
            {
                tokens.push_back(Token(startPos, TokenType::PRIV, identStr));
            }
            else if (identStr == "struct")
            {
                tokens.push_back(Token(startPos, TokenType::STRUCT, identStr));
            }
            else if (identStr == "impl")
            {
                tokens.push_back(Token(startPos, TokenType::IMPL, identStr));
            }
            else if (identStr == "static")
            {
                tokens.push_back(Token(startPos, TokenType::STATIC, identStr));
            }
            else if (identStr == "enum")
            {
                tokens.push_back(Token(startPos, TokenType::ENUM, identStr));
            }
            else if (identStr == "break")
            {
                tokens.push_back(Token(startPos, TokenType::BREAK, identStr));
            }
            else if (identStr == "continue")
            {
                tokens.push_back(Token(startPos, TokenType::CONTINUE, identStr));
            }
            else
            {
                tokens.push_back(Token(startPos, TokenType::ID, identStr));
            }
            // Don't increment _pos here, main loop will handle next char
            continue;
        }
        else if (std::isspace(_cur))
        {
            _pos++;
            continue;
        }
        else if (_cur == '"')
        {
            std::string strVal;
            unsigned int startPos = _pos;
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
                        break; // Unknown escape, treat literally
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
                tokens.push_back(Token(startPos, TokenType::STRING, strVal));
                continue;
            }
            else
            {
                printf("Lexer Error: Unterminated string at position %u\n", startPos);
                exit(EXIT_FAILURE);
            }
        }
        else
        {
            printf("Lexer Error: Unexpected character '%c' at position %u\n", _cur, _pos);
            exit(EXIT_FAILURE);
        }
        ++_pos;
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

bool Lexer::isAtEnd() const
{
    return _pos >= _input.size();
}
