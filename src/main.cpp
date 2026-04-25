#include "driver/driver.hpp"
#include <exception>
#include <iostream>

int main(int argc, char **argv) {
  try {
    zap::driver zapcDriver;
    if (argc > 0 && argv[0]) {
      zapcDriver.setExecutablePath(argv[0]);
    }

    if (!zapcDriver.parseArgs(argc, argv)) {
      return 0;
    }

    if (zapcDriver.splitInputs()) {
      return 1;
    }

    if (zapcDriver.verifySources()) {
      return 1;
    }

    if (zapcDriver.verifyOutput()) {
      return 1;
    }

    if (zapcDriver.compile()) {
      return 1;
    }

    int err = 0;
    if (zapcDriver.link()) {
      err = 1;
    }

    if (zapcDriver.cleanup()) {
      return 1;
    }

    return err;
  } catch (const std::exception &ex) {
    std::cerr << "zapc: unhandled exception: " << ex.what() << '\n';
    return 1;
  } catch (...) {
    std::cerr << "zapc: unhandled non-standard exception\n";
    return 1;
  }
}
