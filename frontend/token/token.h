//
// Created by Funcieq on 26.11.2025.
//

#ifndef IGNIS_TOKEN_H
#define IGNIS_TOKEN_H
#include <string_view>

enum TokenType {
  KFn,
  KReturn,
  KLet,
  KIF,
  KWhile,
  KStruct,
  KModule, // module keyword for namespaces
  KImport, // import keyword for importing modules
  KExtern, // extern keyword for C interoperability
  Id,
  LParen,
  RParen,
  LBrace,
  RBrace,
  Semi,
  Arrow,
  Colon,
  Comma,
  ConstInt,
  ConstString,
  Operator,
  Assign,
  Dot,
  DoubleColon, //::
  Star,        // * for pointer
  Ampersand,   // & for reference
  Ellipsis,    // ... for varargs
  EOF_TOKEN
};

struct Token {
  TokenType type;
  unsigned long pos;
  std::string_view value;

  Token(TokenType t, unsigned long pos, std::string_view val) {
    this->type = t;
    this->pos = pos;
    this->value = val;
  }
};
#endif // IGNIS_TOKEN_H