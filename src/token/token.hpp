#pragma once
#include <string>
#include <utility>

/// @brief Determines the type the token will have.
enum TokenType {

  IMPORT = 1,         ///< "import" keyword.
  FUN = 2,            ///< "fun" (Function) keyword.
  RETURN = 3,         ///< "return" or "ret" keyword.
  IF = 4,             ///< "if" keyword.
  ELSE = 5,           ///< "else" keyword.
  WHILE = 6,          ///< "while" keyword.
  FOR = 7,            ///< "for" keyword.
  MATCH = 8,          ///< "match" keyword.
  VAR = 9,            ///< "var" keyword.
  EXTERN = 10,        ///< "ext" (Extern) keyword.
  MODULE = 11,        ///< "module" keyword.
  PUB = 12,           ///< "pub" (Public) keyword.
  PRIV = 13,          ///< "priv" (Private) keyword.
  RECORD = 14,        ///< "record" keyword.
  IMPL = 15,          ///< "impl" keyword.
  STATIC = 16,        ///< "static" keyword.
  ENUM = 17,          ///< "enum" keyword.
  STRUCT = 18,        ///< "struct" keyword.
  SEMICOLON = 19,     ///< ';' symbol.
  COLON = 20,         ///< ':' symbol.
  DOUBLECOLON = 21,   ///< '::' symbol.
  DOT = 22,           ///< '.' symbol.
  COMMA = 23,         ///< ',' symbol.
  LPAREN = 24,        ///< '(' symbol.
  RPAREN = 25,        ///< ')' symbol.
  LBRACE = 26,        ///< '{' symbol.
  RBRACE = 27,        ///< '}' symbol.
  ARRAY = 28,         ///< ???
  SQUARE_LBRACE = 29, ///< '[' symbol.
  SQUARE_RBRACE = 30, ///< ']' symbol.
  AT = 31,            ///< '@' symbol.
  QUESTION = 32,      ///< ';' symbol.
  ARROW = 33,         ///< '->' symbol.
  LAMBDA = 34,        ///< ? '=>' symbol ?
  LESSEQUAL = 35,     ///< '<=' symbol.
  LESS = 36,          ///< '<' symbol.
  GREATER = 37,       ///< '>' symbol.
  GREATEREQUAL = 38,  ///< '>=' symbol.
  EQUAL = 39,         ///< '==' symbol.
  ASSIGN = 40,        ///< '=' symbol.
  PLUS = 41,          ///< '+' symbol.
  MINUS = 42,         ///< '-' symbol.
  MULTIPLY = 43,      ///< '*' symbol.
  REFERENCE = 44,     ///< '&' symbol.
  DIVIDE = 45,        ///< '/' symbol.
  MODULO = 46,        ///< '%' symbol.
  POW = 47,           ///< '^' symbol.
  NOT = 48,           ///< '!' symbol.
  AND = 49,           ///< '&&' symbol.
  OR = 50,            ///< '||' symbol.
  ID = 51,            ///< Identifier.
  INTEGER = 52,       ///< Integer.
  FLOAT = 53,         ///< Floating point.
  STRING = 54,        ///< String.
  CHAR = 55,          ///< Char.
  BOOL = 56,          ///< True or false.
  BREAK = 57,         ///< "break" keyword.
  CONTINUE = 58,      ///< "continue" keyword.
  ELLIPSIS = 59,      ///< '...' symbol.
  NOTEQUAL = 60,      ///< '!=' symbol.
  CONCAT = 61,        ///< '~' symbol.
  BIT_OR = 62,        ///< '|' symbol.
  ALIAS = 63,         ///< "alias" keyword.
  REF = 64,           ///< "ref" keyword.
  LSHIFT,             ///< '<<' symbol.
  RSHIFT,             ///< '>>' symbol.
  AS,
  GLOBAL,
  CONST,
  UNSAFE,
  NULL_LITERAL,
  CLASS,
  PROT,
  NEW,
  WEAK,
  WHERE,
  IFTYPE,
  FAIL,
  PLUS_ASSIGN,    ///< '+=' symbol.
  MINUS_ASSIGN,   ///< '-=' symbol.
  STAR_ASSIGN,    ///< '*=' symbol.
  SLASH_ASSIGN,   ///< '/=' symbol.
  PERCENT_ASSIGN, ///< '%=' symbol.
  AMP_ASSIGN,     ///< '&=' symbol.
  PIPE_ASSIGN,    ///< '|=' symbol.
  CARET_ASSIGN,   ///< '^=' symbol.
  LSHIFT_ASSIGN,  ///< '<<=' symbol.
  RSHIFT_ASSIGN,  ///< '>>=' symbol.
  INCREMENT,      ///< '++' symbol.
  DECREMENT,      ///< '--' symbol.
  ASM,            ///< "asm" keyword.
};

/// @brief Contains in-file related information like line, column, offset, and
/// length.
struct SourceSpan {
  size_t line;            ///< Line of the source in the file.
  size_t column;          ///< Column of the source in the file.
  size_t offset;          ///< Offset of the source in the file.
  size_t length;          ///< Length of the source.
  std::string sourceName; ///< Source file this span belongs to.

  /// @brief Basic constructor of the source span.
  /// @param l Line.
  /// @param c Column.
  /// @param o Offset.
  /// @param len Length.
  SourceSpan(size_t l = 0, size_t c = 0, size_t o = 0, size_t len = 0,
             std::string source = "") noexcept
      : line(l), column(c), offset(o), length(len),
        sourceName(std::move(source)) {}

  /// @brief Merges two 'SourceSpan' classes.
  /// @param start From.
  /// @param end To.
  /// @return Merged 'SourceSpan'.
  static SourceSpan merge(const SourceSpan &start,
                          const SourceSpan &end) noexcept {
    size_t newLen = (end.offset + end.length) - start.offset;
    return SourceSpan(start.line, start.column, start.offset, newLen,
                      start.sourceName);
  }
};

class Token {
public:
  SourceSpan span;   ///< Source of the token in the file.
  TokenType type;    ///< Type of the token.
  std::string value; ///< String of the token.

  /// @brief Default constructor of the 'Token' class.
  Token(TokenType type, const std::string &value, SourceSpan span)
      : span(span), type(type), value(value) {}

  /// @brief Helper constructor for when we build span component-wise.
  Token(TokenType type, const std::string &value, size_t line, size_t column,
        size_t offset, size_t length)
      : span(line, column, offset, length), type(type), value(value) {}

  ~Token() noexcept = default;
};

/// @brief Turns the provided 'TokenType' to a string.
/// @param type The provided type.
/// @return String version of the token type.
inline std::string tokenTypeToString(TokenType type) {
  switch (type) {
  case TokenType::IMPORT:
    return "import";
  case TokenType::FUN:
    return "fun";
  case TokenType::RETURN:
    return "return";
  case TokenType::IF:
    return "if";
  case TokenType::ELSE:
    return "else";
  case TokenType::WHILE:
    return "while";
  case TokenType::FOR:
    return "for";
  case TokenType::VAR:
    return "var";
  case TokenType::RECORD:
    return "record";
  case TokenType::ENUM:
    return "enum";
  case TokenType::SEMICOLON:
    return ";";
  case TokenType::COLON:
    return ":";
  case TokenType::DOUBLECOLON:
    return "::";
  case TokenType::DOT:
    return ".";
  case TokenType::COMMA:
    return ",";
  case TokenType::LPAREN:
    return "(";
  case TokenType::RPAREN:
    return ")";
  case TokenType::LBRACE:
    return "{";
  case TokenType::RBRACE:
    return "}";
  case TokenType::SQUARE_LBRACE:
    return "[";
  case TokenType::SQUARE_RBRACE:
    return "]";
  case TokenType::AT:
    return "@";
  case TokenType::QUESTION:
    return "?";
  case TokenType::ASSIGN:
    return "=";
  case TokenType::EQUAL:
    return "==";
  case TokenType::NOTEQUAL:
    return "!=";
  case TokenType::PLUS:
    return "+";
  case TokenType::MINUS:
    return "-";
  case TokenType::MULTIPLY:
    return "*";
  case TokenType::REFERENCE:
    return "&";
  case TokenType::DIVIDE:
    return "/";
  case TokenType::CONCAT:
    return "~";
  case TokenType::PLUS_ASSIGN:
    return "+=";
  case TokenType::MINUS_ASSIGN:
    return "-=";
  case TokenType::STAR_ASSIGN:
    return "*=";
  case TokenType::SLASH_ASSIGN:
    return "/=";
  case TokenType::PERCENT_ASSIGN:
    return "%=";
  case TokenType::AMP_ASSIGN:
    return "&=";
  case TokenType::PIPE_ASSIGN:
    return "|=";
  case TokenType::CARET_ASSIGN:
    return "^=";
  case TokenType::LSHIFT_ASSIGN:
    return "<<=";
  case TokenType::RSHIFT_ASSIGN:
    return ">>=";
  case TokenType::INCREMENT:
    return "++";
  case TokenType::DECREMENT:
    return "--";
  case TokenType::ID:
    return "identifier";
  case TokenType::INTEGER:
    return "integer literal";
  case TokenType::FLOAT:
    return "float literal";
  case TokenType::STRING:
    return "string literal";
  case TokenType::CHAR:
    return "char literal";
  case TokenType::BOOL:
    return "boolean literal";
  case TokenType::GLOBAL:
    return "global";
  case TokenType::CONST:
    return "const";
  case TokenType::UNSAFE:
    return "unsafe";
  case TokenType::ASM:
    return "asm";
  case TokenType::NULL_LITERAL:
    return "null";
  case TokenType::ALIAS:
    return "alias";
  case TokenType::REF:
    return "ref";
  case TokenType::LSHIFT:
    return "<<";
  case TokenType::RSHIFT:
    return ">>";
  case TokenType::AS:
    return "as";
  case TokenType::CLASS:
    return "class";
  case TokenType::PROT:
    return "prot";
  case TokenType::WHERE:
    return "where";
  case TokenType::IFTYPE:
    return "iftype";
  case TokenType::FAIL:
    return "fail";
  case TokenType::NEW:
    return "new";
  case TokenType::WEAK:
    return "weak";
  default:
    return "unknown token";
  }
}
