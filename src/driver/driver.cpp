#include "driver/driver.hpp"
#include "driver/compiler.hpp"
#include <llvm/ADT/ArrayRef.h>
#include <llvm/Support/raw_ostream.h>

namespace zap
{

namespace opts
{
using namespace llvm::opt;

#define OPTTABLE_STR_TABLE_CODE
#include "Options.inc"
#undef OPTTABLE_STR_TABLE_CODE

#define OPTTABLE_PREFIXES_TABLE_CODE
#include "Options.inc"
#undef OPTTABLE_PREFIXES_TABLE_CODE

static const OptTable::Info InfoTable[] = {
#define OPTION(...) LLVM_CONSTRUCT_OPT_INFO(__VA_ARGS__),
#include "Options.inc"
#undef OPTION
};

ZapcOptTable::ZapcOptTable()
    : GenericOptTable(OptionStrTable, OptionPrefixesTable, InfoTable) {}
  
} // namespace opts


driver::driver()
{
  
}

enum class ColorState : uint8_t
{
  None,
  Available,
  Unavailable
};

ColorState colors = ColorState::None;

void print_red(llvm::StringRef msg)
{
  if (colors == ColorState::None) {
    colors = llvm::errs().has_colors() ? ColorState::Available : ColorState::Unavailable;
  }

  switch (colors) {
    case ColorState::Available:
      llvm::errs().changeColor(llvm::raw_ostream::RED, true);
      llvm::errs() << msg;
      llvm::errs().resetColor();
      break;
    case ColorState::Unavailable:
      llvm::errs() << msg;
      break;
    default:
      break;
  }
    
}

bool driver::parseArgs(int argc, char **argv)
{
  opts::ZapcOptTable table;
  unsigned missingIndex, missingCount;
  auto argsArr = 
      llvm::ArrayRef<const char*>(argv, (size_t)argc).slice(1);

  auto args = table.ParseArgs(argsArr, missingIndex, missingCount);

  if (args.hasArg(opts::OPT_help)) {
    table.printHelp(
      llvm::outs(),
      ZAP_NAME_MACRO " [options] file...",
      "Zap Compiler"
    );
    return false;
  }

  if (args.hasArg(opts::OPT_version)) {
    llvm::outs() << "Zap Compiler v" << zap::ZAP_VERSION << '\n';
    return false;
  }

  if (missingCount) {
      print_red("error: ");
      llvm::errs() << "argument to '" 
                   << args.getArgString(missingIndex) 
                   << "' is missing\n";
      return false;
  }

  llvm::errs() << "zapc: ";
  print_red("error: ");
  llvm::errs() << "no input files\n";
  return false;
}

} // namespace driver