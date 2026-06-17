#pragma once

#include "driver/args/args.hpp"
#include "utils/stream.hpp"
#include <string>
#include <string_view>
#include <vector>

namespace zap {

template <typename... TArgs> void reportError(TArgs &&...args) {
  ((err() << "zapc: ").changeColor(Color::RED, true) << "error: ").resetColor();
  (err() << ... << args);
  err() << '\n';
}

void printHelp();
void printVersion();

namespace args {

enum class ParseResult {
  Success,
  Failed,
  SkipCompilation,
};

/// @brief Parses command line arguments from provided argc & argv
/// @return Success on success (no way), Failed on error, SkipCompilation if
/// help/version was printed.
ParseResult parse(int argc, char **argv, CmdlineArgs &args);

/// @brief Parses command line arguments from provided vector of string views
/// @return Success on success, Failed on error, SkipCompilation if help/version
/// was printed.
ParseResult parse(const std::vector<std::string_view> &cmdline,
                  CmdlineArgs &args);

/// @brief Parses command line arguments from provided vector of strings
/// @return Success on success, Failed on error, SkipCompilation if help/version
/// was printed.
ParseResult parse(const std::vector<std::string> &argv, CmdlineArgs &args);

} // namespace args
} // namespace zap
