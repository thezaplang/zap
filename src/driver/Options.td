include "llvm/Option/OptParser.td"

// --help
def help : Flag<["--"], "help">,
  HelpText<"Display available options">;

// --version
def version : Flag<["--"], "version">,
  HelpText<"Print version information">;

// -o <file>
def output : Separate<["-"], "o">,
  HelpText<"Write output to <file>">;

// -o<file> usually expected from gnu-style cli
def : Joined<["-"], "o">, Alias<output>;

def nostdlib : Flag<["-"], "nostdlib">,
  HelpText<"Stops the linker from linking the zap stdlib">;

// -c
def nolink : Flag<["-"], "c">,
  HelpText<"Compile and assemble but not link">;

// -S
def emitS : Flag<["-"], "S">,
  HelpText<"Compile only no assembling or linking">;

// -emit-llvm
def emitLLVM : Flag<["-"], "emit-llvm">, 
  HelpText<"Emit LLVM IR instead of final output">;

// -emit-zir
def emitZIR : Flag<["-"], "emit-zir">, 
  HelpText<"Emit ZIR instead of final output">;