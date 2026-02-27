#include "driver/driver.hpp"
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/raw_ostream.h>

int main(int argc, char **argv) {
  llvm::InitLLVM X(argc, argv);

  zap::driver zapcDriver;

  ///
  /// Parse the argc and the argv.
  ///
  if (!zapcDriver.parseArgs(argc, argv)) {
    return 0;
  }

  ///
  /// Split the inputs to sources and objects.
  ///
  if (zapcDriver.splitInputs()) {
    return 1;
  }

  ///
  /// Verify that the sources exist on disk and are files.
  ///
  if (zapcDriver.verifySources()) {
    return 1;
  }

  ///
  /// Verify if the inputs and the output are valid or not.
  ///
  if (zapcDriver.verifyOutput()) {
    return 1;
  }

  ///
  /// Compile the files into whatever format was selected.
  ///
  if (zapcDriver.compile()) {
    return 1;
  }

  ///
  /// Link (if needed) the final files.
  ///
  int err = 0;
  if (zapcDriver.link()) {
    err = 1;
  }

  ///
  /// Clean up whatever needs it.
  ///
  if (zapcDriver.cleanup()) {
    return 1;
  }

  return err;
}