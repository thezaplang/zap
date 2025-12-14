# Ignis Programming Language

<p align="center">
  <img src="art/Logo.png" alt="Ignis Logo" width="200" />
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Status-Pre--alpha-FF5722?style=for-the-badge" alt="Status" />
  <img src="https://img.shields.io/badge/License-MIT-4CAF50?style=for-the-badge" alt="License" />
  <img src="https://img.shields.io/github/languages/code-size/ignislang/ignis?style=for-the-badge" alt="Code size" />
  <a href="https://github.com/ignislang/ignis"><img src="https://img.shields.io/github/stars/ignislang/ignis?style=for-the-badge" alt="Stars" /></a>
  <a href="https://github.com/ignislang/ignis"><img src="https://img.shields.io/github/forks/ignislang/ignis?style=for-the-badge" alt="Forks" /></a>
</p>

## 🔥 Why Ignis?

**You need Ignis if:**

- You're **maintaining legacy C code** and want modern syntax without rewriting everything
- You want **instant interoperability** with existing C libraries (no need for FFI)
- You're building **performance-critical tools** where every millisecond matters but Python/Go are too slow
- You need **small binary files**

**Ignis is the language you use when:**

- You need C's performance but not C's pain
- Rust's borrow checker is overkill but C's unsafety keeps you up at night
- You're tired of metaprogramming nightmares in C++
- You need to **drop into a codebase, write code, and ship it** - not battle the compiler

## Table of Contents

- [Why Ignis?](#why-ignis)
- [Key Advantages](#key-advantages)
- [Features](#features)
- [Getting Started](#getting-started)
  - [Prerequisites](#prerequisites)
  - [Installation](#installation)
  - [Building from Source](#building-from-source)
  - [Compiling to C](#compiling-to-c)
  - [Running Programs](#running-programs)
- [Architecture](#architecture)
- [Contributing](#contributing)
- [License](#license)

## Features

- **Modern syntax** with clear, readable code
- **Compiles to C** for maximum compatibility and performance
- **Static typing** with type inference support
- **Standard library(WIP)** for common operations
- **CLI toolchain** for easy compilation and management
- **Early error detection(WIP)** with comprehensive parsing and semantic analysis

## Key Advantages

### Head-to-Head Comparison

| Feature               | C        | C++       | Rust      | Go      | Ignis     |
| --------------------- | -------- | --------- | --------- | ------- | --------- |
| **Learning Time**     | 2 months | 6+ months | 4+ months | 2 weeks | 2-3 weeks |
| **Compilation Speed** | Instant  | Slow      | Very Slow | Fast    | Very Fast |
| **Binary Size**       | Small    | Large     | Large     | Medium  | Small     |
| **Runtime Overhead**  | None     | Medium    | None      | Medium  | None      |
| **Type Inference**    | ❌       | Partial   | ✅        | ✅      | ✅        |
| **C Interop**         | Native   | Complex   | FFI       | Cgo     | Direct    |
| **Embedded Ready**    | ✅       | ⚠️        | ⚠️        | ❌      | ✅        |

### Why Choose Ignis Over...

**C** → Type safety + modern syntax without sacrificing a single nanosecond of performance. Stop debugging memory corruption at 3 AM.

**C++** → All the power, 70% less boilerplate, faster compilation, no template metaprogramming nightmares. Write real code instead of fighting the compiler.

**Rust** → Same performance, but learn Ignis in days instead of months. You get practical safety through type checking without the borrow checker complexity. Perfect for systems programming without the steep learning curve.

**Go** → Compiled to native machine code, true zero-cost abstractions, smaller binaries. Performance that actually matters for embedded and systems programming.

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

### Hello World

**hello.ign:**

```ignis
fn writeLn(str: string, ...) -> i32;

fn main() -> i32 {
    writeLn("Hello, World!");
    return 0;
}
```

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

Compile and run:

```bash
./Ignis -cc hello.ign hello.c
gcc -o hello hello.c ignis_std.c
./hello
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

| Phase         | feature                                     | Status |
| ------------- | ------------------------------------------- | ------ |
| **pre-alpha** | Basic function definitions                  | ✔️     |
|               | Function calls with arguments               | ✔️     |
|               | Variable declarations                       | ✔️     |
|               | Return statements                           | ✔️     |
|               | Lexer, Parser, Semantic Analyzer            | ✔️     |
|               | C code generation                           | ✔️     |
|               | Control flow (if/else, loops)               | ✔️     |
|               | Operators (arithmetic, logical, comparison) | ✔️     |
|               | Arrays and pointers                         | ⚒️     |
|               | Structs and enums                           | ⚒️     |
|               | Error handling                              | ⏳     |
|               | More type support                           | ⏳     |
|               | Modules and imports                         | ⏳     |
|               | Generics/Templates                          | ⏳     |
| **1.0**       | Direct compilation (LLVM)                   | ⏳     |
|               | Standard library                            | ⚒️     |
|               | Traits/Interfaces)                          | ⏳     |
|               | Pattern matching                            | ⏳     |

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

---

For questions or issues, please open a GitHub issue.
