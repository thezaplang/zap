#include "lexer/source_group.hpp"

#include "lexer/lexer.hpp"
#include "utils/diagnostics.hpp"

#include <utility>

namespace zap {
namespace {

struct Frame {
  char closing;
  bool interpolation;
  size_t opening;
  size_t bodyBegin;
};

struct InterpolationRange {
  size_t opening;
  size_t bodyBegin;
  size_t bodyEnd;
  size_t closing;
};

void advanceLocation(char ch, size_t &line, size_t &column) {
  if (ch == '\n') {
    ++line;
    column = 1;
  } else {
    ++column;
  }
}

SourceSpan spanAt(const std::string &input, size_t openingOffset,
                  size_t openingLine, size_t openingColumn, size_t offset,
                  size_t length, const std::string &sourceName) {
  size_t line = openingLine;
  size_t column = openingColumn;
  for (size_t index = openingOffset; index < offset && index < input.size();
       ++index)
    advanceLocation(input[index], line, column);
  return SourceSpan(line, column, offset, length, sourceName);
}

void rebaseFragment(SourceFragment &child, const SourceFragment &parent,
                    size_t parentBegin);

void rebaseToken(Token &token, const SourceFragment &parent,
                 size_t parentBegin) {
  const size_t relativeOffset = token.span.offset;
  token.span = parent.spanAt(parentBegin + relativeOffset, token.span.length);
  if (token.sourceFragment) {
    auto nested = std::make_shared<SourceFragment>(*token.sourceFragment);
    rebaseFragment(*nested, parent, parentBegin);
    token.sourceFragment = std::move(nested);
  }
}

void rebaseFragment(SourceFragment &child, const SourceFragment &parent,
                    size_t parentBegin) {
  for (auto &interpolation : child.interpolations) {
    const size_t relativeOffset = interpolation.span.offset;
    interpolation.span =
        parent.spanAt(parentBegin + relativeOffset, interpolation.span.length);
    for (auto &token : interpolation.tokens)
      rebaseToken(token, parent, parentBegin);
  }
  for (auto &location : child.offsetMap) {
    const auto &mapped = parent.offsetMap.at(parentBegin + location.offset);
    location = mapped;
  }
  child.sourceName = parent.sourceName;
}

bool lexInterpolation(SourceFragment &fragment, const InterpolationRange &range,
                      size_t bodyOffset, DiagnosticEngine &diagnostics,
                      SourceCaptureBudget &budget) {
  const size_t begin = range.bodyBegin - bodyOffset;
  const size_t end = range.bodyEnd - bodyOffset;
  // Cover lexer tokens, input copies and rebasing copies before lexing.
  if (!budget.charge(end - begin, 1024) ||
      !budget.charge(fragment.sourceName.size(), 4 * (end - begin))) {
    diagnostics.report(fragment.spanAt(begin, end - begin),
                       DiagnosticLevel::Error, "M3003",
                       "Source capture memory limit exceeded.");
    return false;
  }
  const std::string source = fragment.text.substr(begin, end - begin);
  DiagnosticEngine local(source, fragment.sourceName);
  Lexer lexer(local);
  lexer.sourceCaptureBudget = &budget;
  auto tokens = lexer.tokenize(source);
  for (auto &token : tokens)
    rebaseToken(token, fragment, begin);
  for (const auto &error : local.diagnostics()) {
    diagnostics.report(
        fragment.spanAt(begin + error.span.offset, error.span.length),
        error.level, error.code, error.message);
  }
  if (local.hadErrors())
    return false;
  fragment.interpolations.push_back(
      {fragment.spanAt(range.opening - bodyOffset,
                       range.closing + 1 - range.opening),
       begin, end, std::move(tokens)});
  return true;
}

} // namespace

std::optional<SourceGroupCapture>
captureSourceGroup(const std::string &input, size_t openingOffset,
                   size_t openingLine, size_t openingColumn,
                   DiagnosticEngine &diagnostics, SourceCaptureBudget &budget) {
  if (openingOffset >= input.size() || input[openingOffset] != '{')
    return std::nullopt;
  auto limit = [&]() -> std::optional<SourceGroupCapture> {
    diagnostics.report(SourceSpan(openingLine, openingColumn, openingOffset, 1,
                                  diagnostics.sourceName()),
                       DiagnosticLevel::Error, "M3003",
                       "Source capture memory or nesting limit exceeded.");
    return std::nullopt;
  };
  if (budget.depth >= SourceCaptureBudget::MaxDepth ||
      !budget.charge(sizeof(SourceFragment) +
                     diagnostics.sourceName().size()) ||
      !budget.charge(2, sizeof(Frame)))
    return limit();
  ++budget.depth;
  struct DepthGuard {
    size_t &depth;
    ~DepthGuard() { --depth; }
  } guard{budget.depth};
  std::vector<Frame> frames{{'}', false, openingOffset, openingOffset + 1}};
  std::vector<InterpolationRange> interpolations;
  size_t interpolationDepth = 0;
  size_t cursor = openingOffset + 1;
  enum class Mode { Normal, Quoted, LineComment, BlockComment };
  Mode mode = Mode::Normal;
  char quote = '\0';
  while (cursor < input.size() && !frames.empty()) {
    if (cursor - openingOffset > budget.remaining / (sizeof(SourceOffset) + 1))
      return limit();
    const char ch = input[cursor];
    const char next = cursor + 1 < input.size() ? input[cursor + 1] : '\0';
    if (mode == Mode::LineComment) {
      if (ch == '\n')
        mode = Mode::Normal;
      ++cursor;
      continue;
    }
    if (mode == Mode::BlockComment) {
      if (ch == '*' && next == '/') {
        cursor += 2;
        mode = Mode::Normal;
      } else {
        ++cursor;
      }
      continue;
    }
    if (mode == Mode::Quoted) {
      if (ch == '\\' && next != '\0') {
        cursor += 2;
      } else if (ch == quote) {
        if (interpolationDepth == 0 && next == quote) {
          cursor += 2;
        } else {
          mode = Mode::Normal;
          ++cursor;
        }
      } else {
        ++cursor;
      }
      continue;
    }
    // Dialect comments such as SQL `--` stay raw: `--` is also a Zap operator.
    if (ch == '/' && next == '/') {
      mode = Mode::LineComment;
      cursor += 2;
      continue;
    }
    if (ch == '/' && next == '*') {
      mode = Mode::BlockComment;
      cursor += 2;
      continue;
    }
    if (ch == '\'' || ch == '"' || ch == '`') {
      mode = Mode::Quoted;
      quote = ch;
      ++cursor;
      continue;
    }
    if (ch == '$' && next == '{') {
      if (!budget.charge(2, sizeof(Frame)))
        return limit();
      frames.push_back({'}', true, cursor, cursor + 2});
      ++interpolationDepth;
      cursor += 2;
      continue;
    }
    if (ch == '{' || ch == '(' || ch == '[') {
      if (!budget.charge(2, sizeof(Frame)))
        return limit();
      frames.push_back({ch == '{'   ? '}'
                        : ch == '(' ? ')'
                                    : ']',
                        false, cursor, cursor + 1});
      ++cursor;
      continue;
    }
    if (ch == '}' || ch == ')' || ch == ']') {
      if (frames.back().closing != ch) {
        diagnostics.report(spanAt(input, openingOffset, openingLine,
                                  openingColumn, cursor, 1,
                                  diagnostics.sourceName()),
                           DiagnosticLevel::Error,
                           "Mismatched delimiter in source macro group.");
        return std::nullopt;
      }
      const Frame frame = frames.back();
      frames.pop_back();
      if (frame.interpolation) {
        if (interpolationDepth == 1) {
          if (!budget.charge(2, sizeof(InterpolationRange) +
                                    sizeof(SourceInterpolation)))
            return limit();
          interpolations.push_back(
              {frame.opening, frame.bodyBegin, cursor, cursor});
        }
        --interpolationDepth;
      }
      ++cursor;
      continue;
    }
    ++cursor;
  }
  if (!frames.empty()) {
    const size_t opening = frames.back().opening;
    diagnostics.report(spanAt(input, openingOffset, openingLine, openingColumn,
                              opening, 1, diagnostics.sourceName()),
                       DiagnosticLevel::Error,
                       frames.back().interpolation
                           ? "Unterminated source macro interpolation."
                           : "Unterminated source macro group.");
    return std::nullopt;
  }

  const size_t bodyOffset = openingOffset + 1;
  const size_t closingOffset = cursor - 1;
  if (!budget.charge(closingOffset - bodyOffset + 1, sizeof(SourceOffset) + 1))
    return limit();
  auto fragment = std::make_shared<SourceFragment>();
  fragment->text = input.substr(bodyOffset, closingOffset - bodyOffset);
  fragment->sourceName = diagnostics.sourceName();
  fragment->offsetMap.reserve(fragment->text.size() + 1);
  size_t line = openingLine;
  size_t column = openingColumn + 1;
  for (size_t index = 0; index < fragment->text.size(); ++index) {
    fragment->offsetMap.push_back({line, column, bodyOffset + index});
    advanceLocation(fragment->text[index], line, column);
  }
  fragment->offsetMap.push_back({line, column, closingOffset});
  for (const auto &range : interpolations) {
    if (!lexInterpolation(*fragment, range, bodyOffset, diagnostics, budget))
      return std::nullopt;
  }
  SourceGroupCapture capture;
  capture.fragment = std::move(fragment);
  capture.nextOffset = cursor;
  capture.closingLine = line;
  capture.closingColumn = column;
  advanceLocation('}', line, column);
  capture.nextLine = line;
  capture.nextColumn = column;
  return capture;
}

std::optional<TokenTree> materializeSourceGroup(const TokenTree &group,
                                                DiagnosticEngine &diagnostics) {
  if (group.isLeaf())
    return group;
  std::vector<TokenTree> children;
  if (group.opening().sourceFragment) {
    const auto &fragment = *group.opening().sourceFragment;
    SourceCaptureBudget budget;
    if (!budget.charge(fragment.text.size(), 256) ||
        !budget.charge(fragment.sourceName.size(), 4 * fragment.text.size())) {
      diagnostics.report(group.opening().span, DiagnosticLevel::Error, "M3003",
                         "Source materialization memory limit exceeded.");
      return std::nullopt;
    }
    DiagnosticEngine local(fragment.text, fragment.sourceName);
    Lexer lexer(local);
    lexer.sourceCaptureBudget = &budget;
    auto tokens = lexer.tokenize(fragment.text);
    for (auto &token : tokens)
      rebaseToken(token, fragment, 0);
    for (const auto &error : local.diagnostics()) {
      diagnostics.report(fragment.spanAt(error.span.offset, error.span.length),
                         error.level, error.code, error.message);
    }
    if (local.hadErrors())
      return std::nullopt;
    auto grouped = TokenTreeBuilder::build(tokens, diagnostics);
    if (grouped.hadDelimiterErrors)
      return std::nullopt;
    children = std::move(grouped.trees);
  } else {
    children = group.children();
  }
  // Nested macro groups may contain foreign syntax. Their own macro decides
  // whether they are source captures or Zap tokens.
  return TokenTree::group(group.delimiter(), group.opening(),
                          std::move(children), group.closing());
}

} // namespace zap
