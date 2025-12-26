#pragma once
#include <string>
#include <vector>
#include "../token/token.hpp"
class Lexer{
    public:
        unsigned int _pos;
        std::string _input;
        Lexer(){

        }
        ~Lexer(){

        }
        std::vector<Token> tokenize(const std::string& input);
        char Peek2();
        char Peek3();
        bool isAtEnd() const;
};