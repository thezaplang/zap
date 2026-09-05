<p align="center">
  <img src="art/Logo.svg" alt="Zap logo" width="220" />
</p>

<h1 align="center">Zap Programming Language</h1>

<p align="center">Systems programming that doesn't get in your way.</p>

<p align="center">
  <a href="https://discord.gg/tfbE5Cps5j">Discord</a> ·
  <a href="ROADMAP.md">Roadmap</a> ·
  <a href="https://zaplang.xyz">Website</a> ·
  <a href="https://zaplang.xyz/getting-started/">Documentation</a>
</p>

## What is Zap?

Zap is a systems programming language created to bring back the joy of
programming. Zap provides safe, automatic memory management without the Stop the world behavior typical of garbage collectors.
Instead, Zap uses predictable and deterministic ownership-aware ARC.

Zap is an OOP language, so it allows you to design your code around clean, well-defined models.

## The main assumptions of the language

- **Programming should feel good.** The language should stay approachable and
  let you focus on solving the problem.
- **Memory management must be predictable.** Automatic memory management
  should not introduce unexpected global pauses.
- **Safety belongs in everyday code.** Safe defaults should make the common
  path reliable without turning programming into ceremony.
- **Code should scale with the project.** Object-oriented design, strong types,
  and explicit control flow help keep programs understandable as they grow.
- **Systems programming should stay practical.** Zap targets native software
  while keeping the language pleasant to use.

## Key features

- **Error handling** with explicitly failable functions.
- **Pattern matching** *(WIP)*.
- **Classes** with inheritance and polymorphism.
- **Attributes** for expressing compiler and runtime intent.
- **Macros** *(WIP)*.
- **Ownership-aware ARC** for automatic, predictable memory management.

## Support

| Platform | Architecture | Status |
| --- | --- | --- |
| Linux | x86_64 | Officially supported |
| Linux | AArch64 | Works; not officially supported yet |

## First steps

Install the Zap toolchain with `zapup`:

```bash
curl --proto '=https' --tlsv1.2 -sSf https://zaplang.xyz/install.sh | sh
```

Create and run your first project with [Thor](https://github.com/thezaplang/thor):

```bash
thor new world
cd world
thor run
```

## Compiling from source

To build Zap yourself, you need:

- Clang 15+ or GCC 12.1+.
- LLVM 21 development libraries.
- Meson and Ninja.
- OpenSSL development libraries.
- Python 3.

Clone the repository and run the build script:

```bash
git clone https://github.com/thezaplang/zap.git
cd zap
./build.sh
```

The compiler will be available as `build/zapc`.

## Contributing

Zap is in early alpha, and feedback directly shapes the language. You can
report bugs, discuss language design, improve documentation, or contribute
code. Start with the [contribution guide](CONTRIBUTING.md), then join us on
[Discord](https://discord.gg/tfbE5Cps5j).
