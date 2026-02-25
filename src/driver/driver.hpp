#pragma once

#include <llvm/Option/ArgList.h>
#include <llvm/Option/OptTable.h>
#include <vector>
#include <string>

namespace zap
{

namespace opts
{
  ///
  /// @brief Driver's available options.
  ///
  enum ID
  {
    OPT_INVALID = 0,
    #define OPTION(...) LLVM_MAKE_OPT_ID(__VA_ARGS__),
    #include "Options.inc"
    #undef OPTION
  };

  ///
  /// @brief Table that the driver class uses.
  ///
  class ZapcOptTable : public llvm::opt::GenericOptTable
  {
  public:
    ZapcOptTable();
  };

} // namespace opts

  ///
  /// @brief The class that drives the argument parsing.
  ///
  class driver
  {
  public:
    driver();

    bool parseArgs(int argc, char **argv);

    const std::vector<std::string>& get_inputs() const noexcept{
      return inputs;
    }

    const std::string& get_output() const noexcept{
      return output;
    }
    
  private:
    std::vector<std::string> inputs;
    std::string output;
  };

} // namespace driver