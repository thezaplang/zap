/*===- TableGen'erated file -------------------------------------*- C++ -*-===*\
|*                                                                            *|
|* Option Parsing Definitions                                                 *|
|*                                                                            *|
|* Automatically generated file, do not edit!                                 *|
|*                                                                            *|
\*===----------------------------------------------------------------------===*/

/////////
// String table

#ifdef OPTTABLE_STR_TABLE_CODE

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif
static constexpr char OptionStrTableStorage[] = 
  "\0"
  "-\0"
  "--\0"
  "<input>\0"
  "<unknown>\0"
  "-c\0"
  "-emit-llvm\0"
  "-emit-zir\0"
  "--help\0"
  "-nostdlib\0"
  "-o\0"
  "-S\0"
  "--version\0"
  ;
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

static constexpr llvm::StringRef
OptionStrTable(OptionStrTableStorage, sizeof(OptionStrTableStorage) - 1);
#endif // OPTTABLE_STR_TABLE_CODE

/////////
// Prefixes

#ifdef OPTTABLE_PREFIXES_TABLE_CODE
static constexpr unsigned OptionPrefixesTable[] = {
  0 /* prefixes */,
  1 /* prefixes */, 1 /* '-' */,
  1 /* prefixes */, 3 /* '--' */
};
#endif // OPTTABLE_PREFIXES_TABLE_CODE

/////////
// Prefix Union

#ifdef OPTTABLE_PREFIXES_UNION_CODE
static constexpr unsigned OptionPrefixesUnion[] = {
  1 /* '-' */,   3 /* '--' */
};
#endif // OPTTABLE_PREFIXES_UNION_CODE

/////////
// ValuesCode

#ifdef OPTTABLE_VALUES_CODE
#endif
/////////
// Groups

#ifdef OPTION

//////////
// Options

OPTION(0, "<input>", INPUT, Input, INVALID, INVALID, nullptr, 0, DefaultVis, 0, nullptr, {}, nullptr, nullptr)
OPTION(0, "<unknown>", UNKNOWN, Unknown, INVALID, INVALID, nullptr, 0, DefaultVis, 0, nullptr, {}, nullptr, nullptr)
OPTION(1, "-c", nolink, Flag, INVALID, INVALID, nullptr, 0, DefaultVis, 0,
       "Compile and assemble but not link", {}, nullptr, nullptr)
OPTION(1, "-emit-llvm", emitLLVM, Flag, INVALID, INVALID, nullptr, 0, DefaultVis, 0,
       "Emit LLVM IR instead of final output", {}, nullptr, nullptr)
OPTION(1, "-emit-zir", emitZIR, Flag, INVALID, INVALID, nullptr, 0, DefaultVis, 0,
       "Emit ZIR instead of final output", {}, nullptr, nullptr)
OPTION(2, "--help", help, Flag, INVALID, INVALID, nullptr, 0, DefaultVis, 0,
       "Display available options", {}, nullptr, nullptr)
OPTION(1, "-nostdlib", nostdlib, Flag, INVALID, INVALID, nullptr, 0, DefaultVis, 0,
       "Stops the linker from linking the zap stdlib", {}, nullptr, nullptr)
OPTION(1, "-o", output, Separate, INVALID, INVALID, nullptr, 0, DefaultVis, 0,
       "Write output to <file>", {}, nullptr, nullptr)
OPTION(1, "-o", anonymous_0, Joined, INVALID, output, nullptr, 0, DefaultVis, 0, nullptr, {}, nullptr, nullptr)
OPTION(1, "-S", emitS, Flag, INVALID, INVALID, nullptr, 0, DefaultVis, 0,
       "Compile only no assembling or linking", {}, nullptr, nullptr)
OPTION(2, "--version", version, Flag, INVALID, INVALID, nullptr, 0, DefaultVis, 0,
       "Print version information", {}, nullptr, nullptr)
#endif // OPTION

#ifdef SIMPLE_ENUM_VALUE_TABLE

struct SimpleEnumValue {
  const char *Name;
  unsigned Value;
};

struct SimpleEnumValueTable {
  const SimpleEnumValue *Table;
  unsigned Size;
};
static const SimpleEnumValueTable SimpleEnumValueTables[] = {};
static const unsigned SimpleEnumValueTablesSize = std::size(SimpleEnumValueTables);
#endif // SIMPLE_ENUM_VALUE_TABLE


