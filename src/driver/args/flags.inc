/// This file includes the flags for the zapc driver.

#ifndef ZAP_FLAG
#define ZAP_FLAG(...)
#endif

/// The macro itself follows a simple format:
/// ZAP_FLAG(
///   ID          /* Name of the Enum                               */
///   Flag        /* The flag's string                              */
///   HelpMessage /* The message that displays when --help is used. */
///   Kind        /* Kind of the arg (Flag, Joined, etc..)          */
/// )

// --help
ZAP_FLAG(Help, "--help", "Displays available options.", Flag)

// --version
ZAP_FLAG(Version, "--version", "Prints out the version information.", Flag)

// --print-stdlib-path
ZAP_FLAG(PrintStdlibPath, "--print-stdlib-path",
         "Prints the resolved standard library directory.", Flag)

// --print-core-path
ZAP_FLAG(PrintCorePath, "--print-core-path",
         "Prints the resolved core library directory.", Flag)

// -o
ZAP_FLAG(Output, "-o", "Sets the output file.", JoinedSeparate)

// -nostdlib
ZAP_FLAG(NoStdlib, "-nostdlib",
         "Stops the linker from linking the standard library.", Flag)

// -noprelude
ZAP_FLAG(NoPrelude, "-noprelude",
         "Disables implicit injection of the standard prelude library.", Flag)

// --freestanding
ZAP_FLAG(Freestanding, "--freestanding",
         "Compile without host OS runtime assumptions.", Flag)

// -c
ZAP_FLAG(CompileOnly, "-c", "Compile and assemble but not link.", Flag)

// -S
ZAP_FLAG(CompileOnlyS, "-S", "Compile only no assemble or link.", Flag)

// -emit-llvm
ZAP_FLAG(EmitLLVM, "-emit-llvm", "Emit LLVM IR as the output format.", Flag)

// -emit-zir
ZAP_FLAG(EmitZIR, "-emit-zir", "Emit ZIR as the output format.", Flag)

// --allow-unsafe
ZAP_FLAG(AllowUnsafe, "--allow-unsafe",
         "Enables unsafe blocks, unsafe functions, and raw pointers. (deprecated, unsafe allowed by default)", Flag)

// -l
ZAP_FLAG(LinkLib, "-l", "Link with library <name> (forwarded to the linker).",
         JoinedSeparate)

// -L
ZAP_FLAG(LinkDir, "-L",
         "Add <dir> to library search path (forwarded to the linker).",
         JoinedSeparate)

// -O
ZAP_FLAG(OptLevel, "-O", "Set optimization level (default: -O0).", Joined)

// --target=
ZAP_FLAG(Target, "--target=", "Set LLVM target triple.", Joined)

// --import-map
ZAP_FLAG(ImportMap, "--import-map",
         "Add an import alias mapping (alias=path). May be repeated.",
         JoinedSeparate)
