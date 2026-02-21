# Gemini Project Context: Zap Programming Language

This document provides context for the Gemini AI assistant to understand and assist with the development of the Zap programming language project.

## Project Overview

This project contains the source code for the **Zap compiler (`zapc`)**. Zap is a modern, high-level systems programming language designed to be a successor to Go, addressing some of its perceived pain points.

- **Language:** C++17
- **Core Technology:** The compiler uses the **LLVM** framework for its backend, enabling compilation to native code for various targets.
- **Architecture:** The compiler follows a standard architecture:
    - **Lexer (`lexer/`):** Tokenizes Zap source code.
    - **Parser (`parser/`):** Constructs an Abstract Syntax Tree (AST) from the tokens.
    - **AST (`ast/`):** Defines the tree structure representing the code's logic.
- **Source Language:** The language being compiled is named "Zap" and files use the `.zap` extension.

## Building and Running

The project is built using **CMake**. A helper script is provided for convenience.

### Building the Compiler

To build the `zapc` executable, run the main build script:

```sh
./build.sh
```

This command will:
1.  Create a `build/` directory.
2.  Run `cmake` to configure the project.
3.  Compile the source code.
4.  Place the final executable at `build/zapc`.

### Running the Compiler

The compiler is run from the command line, targeting a `.zap` file.

```sh
./build/zapc example/test.zap
```

The main entry point (`main.cpp`) handles command-line argument parsing for options like `--help`, `--version`, and `-o` for specifying an output file.

## Development Conventions

### Code Formatting

The project uses `clang-format` to maintain a consistent code style. To format all C++ source and header files, run the formatting script:

```sh
./fmt.sh
```

### hints
in this language every type start with capital letter so there is no `int` but there is `Int`

### Contribution Guidelines

The project has standard contribution guidelines, as indicated by the presence of `CONTRIBUTING.md` and `CODE_OF_CONDUCT.md` files. Please adhere to the practices outlined in these documents when making changes.
