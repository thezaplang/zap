# Ignis Programming Language

> **Status**: Pre-alpha 0.1 - Early-stage development

Ignis is a modern compiled programming language designed with a focus on clarity, efficiency, and type safety. It compiles to C, leveraging the mature C ecosystem while providing a more expressive syntax.

## Table of Contents

- [Features](#features)
- [Getting Started](#getting-started)
  - [Prerequisites](#prerequisites)
  - [Installation](#installation)
  - [Building from Source](#building-from-source)
- [Language Syntax](#language-syntax)
  - [Functions](#functions)
  - [Variables](#variables)
  - [Types](#types)
  - [Function Calls](#function-calls)
- [Usage](#usage)
  - [Compiling to C](#compiling-to-c)
  - [Running Programs](#running-programs)
- [Examples](#examples)
- [Architecture](#architecture)
- [Project Structure](#project-structure)
- [Contributing](#contributing)
- [License](#license)

## Features

- ✨ **Modern syntax** with clear, readable code
- 🔄 **Compiles to C** for maximum compatibility and performance
- 🛡️ **Static typing** with type inference support
- 📦 **Standard library** for common operations
- 🔧 **CLI toolchain** for easy compilation and management
- 🎯 **Early error detection** with comprehensive parsing and semantic analysis

## Getting Started

### Prerequisites

- **C++17** compiler (GCC, Clang, or MSVC)
- **CMake 3.31.6** or higher
- **C compiler** (GCC, Clang) to compile generated C code

### Installation

Ignis is still in early development. To use it, you'll need to build from source.

### Building from Source

```bash
# Clone the repository
git clone https://github.com/funcieqDEV/ignis.git
cd ignis

# Create build directory
mkdir build
cd build

# Configure and build
cmake ..
make

# Run Ignis
./Ignis -h
```

## Language Syntax

### Functions

Define functions with the `fn` keyword:

```ignis
fn greet(name: string) -> void{
    writeLn("Hello, " + name);
}
```

**Forward declaration**:
```ignis
fn writeLn(str: string, ...) -> i32;
```

### Variables

Declare mutable variables with `let`:

```ignis
fn main() -> i32 {
    let count: i32 = 42;
    let message: string = "Hello, World!";
    return 0;
}
```

### Types

Ignis supports the following primitive types:

| Type | Description | C Equivalent |
|------|-------------|--------------|
| `i32` | 32-bit signed integer | `int32_t` |
| `f32` | 32-bit floating-point | `float` |
| `string` | String | `const char*` |
| `bool` | Boolean | `bool` |
| `char` | Character | `char` |
| `void` | No type | `void` |

### Function Calls

Call functions with arguments:

```ignis
fn main() -> i32 {
    writeLn("Hello, World!");
    writeLn("Value: ", 42);
    return 0;
}
```

**Variadic arguments** are supported using `...`:
```ignis
fn printf(format: string, ...) -> i32;
```

## Usage

### Compiling to C

Generate C code from an Ignis source file:

```bash
./Ignis -cc input.ign output.c
```

This produces a standalone C file that can be compiled with any C compiler.

### Running Programs

After generating C code, compile with a C compiler:

```bash
gcc -o program output.c ignis_std.c
./program
```

## Examples

### Hello World

**hello.ign:**
```ignis
fn writeLn(str: string, ...) -> i32;

fn main() -> i32 {
    writeLn("Hello, World!");
    return 0;
}
```

Compile and run:
```bash
./Ignis -cc hello.ign hello.c
gcc -o hello hello.c ignis_std.c
./hello
```

### Working with Variables

```ignis
fn main() -> i32 {
    let x: i32 = 10;
    let y: i32 = 20;
    let sum: i32 = x + y;
    writeLn("Sum: ", sum);
    return 0;
}
```

## Architecture

Ignis uses a multi-stage compilation pipeline:

```
Source Code (.ign)
      ↓
   Lexer (Tokenization)
      ↓
   Parser (AST Generation)
      ↓
Semantic Analyzer (Type Checking)
      ↓
Code Generator (C Emission)
      ↓
   C Code (.c)
      ↓
C Compiler (GCC/Clang)
      ↓
  Executable
```

### Components

- **Lexer** (`frontend/lexer/`) - Tokenizes source code
- **Parser** (`frontend/parser/`) - Builds Abstract Syntax Tree (AST)
- **Semantic Analyzer** (`frontend/sema/`) - Type checking and validation
- **Code Generator** (`backend/C/`) - Emits C code
- **AST** (`frontend/ast/`) - Node definitions and arena allocator

## Contributing

Contributions are welcome! As Ignis is in early development, there are many opportunities to help:

- Add new language features
- Improve error messages
- Optimize code generation
- Expand standard library
- Write documentation
- Fix bugs

To contribute:

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes (`git commit -m 'Add amazing feature'`)
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

## Roadmap

### Phase 1 (Current)
- [x] Basic function definitions
- [x] Function calls with arguments
- [x] Variable declarations
- [x] Return statements
- [x] Lexer, Parser, Semantic Analyzer
- [x] C code generation

### Phase 2 (Planned)
- [x] Control flow (if/else, loops)
- [x] Operators (arithmetic, logical, comparison)
- [ ] Arrays and pointers
- [ ] Structs and enums
- [ ] Error handling
- [ ] More type support

### Phase 3 (Future)
- [ ] Modules and imports
- [ ] Generics/Templates
- [ ] Traits/Interfaces
- [ ] Pattern matching
- [ ] Direct compilation (without C intermediate)

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

---

For questions or issues, please open a GitHub issue.
