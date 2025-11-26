#ifndef IGNIS_LEXER_H
#define IGNIS_LEXER_H
#include "../token/token.h"
#include <vector>
#include <string>
class Lexer {
public:
    unsigned long pos;
    std::string current_file;
    Lexer() {}
    std::vector<Token> Tokenize(std::string_view content);

    char Peek();
    char Consume();
    void Advance();
};

#endif //IGNIS_LEXER_H