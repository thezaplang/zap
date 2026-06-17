#pragma once

#include <filesystem>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "args/argparse.hpp"
#include "args/args.hpp"
#include "utils/stream.hpp"

namespace zap {

/// @brief Orchestrates and controls the entire compilation process.
/// Order of the functions that should be called is by how they are defined here
/// in the header. Meaning that first is parseArgs(int, char**), second is
/// splitInputs(), third is verifySources(), and so on... Most functions
/// return true on *error* not on success. It returns error due to easy checks
/// like if(verifyOutput()) return 1;
class driver {
public:
  driver();
  void setExecutablePath(std::filesystem::path path);

  /// @brief Parses the provided args.
  /// @param argc How many arguments.
  /// @param argv Pointer to the arguments.
  args::ParseResult parseArgs(int argc, char **argv);

  /// @brief Splits inputs based on their file extension.
  /// @return True if an error has occured.
  /// This should be called second so that verifyOutput() can work.
  bool splitInputs();

  /// @brief Verifies that the sources provided actually exist or not.
  /// @return True if an error has occured.
  /// This should be called third after splitting the input files.
  bool verifySources();

  /// @brief Checks if the output is valid or not.
  /// @return True if invalid.
  /// This should be called fourth after checking the source/object files.
  bool verifyOutput();

  /// @brief Compiles the files to the chosen output mode.
  /// @return True if an error has occured.
  /// Should be called fifth after checking that the output is valid.
  bool compile();

  /// @brief Links everything if the output mode requires it.
  /// @return True if an error has occured.
  /// Should be called sixth after compiling.
  bool link();

  /// @brief Cleans up the files in the cleanup queue.
  /// @return True if an error has occured.
  /// Should be called seventh after linking.
  bool cleanup();

  /// @brief Returns the unsplit input files vector.
  /// @return Const reference to the input files vector.
  const std::vector<std::string_view> &get_inputs() const noexcept {
    return cmdArgs.inputs;
  }

  /// @brief Returns the paths to the source files.
  /// @return Const reference to the source files vector.
  const std::vector<std::filesystem::path> &get_sources() const noexcept {
    return cmdArgs.sources;
  }

  /// @brief Returns the desired path of the output file.
  /// @return Const reference to the output file path.
  const std::filesystem::path &get_output() const noexcept {
    return cmdArgs.output.path;
  }

  /// @brief Returns whether or not the output was explicit (-o) or not.
  /// @return True if was implicit, false if explicit.
  bool is_implicit_output() const noexcept { return cmdArgs.output.implicit; }
  bool emits_llvm_text() const noexcept {
    return cmdArgs.output.type == args::OutputType::TEXT_LLVM;
  }
  bool emits_zir() const noexcept {
    return cmdArgs.output.type == args::OutputType::ZIR;
  }
  const std::unordered_map<std::string, std::string> &
  get_import_map() const noexcept {
    return cmdArgs.importMap;
  }
  bool emits_text_output() const noexcept {
    return cmdArgs.output.type == args::OutputType::TEXT_LLVM ||
           cmdArgs.output.type == args::OutputType::ZIR ||
           cmdArgs.output.type == args::OutputType::ASM;
  }

  /// @brief Returns the chosen output type.
  args::OutputType get_output_type() const noexcept {
    return cmdArgs.output.type;
  }

  /// @brief Returns whether or not the output type needs linking or not.
  /// @return True if needs, false if not.
  /// This shouldn't be used along link() since it already checks it.
  bool needs_linking() const noexcept {
    return !emits_text_output() &&
           (cmdArgs.output.type == args::OutputType::EXEC);
  }

  /// @brief Returns if the current selected format is supported by this
  /// compiler version.
  /// @return True if supported, false if not.
  /// This should change as the compiler evolves, ideally all of the below
  /// should be true.
  bool format_supported() const noexcept {
    return true; // Condition is always true.
  }

  /// @brief Returns a file extension based on the file format given.
  /// @return Read-only string.
  constexpr static const char *
  format_fileextension(args::OutputType type) noexcept {
    switch (type) {
    default:
      [[fallthrough]];
    case args::OutputType::EXEC: // This should depend on the target set, not
                                 // host.
#ifdef _WIN32
      return ".exe";
#else
      return "";
#endif
    case args::OutputType::OBJECT:
      return ".o";
    case args::OutputType::ASM:
      return ".s";
    case args::OutputType::TEXT_LLVM:
      return ".ll";
    case args::OutputType::LLVM:
      return ".bc";
    case args::OutputType::ZIR:
      return ".zir";
    }
  }

  /// @brief Returns whether the output is binary or text.
  /// @return True if binary, false if text.
  bool binary_output() const noexcept {
    switch (cmdArgs.output.type) {
    case args::OutputType::EXEC:
      [[fallthrough]];
    case args::OutputType::OBJECT:
      [[fallthrough]];
    case args::OutputType::LLVM:
      return true;
    default:
      return false;
    }
  }

  template <typename... Args> static void reportError(Args &&...args) {
    zap::reportError(std::forward<Args>(args)...);
  }

  template <typename... Args> static void reportWarning(Args &&...args) {
    ((err() << "zapc: ").changeColor(Color::YELLOW, true) << "warning: ")
        .resetColor();
    (err() << ... << args);
    err() << '\n';
  }

private:
  friend bool compileLoadedModules(driver &drv,
                                   const std::filesystem::path &entryPath);

  /// @brief Used internally by the compile() function.
  /// @return True if an error has occured.
  bool compileSourceFile(const std::string &source,
                         const std::string &source_name);

  args::CmdlineArgs cmdArgs; ///< Parsed command line arguments.
  std::vector<std::filesystem::path>
      cleanups; ///< A vector of files that need to be deleted.
  std::filesystem::path executable_path; ///< Path to the running executable.
};

} // namespace zap
