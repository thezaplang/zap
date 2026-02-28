#pragma once
#include <string>

/// @brief Determines the type the token will have.
enum TokenType {

  IMPORT = 1, ///< "import" keyword.
  FUN = 2, ///< "fun" (Function) keyword.
  RETURN = 3, ///< "return" or "ret" keyword.
  IF = 4, ///< "if" keyword.
  ELSE = 5, ///< "else" keyword.
  WHILE = 6, ///< "while" keyword.
  FOR = 7, ///< "for" keyword.
  MATCH = 8, ///< "match" keyword.
  VAR = 9, ///< "var" keyword.
  EXTERN = 10, ///< "ext" (Extern) keyword.
  MODULE = 11, ///< "module" keyword.
  PUB = 12, ///< "pub" (Public) keyword.
  PRIV = 13, ///< "priv" (Private) keyword.
  RECORD = 14, ///< "record" keyword.
  IMPL = 15, ///< "impl" keyword.
  STATIC = 16, ///< "static" keyword.
  ENUM = 17, ///< "enum" keyword.
  SEMICOLON = 18, ///< ';' symbol.
  COLON = 19, ///< ':' symbol.
  DOUBLECOLON = 20, ///< '::' symbol.
  DOT = 21, ///< '.' symbol.
  COMMA = 22, ///< ',' symbol.
  LPAREN = 23, ///< '(' symbol.
  RPAREN = 24, ///< ')' symbol.
  LBRACE = 25, ///< '{' symbol.
  RBRACE = 26, ///< '}' symbol.
  ARRAY = 27, ///< ???
  SQUARE_LBRACE = 28, ///< '[' symbol.
  SQUARE_RBRACE = 29, ///< ']' symbol.
  QUESTION = 30, ///< ';' symbol.
  ARROW = 31, ///< '->' symbol.
  LAMBDA = 32, ///< ? '=>' symbol ?
  LESSEQUAL = 33, ///< '<=' symbol.
  LESS = 34, ///< '<' symbol.
  GREATER = 35, ///< '>' symbol.
  GREATEREQUAL = 36, ///< '>=' symbol.
  EQUAL = 37, ///< '==' symbol.
  ASSIGN = 38, ///< '=' symbol.
  PLUS = 39, ///< '+' symbol.
  MINUS = 40, ///< '-' symbol.
  MULTIPLY = 41, ///< '*' symbol.
  REFERENCE = 42, ///< '&' symbol.
  DIVIDE = 43, ///< '/' symbol.
  MODULO = 44, ///< '%' symbol.
  POW = 45, ///< '^' symbol.
  NOT = 46, ///< '!' symbol.
  AND = 47, ///< '&&' symbol.
  OR = 48, ///< '||' symbol.
  ID = 49, ///< Identifier.
  INTEGER = 50, ///< Integer.
  FLOAT = 51, ///< Floating point.
  STRING = 52, ///< String.
  CHAR = 53, ///< Char.
  BOOL = 54, ///< True or false.
  BREAK = 55, ///< "break" keyword.
  CONTINUE = 56, ///< "continue" keyword.
  ELLIPSIS = 57, ///< '...' symbol.
  NOTEQUAL = 58, ///< '!=' symbol.
  CONCAT = 59, ///< '~' symbol.
  BIT_OR = 60, ///< '|' symbol.
  VAL,
  CONST,
};

/// @brief Contains in-file related information like line, column, offset, and length.
struct SourceSpan {
  size_t line; ///< Line of the source in the file.
  size_t column; ///< Column of the source in the file.
  size_t offset; ///< Offset of the source in the file.
  size_t length; ///< Length of the source.

  /// @brief Basic constructor of the source span.
  /// @param l Line.
  /// @param c Column.
  /// @param o Offset.
  /// @param len Length.
  SourceSpan(size_t l = 0, size_t c = 0, size_t o = 0, size_t len = 0) noexcept
      : line(l), column(c), offset(o), length(len) {}

  /// @brief Merges two 'SourceSpan' classes.
  /// @param start From.
  /// @param end To.
  /// @return Merged 'SourceSpan'.
  static SourceSpan merge(const SourceSpan &start, const SourceSpan &end) noexcept 
  {
    size_t newLen = (end.offset + end.length) - start.offset;
    return SourceSpan(start.line, start.column, start.offset, newLen);
  }
};

class Token {
public:
  SourceSpan span; ///< Source of the token in the file.
  TokenType type; ///< Type of the token.
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
inline std::string tokenTypeToString(TokenType type) 
{
  switch (type) {
    case TokenType::IMPORT: return "import";
    case TokenType::FUN: return "fun";
    case TokenType::RETURN: return "return";
    case TokenType::IF: return "if";
    case TokenType::ELSE: return "else";
    case TokenType::WHILE: return "while";
    case TokenType::FOR: return "for";
    case TokenType::VAR: return "var";
    case TokenType::RECORD: return "record";
    case TokenType::ENUM: return "enum";
    case TokenType::SEMICOLON: return ";";
    case TokenType::COLON: return ":";
    case TokenType::DOUBLECOLON: return "::";
    case TokenType::DOT: return ".";
    case TokenType::COMMA: return ",";
    case TokenType::LPAREN: return "(";
    case TokenType::RPAREN: return ")";
    case TokenType::LBRACE: return "{";
    case TokenType::RBRACE: return "}";
    case TokenType::SQUARE_LBRACE: return "[";
    case TokenType::SQUARE_RBRACE: return "]";
    case TokenType::ASSIGN: return "=";
    case TokenType::EQUAL: return "==";
    case TokenType::NOTEQUAL: return "!=";
    case TokenType::PLUS: return "+";
    case TokenType::MINUS: return "-";
    case TokenType::MULTIPLY: return "*";
    case TokenType::DIVIDE: return "/";
    case TokenType::ID: return "identifier";
    case TokenType::INTEGER: return "integer literal";
    case TokenType::FLOAT: return "float literal";
    case TokenType::STRING: return "string literal";
    case TokenType::CHAR: return "char literal";
    case TokenType::BOOL: return "boolean literal";
    case TokenType::VAL: return "val";
    case TokenType::CONST: return "const";
    case TokenType::CONCAT: return "~";
    default: return "unknown token";
  }
}