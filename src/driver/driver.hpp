#pragma once

#include <llvm/Option/ArgList.h>
#include <llvm/Option/OptTable.h>

namespace zap
{

namespace opts
{

  enum ID
  {
    OPT_INVALID = 0,
    #define OPTION(...) LLVM_MAKE_OPT_ID(__VA_ARGS__),
    #include "Options.inc"
    #undef OPTION
  };

  class ZapcOptTable : public llvm::opt::GenericOptTable
  {
  public:
    ZapcOptTable();
  };

} // namespace opts

  class driver
  {
  public:
    driver();

    bool parseArgs(int argc, char **argv);
  private:
  };

} // namespace driver