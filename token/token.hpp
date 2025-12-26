#pragma once
#include <string>
enum TokenType {
//keywords

IMPORT=1,
FUN=2,
RETURN=3,
IF=4,
ELSE=5,
WHILE=6,
FOR=7,
MATCH=8,
LET=9,
EXTERN=10,
MODULE=11,
PUB=12,
PRIV=13,
STRUCT=14,
IMPL=15,
STATIC=16,
ENUM=17,
SEMICOLON=18,
COLON=19,
DOUBLECOLON=20,
DOT=21,
COMMA=22,
LPAREN=23,
RPAREN=24,
LBRACE=25,
RBRACE=26,
ARRAY=27,
SQUARE_LBRACE=28,
SQUARE_RBRACE=29,
QUESTION=30,
ARROW=31,
LAMBDA=32,
LESSEQUAL=33,
LESS=34,
GREATER=35,
GREATEREQUAL=36,
EQUAL=37,
ASSIGN=38,
PLUS=39,
MINUS=40,
MULTIPLY=41,
REFERENCE=42,
DIVIDE=43,
MODULO=44,
POW=45,
NOT=46,
AND=47,
OR=48,
ID=49,
INTEGER=50,
FLOAT=51,
STRING=52,
CHAR=53,
BOOL=54,
BREAK=55,
CONTINUE=56,
ELLIPSIS=57, // ...
};

class Token {
public:
    unsigned int pos;
    TokenType type;
    std::string value;
    Token(unsigned int position, TokenType tokenType, const std::string& tokenValue)
        : pos(position), type(tokenType), value(tokenValue) {}
    ~Token(){}
};