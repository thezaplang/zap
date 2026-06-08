#pragma once
#include "../token/token.hpp"
#include "stream.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace zap {

enum class DiagnosticLevel { Note, Warning, Error };

struct DiagnosticPosition {
  size_t line = 0;
  size_t column = 0; // 1-based, logical (character-based) column
  size_t offset = 0; // byte offset in source text
};

struct DiagnosticRange {
  DiagnosticPosition start;
  DiagnosticPosition end;
};

struct Diagnostic {
  DiagnosticLevel level;
  std::string code;
  std::string message;
  std::string fileName;
  SourceSpan span;
  DiagnosticRange range;
};

namespace detail {

inline size_t tabWidth() { return 4; }

inline bool isUtf8ContinuationByte(unsigned char c) {
  return (c & 0xC0) == 0x80;
}

inline size_t utf8CodePointLength(unsigned char lead) {
  if ((lead & 0x80) == 0x00)
    return 1;
  if ((lead & 0xE0) == 0xC0)
    return 2;
  if ((lead & 0xF0) == 0xE0)
    return 3;
  if ((lead & 0xF8) == 0xF0)
    return 4;
  return 1; // invalid lead byte fallback
}

inline uint32_t decodeUtf8At(const std::string &s, size_t i, size_t *lenOut) {
  if (i >= s.size()) {
    if (lenOut)
      *lenOut = 0;
    return 0;
  }

  const unsigned char lead = static_cast<unsigned char>(s[i]);
  const size_t len = std::min(utf8CodePointLength(lead), s.size() - i);

  if (len == 1) {
    if (lenOut)
      *lenOut = 1;
    return lead;
  }

  // Validate continuation bytes; if invalid, treat as single-byte char.
  for (size_t j = 1; j < len; ++j) {
    if (!isUtf8ContinuationByte(static_cast<unsigned char>(s[i + j]))) {
      if (lenOut)
        *lenOut = 1;
      return lead;
    }
  }

  uint32_t cp = 0;
  if (len == 2) {
    cp = (lead & 0x1F);
  } else if (len == 3) {
    cp = (lead & 0x0F);
  } else {
    cp = (lead & 0x07);
  }

  for (size_t j = 1; j < len; ++j) {
    cp = (cp << 6) | (static_cast<unsigned char>(s[i + j]) & 0x3F);
  }

  if (lenOut)
    *lenOut = len;
  return cp;
}

// A practical display-width heuristic:
// - tabs: to next tab stop
// - control chars: width 1
// - combining marks: width 0
// - common wide emoji / East-Asian wide ranges: width 2
// - otherwise width 1
inline bool isCombining(uint32_t cp) {
  return (cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x1AB0 && cp <= 0x1AFF) ||
         (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x20D0 && cp <= 0x20FF) ||
         (cp >= 0xFE20 && cp <= 0xFE2F);
}

inline bool isWide(uint32_t cp) {
  return (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2329 && cp <= 0x232A) ||
         (cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7A3) ||
         (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE10 && cp <= 0xFE19) ||
         (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
         (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1FAFF);
}

inline size_t displayWidthOfCodePoint(uint32_t cp, size_t currentColumn) {
  if (cp == '\t') {
    const size_t t = tabWidth();
    const size_t rem = currentColumn % t;
    return rem == 0 ? t : (t - rem);
  }
  if (cp < 0x20 || cp == 0x7F)
    return 1;
  if (isCombining(cp))
    return 0;
  if (isWide(cp))
    return 2;
  return 1;
}

inline size_t findLineStartByOffset(const std::string &source, size_t offset) {
  const size_t start = std::min(offset, source.size());
  size_t i = start;
  while (i > 0 && source[i - 1] != '\n')
    --i;
  return i;
}

inline size_t findLineStartByLine(const std::string &source, size_t line1) {
  if (line1 <= 1)
    return 0;
  size_t line = 1;
  size_t i = 0;
  size_t lineStart = 0;
  while (i < source.size() && line < line1) {
    if (source[i] == '\n') {
      ++line;
      lineStart = i + 1;
    }
    ++i;
  }
  return lineStart;
}

inline size_t findLineEnd(const std::string &source, size_t lineStart) {
  size_t i = std::min(lineStart, source.size());
  while (i < source.size() && source[i] != '\n')
    ++i;
  return i;
}

inline size_t clampToCodePointBoundary(const std::string &s, size_t pos) {
  size_t p = std::min(pos, s.size());
  while (p > 0 && p < s.size() &&
         isUtf8ContinuationByte(static_cast<unsigned char>(s[p]))) {
    --p;
  }
  return p;
}

inline size_t visualColumnsForBytes(const std::string &line, size_t bytesUpto) {
  size_t i = 0;
  size_t col = 0;
  const size_t end = std::min(bytesUpto, line.size());

  while (i < end) {
    size_t len = 0;
    uint32_t cp = decodeUtf8At(line, i, &len);
    if (len == 0)
      break;

    // Prevent stepping beyond requested byte count.
    if (i + len > end)
      break;

    col += displayWidthOfCodePoint(cp, col);
    i += len;
  }

  return col;
}

inline std::string expandTabsForDisplay(const std::string &line) {
  std::string out;
  out.reserve(line.size());

  size_t i = 0;
  size_t col = 0;
  while (i < line.size()) {
    size_t len = 0;
    uint32_t cp = decodeUtf8At(line, i, &len);
    if (len == 0)
      break;

    if (cp == '\t') {
      const size_t spaces = displayWidthOfCodePoint(cp, col);
      out.append(spaces, ' ');
      col += spaces;
      i += len;
      continue;
    }

    out.append(line, i, len);
    col += displayWidthOfCodePoint(cp, col);
    i += len;
  }

  return out;
}

inline std::pair<size_t, size_t>
computeUnderlineColumns(const SourceSpan &span, const std::string &lineContent,
                        size_t lineStartOffset) {
  size_t startByteInLine = 0;
  if (span.offset >= lineStartOffset) {
    startByteInLine = span.offset - lineStartOffset;
  } else if (span.column > 0) {
    // Approx fallback when offset is unavailable: best-effort by characters.
    startByteInLine = std::min(span.column - 1, lineContent.size());
  }
  startByteInLine = clampToCodePointBoundary(lineContent, startByteInLine);
  startByteInLine = std::min(startByteInLine, lineContent.size());

  size_t endByteInLine = startByteInLine;
  if (span.length > 0) {
    size_t desired = startByteInLine + span.length;
    endByteInLine = clampToCodePointBoundary(
        lineContent, std::min(desired, lineContent.size()));
  } else {
    // zero-length span: point to the next visible cell
    endByteInLine = startByteInLine;
  }

  size_t startCol = visualColumnsForBytes(lineContent, startByteInLine);
  size_t endCol = visualColumnsForBytes(lineContent, endByteInLine);

  size_t width = (endCol > startCol) ? (endCol - startCol) : 1;
  return {startCol, width};
}

inline std::string levelToText(DiagnosticLevel level) {
  switch (level) {
  case DiagnosticLevel::Note:
    return "note";
  case DiagnosticLevel::Warning:
    return "warning";
  case DiagnosticLevel::Error:
    return "error";
  }
  return "unknown";
}

inline Color levelToColor(DiagnosticLevel level) {
  switch (level) {
  case DiagnosticLevel::Note:
    return Color::BLUE;
  case DiagnosticLevel::Warning:
    return Color::YELLOW;
  case DiagnosticLevel::Error:
    return Color::RED;
  }
  return Color::WHITE;
}

} // namespace detail

class DiagnosticTextFormatter {
public:
  static void print(Stream &os, const std::vector<Diagnostic> &diagnostics,
                    const std::string &source) {
    for (const auto &diagnostic : diagnostics) {
      printDiagnostic(os, diagnostic, source);
    }
  }

  static void print(std::ostream &os,
                    const std::vector<Diagnostic> &diagnostics,
                    const std::string &source) {
    for (const auto &diagnostic : diagnostics) {
      printDiagnostic(os, diagnostic, source);
    }
  }

private:
  static void printDiagnostic(Stream &os, const Diagnostic &diagnostic,
                              const std::string &source) {
    os.changeColor(detail::levelToColor(diagnostic.level), true);
    os << detail::levelToText(diagnostic.level);
    os.resetColor();
    os << ": ";
    if (!diagnostic.code.empty()) {
      os << diagnostic.code << " ";
    }
    os << diagnostic.message << '\n';
    os << " --> " << diagnostic.fileName << ":" << diagnostic.range.start.line
       << ":" << diagnostic.range.start.column << '\n';
    printContext(os, diagnostic, source);
  }

  static void printDiagnostic(std::ostream &os, const Diagnostic &diagnostic,
                              const std::string &source) {
    os << detail::levelToText(diagnostic.level) << ": ";
    if (!diagnostic.code.empty()) {
      os << diagnostic.code << " ";
    }
    os << diagnostic.message << '\n';
    os << " --> " << diagnostic.fileName << ":" << diagnostic.range.start.line
       << ":" << diagnostic.range.start.column << '\n';
    printContext(os, diagnostic, source);
  }

  static void printContext(Stream &os, const Diagnostic &diagnostic,
                           const std::string &source) {
    SourceSpan span = diagnostic.span;

    size_t lineStart = 0;
    if (span.offset <= source.size()) {
      lineStart = detail::findLineStartByOffset(source, span.offset);
    } else {
      lineStart = detail::findLineStartByLine(source, span.line);
    }
    const size_t lineEnd = detail::findLineEnd(source, lineStart);
    const std::string lineContent =
        source.substr(lineStart, lineEnd - lineStart);
    const std::string renderedLine = detail::expandTabsForDisplay(lineContent);

    const std::string lineNumStr = std::to_string(span.line);
    os << " " << lineNumStr << " | " << renderedLine << "\n";

    const size_t prefixLen = lineNumStr.length() + 4;
    for (size_t i = 0; i < prefixLen; ++i)
      os << ' ';

    auto [startCol, width] =
        detail::computeUnderlineColumns(span, lineContent, lineStart);

    for (size_t i = 0; i < startCol; ++i)
      os << ' ';

    os.changeColor(detail::levelToColor(diagnostic.level), true);

    const size_t MAX_UNDERLINE = 80;
    if (width > MAX_UNDERLINE) {
      const size_t half = MAX_UNDERLINE / 2;
      for (size_t i = 0; i < half; ++i)
        os << '^';
      os << "...";
      for (size_t i = 0; i < half; ++i)
        os << '^';
    } else {
      for (size_t i = 0; i < width; ++i)
        os << '^';
    }

    os.resetColor();
    os << '\n';
  }

  static void printContext(std::ostream &os, const Diagnostic &diagnostic,
                           const std::string &source) {
    SourceSpan span = diagnostic.span;

    size_t lineStart = 0;
    if (span.offset <= source.size()) {
      lineStart = detail::findLineStartByOffset(source, span.offset);
    } else {
      lineStart = detail::findLineStartByLine(source, span.line);
    }
    const size_t lineEnd = detail::findLineEnd(source, lineStart);
    const std::string lineContent =
        source.substr(lineStart, lineEnd - lineStart);
    const std::string renderedLine = detail::expandTabsForDisplay(lineContent);

    const std::string lineNumStr = std::to_string(span.line);
    os << " " << lineNumStr << " | " << renderedLine << "\n";

    const size_t prefixLen = lineNumStr.length() + 4;
    for (size_t i = 0; i < prefixLen; ++i)
      os << ' ';

    auto [startCol, width] =
        detail::computeUnderlineColumns(span, lineContent, lineStart);

    for (size_t i = 0; i < startCol; ++i)
      os << ' ';
    for (size_t i = 0; i < width; ++i)
      os << '^';

    os << '\n';
  }
};

class DiagnosticEngine {
private:
  const std::string &source;
  std::string fileName;
  std::vector<Diagnostic> diagnostics_;
  size_t errorCount = 0;
  bool errorCapReached_ = false;

  static constexpr size_t MAX_ERRORS = 50;
  static constexpr size_t MAX_DIAGNOSTICS = 200;

public:
  DiagnosticEngine(const std::string &src, const std::string &fname = "input")
      : source(src), fileName(fname) {}

  void report(SourceSpan span, DiagnosticLevel level,
              const std::string &message) {
    report(span, level, defaultCodeFor(level, message), message);
  }

  void report(SourceSpan span, DiagnosticLevel level, const std::string &code,
              const std::string &message) {
    // Deduplicate identical diagnostics (same level, message and span).
    for (const auto &existing : diagnostics_) {
      if (existing.level == level && existing.message == message &&
          existing.span.line == span.line &&
          existing.span.column == span.column &&
          existing.span.offset == span.offset &&
          existing.span.length == span.length) {
        return;
      }
    }

    // Stop flooding on errors, but emit one explanatory note once.
    if (level == DiagnosticLevel::Error && errorCount >= MAX_ERRORS) {
      if (!errorCapReached_) {
        errorCapReached_ = true;
        const SourceSpan capSpan = span;
        diagnostics_.push_back(Diagnostic{
            DiagnosticLevel::Note, "N0001",
            "Too many errors emitted; stopping after " +
                std::to_string(MAX_ERRORS) +
                " errors. Fix the first errors and re-run for more details.",
            fileName, capSpan, makeRange(capSpan)});
      }
      return;
    }

    if (diagnostics_.size() >= MAX_DIAGNOSTICS) {
      return;
    }

    diagnostics_.push_back(
        Diagnostic{level, code, message, fileName, span, makeRange(span)});

    if (level == DiagnosticLevel::Error) {
      ++errorCount;
      maybeAddHelpHint(span, message);
    }
  }

  bool hadErrors() const { return errorCount > 0; }

  bool empty() const { return diagnostics_.empty(); }

  const std::vector<Diagnostic> &diagnostics() const { return diagnostics_; }

  const std::string &sourceText() const { return source; }

  const std::string &sourceName() const { return fileName; }

  void printText(std::ostream &os) const {
    DiagnosticTextFormatter::print(os, diagnostics_, source);
  }

  void printText(Stream &os) const {
    DiagnosticTextFormatter::print(os, diagnostics_, source);
  }

  std::string toJson() const {
    std::string json = "[";
    for (size_t i = 0; i < diagnostics_.size(); ++i) {
      if (i > 0)
        json += ',';

      const auto &d = diagnostics_[i];
      json += "{";
      appendJsonField(json, "level", levelToString(d.level));
      json += ",";
      appendJsonField(json, "message", d.message);
      json += ",";
      appendJsonField(json, "code", d.code);
      json += ",";
      appendJsonField(json, "fileName", d.fileName);
      json += ",\"range\":{";
      json += "\"start\":";
      appendJsonPosition(json, d.range.start);
      json += ",\"end\":";
      appendJsonPosition(json, d.range.end);
      json += "}}";
    }
    json += "]";
    return json;
  }

private:
  void addNote(SourceSpan span, const std::string &message) {
    for (const auto &existing : diagnostics_) {
      if (existing.level == DiagnosticLevel::Note &&
          existing.message == message && existing.span.line == span.line &&
          existing.span.column == span.column &&
          existing.span.offset == span.offset &&
          existing.span.length == span.length) {
        return;
      }
    }

    if (diagnostics_.size() >= MAX_DIAGNOSTICS) {
      return;
    }

    diagnostics_.push_back(Diagnostic{
        DiagnosticLevel::Note, defaultCodeFor(DiagnosticLevel::Note, message),
        message, fileName, span, makeRange(span)});
  }

  void maybeAddHelpHint(SourceSpan span, const std::string &message) {
    if (message.find("Expected ';'") != std::string::npos) {
      addNote(span, "help: add ';' at the end of the statement.");
      return;
    }

    if (message.find("Expected primary expression") != std::string::npos) {
      addNote(
          span,
          "help: insert an expression (identifier, literal, or call) here.");
      return;
    }

    if (message.find("Undefined identifier") != std::string::npos) {
      addNote(span,
              "help: check spelling or declare the identifier before use.");
      return;
    }

    if (message.find("Cannot assign expression of type") != std::string::npos) {
      addNote(span, "help: change the variable type or cast/convert the "
                    "assigned expression.");
      return;
    }

    if (message.find("If condition must be Bool") != std::string::npos ||
        message.find("While condition must be Bool") != std::string::npos) {
      addNote(span,
              "help: use a Bool expression, e.g. comparison like 'x != 0'.");
      return;
    }

    if (message.find("Cannot compare") != std::string::npos) {
      addNote(
          span,
          "help: ensure both operands are comparable and of compatible types.");
      return;
    }
  }

  static std::string defaultCodeFor(DiagnosticLevel level,
                                    const std::string &message) {
    if (level == DiagnosticLevel::Warning) {
      if (message.find("has non-void return type but no return") !=
          std::string::npos) {
        return "W1001";
      }
      return "W0000";
    }
    if (level == DiagnosticLevel::Note) {
      if (message.find("help:") == 0) {
        return "N1000";
      }
      if (message.find("Too many errors emitted; stopping after") !=
          std::string::npos) {
        return "N1001";
      }
      return "N0000";
    }

    // More specific semantic diagnostics must be checked before generic parser
    // "Expected ... but got ..." classification.
    if (message.find("Undefined identifier") != std::string::npos) {
      return "S2001";
    }
    if (message.find("Cannot assign expression of type") != std::string::npos ||
        message.find("Cannot assign type") != std::string::npos) {
      return "S2002";
    }
    if (message.find("If condition must be Bool") != std::string::npos ||
        message.find("While condition must be Bool") != std::string::npos ||
        message.find("Ternary condition must be Bool") != std::string::npos) {
      return "S2003";
    }
    if (message.find("Cannot compare") != std::string::npos) {
      return "S2004";
    }
    if (message.find("Operator '") != std::string::npos &&
        message.find("cannot be applied") != std::string::npos) {
      return "S2005";
    }
    if (message.find("Cannot cast from '") != std::string::npos) {
      return "S2006";
    }
    if (message.find("does not support indexing") != std::string::npos) {
      return "S2007";
    }
    if (message.find("Array index must be an integer") != std::string::npos) {
      return "S2008";
    }
    if (message.find("Array elements must have the same type") !=
        std::string::npos) {
      return "S2009";
    }
    if (message.find("Cannot dereference non-pointer type") !=
        std::string::npos) {
      return "S2010";
    }
    if (message.find("Weak references cannot") != std::string::npos) {
      return "S2011";
    }
    if (message.find("No matching overload for function") !=
        std::string::npos) {
      return "S2012";
    }
    if (message.find("Call to function '") != std::string::npos &&
        message.find("' is ambiguous between ") != std::string::npos) {
      return "S2013";
    }

    if (message.find("Expected ';'") != std::string::npos) {
      return "P1001";
    }
    if (message.find("Expected primary expression") != std::string::npos) {
      return "P1002";
    }
    if (message.find("Unexpected token") != std::string::npos) {
      return "P1003";
    }
    if (message.find("Expected ") != std::string::npos &&
        message.find(" but got ") != std::string::npos) {
      return "P1004";
    }
    if (message.find("reached end of file") != std::string::npos) {
      return "P1005";
    }

    return "E0000";
  }

  static std::string levelToString(DiagnosticLevel level) {
    switch (level) {
    case DiagnosticLevel::Note:
      return "note";
    case DiagnosticLevel::Warning:
      return "warning";
    case DiagnosticLevel::Error:
      return "error";
    }
    return "unknown";
  }

  DiagnosticRange makeRange(SourceSpan span) const {
    DiagnosticPosition start{span.line, span.column, span.offset};

    size_t endOffset = std::min(span.offset, source.size());
    size_t remaining = span.length;

    size_t line = start.line;
    size_t column = start.column;

    while (endOffset < source.size() && remaining > 0) {
      if (source[endOffset] == '\n') {
        ++line;
        column = 1;
      } else {
        ++column;
      }
      ++endOffset;
      --remaining;
    }

    if (span.length == 0) {
      ++column;
    }

    return DiagnosticRange{start, DiagnosticPosition{line, column, endOffset}};
  }

  static void appendJsonEscaped(std::string &json, const std::string &value) {
    json += '"';
    for (char ch : value) {
      switch (ch) {
      case '\\':
        json += "\\\\";
        break;
      case '"':
        json += "\\\"";
        break;
      case '\n':
        json += "\\n";
        break;
      case '\r':
        json += "\\r";
        break;
      case '\t':
        json += "\\t";
        break;
      default:
        json += ch;
        break;
      }
    }
    json += '"';
  }

  static void appendJsonField(std::string &json, const std::string &key,
                              const std::string &value) {
    appendJsonEscaped(json, key);
    json += ":";
    appendJsonEscaped(json, value);
  }

  static void appendJsonPosition(std::string &json,
                                 const DiagnosticPosition &p) {
    json += "{";
    json += "\"line\":";
    json += std::to_string(p.line);
    json += ",\"column\":";
    json += std::to_string(p.column);
    json += ",\"offset\":";
    json += std::to_string(p.offset);
    json += "}";
  }
};

} // namespace zap