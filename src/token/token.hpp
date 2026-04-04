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
  STRUCT = 18, ///< "struct" keyword.
  SEMICOLON = 19, ///< ';' symbol.
  COLON = 20, ///< ':' symbol.
  DOUBLECOLON = 21, ///< '::' symbol.
  DOT = 22, ///< '.' symbol.
  COMMA = 23, ///< ',' symbol.
  LPAREN = 24, ///< '(' symbol.
  RPAREN = 25, ///< ')' symbol.
  LBRACE = 26, ///< '{' symbol.
  RBRACE = 27, ///< '}' symbol.
  ARRAY = 28, ///< ???
  SQUARE_LBRACE = 29, ///< '[' symbol.
  SQUARE_RBRACE = 30, ///< ']' symbol.
  QUESTION = 31, ///< ';' symbol.
  ARROW = 32, ///< '->' symbol.
  LAMBDA = 33, ///< ? '=>' symbol ?
  LESSEQUAL = 34, ///< '<=' symbol.
  LESS = 35, ///< '<' symbol.
  GREATER = 36, ///< '>' symbol.
  GREATEREQUAL = 37, ///< '>=' symbol.
  EQUAL = 38, ///< '==' symbol.
  ASSIGN = 39, ///< '=' symbol.
  PLUS = 40, ///< '+' symbol.
  MINUS = 41, ///< '-' symbol.
  MULTIPLY = 42, ///< '*' symbol.
  REFERENCE = 43, ///< '&' symbol.
  DIVIDE = 44, ///< '/' symbol.
  MODULO = 45, ///< '%' symbol.
  POW = 46, ///< '^' symbol.
  NOT = 47, ///< '!' symbol.
  AND = 48, ///< '&&' symbol.
  OR = 49, ///< '||' symbol.
  ID = 50, ///< Identifier.
  INTEGER = 51, ///< Integer.
  FLOAT = 52, ///< Floating point.
  STRING = 53, ///< String.
  CHAR = 54, ///< Char.
  BOOL = 55, ///< True or false.
  BREAK = 56, ///< "break" keyword.
  CONTINUE = 57, ///< "continue" keyword.
  ELLIPSIS = 58, ///< '...' symbol.
  NOTEQUAL = 59, ///< '!=' symbol.
  CONCAT = 60, ///< '~' symbol.
  BIT_OR = 61, ///< '|' symbol.
  ALIAS = 62, ///< "alias" keyword.
  GLOBAL,
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
    case TokenType::GLOBAL: return "global";
    case TokenType::CONST: return "const";
    case TokenType::ALIAS: return "alias";
    case TokenType::CONCAT: return "~";
    default: return "unknown token";
  }
}