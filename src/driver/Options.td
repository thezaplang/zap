include "llvm/Option/OptParser.td"

def help : Flag<["--"], "help">, HelpText<"Display available options">;
def version : Flag<["--"], "version">, HelpText<"Print version information">;

// Output file
def output : Separate<["-"], "o">, HelpText<"Write output to <file>">;
def : Joined<["-"], "o">, Alias<output>;