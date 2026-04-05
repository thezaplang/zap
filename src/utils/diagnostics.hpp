#pragma once
#include "../token/token.hpp"
#include "stream.hpp"
#include <ostream>
#include <string>
#include <vector>

namespace zap {

enum class DiagnosticLevel {
  Note,
  Warning,
  Error
};

struct DiagnosticPosition {
  size_t line = 0;
  size_t column = 0;
  size_t offset = 0;
};

struct DiagnosticRange {
  DiagnosticPosition start;
  DiagnosticPosition end;
};

struct Diagnostic {
  DiagnosticLevel level;
  std::string message;
  std::string fileName;
  SourceSpan span;
  DiagnosticRange range;
};

class DiagnosticTextFormatter {
public:
  static void print(Stream& os,
                    const std::vector<Diagnostic>& diagnostics,
                    const std::string& source) {
    for (const auto& diagnostic : diagnostics) {
      printDiagnostic(os, diagnostic, source);
    }
  }

  static void print(std::ostream& os,
                    const std::vector<Diagnostic>& diagnostics,
                    const std::string& source) {
    for (const auto& diagnostic : diagnostics) {
      printDiagnostic(os, diagnostic, source);
    }
  }

private:
  static std::string levelToText(DiagnosticLevel level) {
    switch (level) {
      case DiagnosticLevel::Note: return "note";
      case DiagnosticLevel::Warning: return "warning";
      case DiagnosticLevel::Error: return "error";
    }

    return "unknown";
  }

  static Color levelToColor(DiagnosticLevel level) {
    switch (level) {
      case DiagnosticLevel::Note: return Color::BLUE;
      case DiagnosticLevel::Warning: return Color::YELLOW;
      case DiagnosticLevel::Error: return Color::RED;
    }

    return Color::WHITE;
  }

  static void printDiagnostic(Stream& os,
                              const Diagnostic& diagnostic,
                              const std::string& source) {
    os.changeColor(levelToColor(diagnostic.level), true);
    os << levelToText(diagnostic.level);
    os.resetColor();
    os << ": " << diagnostic.message << '\n';
    os << " --> " << diagnostic.fileName << ":"
       << diagnostic.range.start.line << ":" << diagnostic.range.start.column
       << '\n';

    printContext(os, diagnostic, source);
  }

  static void printDiagnostic(std::ostream& os,
                              const Diagnostic& diagnostic,
                              const std::string& source) {
    os << levelToText(diagnostic.level) << ": " << diagnostic.message << '\n';
    os << " --> " << diagnostic.fileName << ":"
       << diagnostic.range.start.line << ":" << diagnostic.range.start.column
       << '\n';

    printContext(os, diagnostic, source);
  }

  static void printContext(Stream& os,
                           const Diagnostic& diagnostic,
                           const std::string& source) {
    SourceSpan span = diagnostic.span;
    size_t lineStart = 0;
    if (span.offset <= source.size()) {
      size_t searchStart = span.offset;
      while (searchStart > 0 && source[searchStart - 1] != '\n') {
        searchStart--;
      }
      lineStart = searchStart;
    } else {
      size_t line = 1;
      size_t i = 0;
      while (i < source.length() && line < span.line) {
        if (source[i] == '\n') {
          line++;
          lineStart = i + 1;
        }
        i++;
      }
    }

    size_t lineEnd = lineStart;
    while (lineEnd < source.length() && source[lineEnd] != '\n') {
      lineEnd++;
    }

    std::string lineContent = source.substr(lineStart, lineEnd - lineStart);

    std::string lineNumStr = std::to_string(span.line);
    os << " " << lineNumStr << " | " << lineContent << "\n";

    size_t prefixLen = lineNumStr.length() + 4;
    for (size_t j = 0; j < prefixLen; ++j) os << " ";

    size_t startIdx = 0;
    if (span.offset >= lineStart && span.offset <= lineEnd) {
      startIdx = span.offset - lineStart;
    } else if (span.column > 0) {
      startIdx = span.column - 1;
    }
    if (startIdx > lineContent.size()) {
      startIdx = lineContent.size();
    }

    for (size_t j = 0; j < startIdx; ++j) {
      os << (lineContent[j] == '\t' ? '\t' : ' ');
    }

    os.changeColor(levelToColor(diagnostic.level), true);
    size_t len = span.length > 0 ? span.length : 1;

    if (startIdx >= lineContent.size()) {
      len = 1;
    } else {
      size_t maxAvailable = lineContent.size() - startIdx;
      if (len > maxAvailable) len = maxAvailable;
    }

    const size_t MAX_UNDERLINE = 40;
    if (len > MAX_UNDERLINE) {
      size_t half = MAX_UNDERLINE / 2;
      for (size_t k = 0; k < half; ++k) os << "^";
      os << "...";
      for (size_t k = 0; k < half; ++k) os << "^";
    } else {
      for (size_t j = 0; j < len; ++j) {
        os << "^";
      }
    }
    os.resetColor();
    os << '\n';
  }

  static void printContext(std::ostream& os,
                           const Diagnostic& diagnostic,
                           const std::string& source) {
    SourceSpan span = diagnostic.span;
    size_t lineStart = 0;
    if (span.offset <= source.size()) {
      size_t searchStart = span.offset;
      while (searchStart > 0 && source[searchStart - 1] != '\n') {
        searchStart--;
      }
      lineStart = searchStart;
    } else {
      size_t line = 1;
      size_t i = 0;
      while (i < source.length() && line < span.line) {
        if (source[i] == '\n') {
          line++;
          lineStart = i + 1;
        }
        i++;
      }
    }

    size_t lineEnd = lineStart;
    while (lineEnd < source.length() && source[lineEnd] != '\n') {
      lineEnd++;
    }

    std::string lineContent = source.substr(lineStart, lineEnd - lineStart);

    std::string lineNumStr = std::to_string(span.line);
    os << " " << lineNumStr << " | " << lineContent << "\n";

    size_t prefixLen = lineNumStr.length() + 4;
    for (size_t j = 0; j < prefixLen; ++j) os << " ";

    size_t startIdx = 0;
    if (span.offset >= lineStart && span.offset <= lineEnd) {
      startIdx = span.offset - lineStart;
    } else if (span.column > 0) {
      startIdx = span.column - 1;
    }
    if (startIdx > lineContent.size()) {
      startIdx = lineContent.size();
    }

    for (size_t j = 0; j < startIdx; ++j) {
      os << (lineContent[j] == '\t' ? '\t' : ' ');
    }

    size_t len = span.length > 0 ? span.length : 1;

    if (startIdx >= lineContent.size()) {
      len = 1;
    } else {
      size_t maxAvailable = lineContent.size() - startIdx;
      if (len > maxAvailable) len = maxAvailable;
    }

    const size_t MAX_UNDERLINE = 40;
    if (len > MAX_UNDERLINE) {
      size_t half = MAX_UNDERLINE / 2;
      for (size_t k = 0; k < half; ++k) os << "^";
      os << "...";
      for (size_t k = 0; k < half; ++k) os << "^";
    } else {
      for (size_t j = 0; j < len; ++j) {
        os << "^";
      }
    }
    os << std::endl;
  }
};

class DiagnosticEngine {
private:
  const std::string& source;
  std::string fileName;
  std::vector<Diagnostic> diagnostics_;
  size_t errorCount = 0;

public:
  DiagnosticEngine(const std::string& src, const std::string& fname = "input") 
    : source(src), fileName(fname) {}

  void report(SourceSpan span, DiagnosticLevel level, const std::string& message) {
    diagnostics_.push_back(Diagnostic{level, message, fileName, span,
                                      makeRange(span)});

    if (level == DiagnosticLevel::Error) {
      errorCount++;
    }
  }

  bool hadErrors() const {
    return errorCount > 0;
  }

  bool empty() const {
    return diagnostics_.empty();
  }

  const std::vector<Diagnostic>& diagnostics() const {
    return diagnostics_;
  }

  const std::string& sourceText() const {
    return source;
  }

  const std::string& sourceName() const {
    return fileName;
  }

  void printText(std::ostream& os) const {
    DiagnosticTextFormatter::print(os, diagnostics_, source);
  }

  void printText(Stream& os) const {
    DiagnosticTextFormatter::print(os, diagnostics_, source);
  }

  std::string toJson() const {
    std::string json = "[";

    for (size_t i = 0; i < diagnostics_.size(); ++i) {
      if (i > 0) {
        json += ',';
      }

      const auto& diagnostic = diagnostics_[i];
      json += "{";
      appendJsonField(json, "level", levelToString(diagnostic.level));
      json += ",";
      appendJsonField(json, "message", diagnostic.message);
      json += ",";
      appendJsonField(json, "fileName", diagnostic.fileName);
      json += ",\"range\":{";
      json += "\"start\":";
      appendJsonPosition(json, diagnostic.range.start);
      json += ",\"end\":";
      appendJsonPosition(json, diagnostic.range.end);
      json += "}}";
    }

    json += "]";
    return json;
  }

private:
  static std::string levelToString(DiagnosticLevel level) {
    switch (level) {
      case DiagnosticLevel::Note: return "note";
      case DiagnosticLevel::Warning: return "warning";
      case DiagnosticLevel::Error: return "error";
    }

    return "unknown";
  }

  DiagnosticRange makeRange(SourceSpan span) const {
    DiagnosticPosition start{span.line, span.column, span.offset};

    size_t endOffset = span.offset;
    size_t remaining = span.length;
    if (endOffset > source.size()) {
      endOffset = source.size();
      remaining = 0;
    }

    size_t line = start.line;
    size_t column = start.column;

    while (endOffset < source.size() && remaining > 0) {
      if (source[endOffset] == '\n') {
        line++;
        column = 1;
      } else {
        column++;
      }
      endOffset++;
      remaining--;
    }

    if (span.length == 0) {
      column = start.column + 1;
    }

    return DiagnosticRange{start, DiagnosticPosition{line, column, endOffset}};
  }

  static void appendJsonEscaped(std::string& json, const std::string& value) {
    json += '"';
    for (char ch : value) {
      switch (ch) {
        case '\\': json += "\\\\"; break;
        case '"': json += "\\\""; break;
        case '\n': json += "\\n"; break;
        case '\r': json += "\\r"; break;
        case '\t': json += "\\t"; break;
        default: json += ch; break;
      }
    }
    json += '"';
  }

  static void appendJsonField(std::string& json,
                              const std::string& key,
                              const std::string& value) {
    appendJsonEscaped(json, key);
    json += ":";
    appendJsonEscaped(json, value);
  }

  static void appendJsonPosition(std::string& json,
                                 const DiagnosticPosition& position) {
    json += "{";
    json += "\"line\":";
    json += std::to_string(position.line);
    json += ",\"column\":";
    json += std::to_string(position.column);
    json += ",\"offset\":";
    json += std::to_string(position.offset);
    json += "}";
  }
};

} // namespace zap
