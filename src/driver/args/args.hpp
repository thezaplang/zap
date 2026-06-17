#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace zap::args {

using FilePath = std::filesystem::path;
using ImportMap = std::unordered_map<std::string, std::string>;

/// @brief Different output types that the driver (most likely) supports.
enum class OutputType : uint8_t {
  EXEC,      ///< Executable (default), link.
  OBJECT,    ///< Object file, no linking.
  ASM,       ///< Assembly.
  TEXT_LLVM, ///< Textual LLVM IR (-S -emit-llvm).
  LLVM,      ///< LLVM IR (.bc).
  ZIR,       ///< ZIR.
};

/// @brief Optimization level used by the compiler.
enum class OptLevel : uint8_t {
  O0, ///< No optimization.
  O1, ///< Less optimizations.
  O2, ///< Standard optimization level.
  O3, ///< More optimizations.
};

struct CmdlineArgs {
  std::vector<std::string_view> inputs; ///< A vector of input files.
  std::vector<FilePath> sources;        ///< A vector of .zp files.
  std::vector<FilePath> objects;        ///< A vector of .o files.

  struct {
    FilePath path;                      ///< Output file.
    OutputType type = OutputType::EXEC; ///< Output type, default executable.
    bool implicit; ///< Was the output implicit or explicit.
  } output;

  bool incStdlib;         ///< Include the zap stdlib.o or not.
  bool incPrelude = true; ///< Include the implicit prelude or not.

  OptLevel optLevel = OptLevel::O1; ///< Optimization level (0-3).

  std::vector<std::string>
      linkerArgs; ///< Extra linker arguments (e.g. -lSDL2, -L/path).

  ImportMap importMap; ///< Import path aliases (@alias -> path).
};

} // namespace zap::args
