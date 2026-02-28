#pragma once

#include <filesystem>
#include <llvm/Option/ArgList.h>
#include <llvm/Option/OptTable.h>
#include <string>
#include <vector>

namespace zap {

namespace opts {
/// @brief Driver's available options.
enum ID {
  OPT_INVALID = 0,
#define OPTION(...) LLVM_MAKE_OPT_ID(__VA_ARGS__),
#include "Options.inc"
#undef OPTION
};

/// @brief Table that the driver class uses.
class ZapcOptTable : public llvm::opt::GenericOptTable {
public:
  ZapcOptTable();
};

} // namespace opts

/// @brief The class that drives the argument parsing.
/// Order of the functions that should be called is by how they are defined here
/// in the header. Meaning that first is parseArgs(int, char**), second is
/// splitInputs(), third is verifySources(), and so on... Most functions
/// return true on *error* not on success. It returns error due to easy checks
/// like if(verifyOutput()) return 1;
class driver {
public:
  driver();

  /// @brief Parses the provided args.
  /// @param argc How many arguments.
  /// @param argv Pointer to the arguments.
  /// @return True if should continue compiling.
  /// This should be called first right after initializing llvm and the driver.
  bool parseArgs(int argc, char **argv);

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
  const std::vector<std::string> &get_inputs() const noexcept { return inputs; }

  /// @brief Returns the paths to the source files.
  /// @return Const reference to the source files vector.
  const std::vector<std::filesystem::path> &get_sources() const noexcept {
    return sources;
  }

  /// @brief Returns the desired path of the output file.
  /// @return Const reference to the output file path.
  const std::filesystem::path &get_output() const noexcept { return output; }

  /// @brief Returns whether or not the output was explicit (-o) or not.
  /// @return True if was implicit, false if explicit.
  bool is_implicit_output() const noexcept { return implicit_output; }

  /// @brief Different output types that the driver (most likely) supports.
  /// @see parseArgs(int, char**) implementation to see how it chooses the
  /// output type.
  enum class output_type : uint8_t {
    EXEC,      ///< Executable (default), link.
    OBJECT,    ///< Object file, no linking.
    ASM,       ///< Assembly.
    TEXT_LLVM, ///< Textual LLVM IR (-S -emit-llvm).
    LLVM,      ///< LLVM IR (.bc).
    ZIR,       ///< ZIR.
  };

  /// @brief Returns the chosen output type.
  output_type get_output_type() const noexcept { return out_type; }

  /// @brief Returns whether or not the output type needs linking or not.
  /// @return True if needs, false if not.
  /// This shouldn't be used along link() since it already checks it.
  bool needs_linking() const noexcept {
    return (out_type == output_type::EXEC);
  }

  /// @brief Returns if the current selected format is supported by this
  /// compiler version.
  /// @return True if supported, false if not.
  /// This should change as the compiler evolves, ideally all of the below
  /// should be true.
  bool format_supported() const noexcept {
    switch (out_type) {
    case output_type::EXEC:
      [[fallthrough]];
    case output_type::OBJECT:
      [[fallthrough]];
    case output_type::ZIR:
      [[fallthrough]];
    case output_type::TEXT_LLVM:
      return true;
    case output_type::ASM:
      [[fallthrough]];
    case output_type::LLVM:
      return false;
    }
  }

  /// @brief Returns a file extension based on the file format given.
  /// @return Read-only string.
  constexpr static const char *format_fileextension(output_type type) noexcept {
    switch (type) {
    default:
      [[fallthrough]];
    case output_type::EXEC:
#ifdef _WIN32
      return ".exe";
#else
      return "";
#endif
    case output_type::OBJECT:
      return ".o";
    case output_type::ASM:
      return ".s";
    case output_type::TEXT_LLVM:
      return ".ll";
    case output_type::LLVM:
      return ".bc";
    case output_type::ZIR:
      return ".zir";
    }
  }

  /// @brief Returns whether the output is binary or text.
  /// @return True if binary, false if text.
  bool binary_output() const noexcept {
    switch (out_type) {
    case output_type::EXEC:
      [[fallthrough]];
    case output_type::OBJECT:
      [[fallthrough]];
    case output_type::LLVM:
      return true;
    case output_type::ASM:
      [[fallthrough]];
    case output_type::TEXT_LLVM:
      [[fallthrough]];
    case output_type::ZIR:
      return false;
    }
  }

private:
  std::vector<std::string> inputs;            ///< A vector of input files.
  std::vector<std::filesystem::path> sources; ///< A vector of .zap files.
  std::vector<std::filesystem::path> objects; ///< A vector of .o files.
  std::vector<std::filesystem::path>
      cleanups;                 ///< A vector of files that need to be deleted.
  std::filesystem::path output; ///< Output file.
  output_type out_type =
      driver::output_type::EXEC; ///< Output type, default executable.
  bool implicit_output;          ///< Was the output implicit or explicit.
  bool inc_stdlib;               ///< Include the zap stdlib.o or not.

  ///
  /// @brief Used internally by the compile() function.
  /// @return True if an error has occured.
  ///
  bool compileSourceFile(const std::string &source,
                         const std::string &source_name);
};

} // namespace zap