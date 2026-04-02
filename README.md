<h1 align="center"> Zap Programming Language </h1>

<p align="center">
  <img src="art/Logo.svg" alt="Zap Logo" width="275" />
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Status-Early%20Alpha-FF9800?style=for-the-badge" alt="Status" />
  <img src="https://img.shields.io/badge/License-Apache%202.0-4CAF50?style=for-the-badge&logo=apache&logoColor=white" alt="License" />
  <a href="https://github.com/zap-lang-org/zap"><img src="https://img.shields.io/github/stars/thezaplang/zap?style=for-the-badge" alt="Stars" /></a>
</p>

> **TL;DR:** Zap is something like Go with its pain points fixed: better error handling, real enums with pattern matching, practical generics, if-expressions, and full target support via LLVM.

---

[Discord](https://discord.gg/cVGqffBA6m)

## v0.1.0 ROADMAP
[Roadmap](ROADMAP.md)

## What is Zap?

**Zap** is a modern, high-level systems programming language compiled to native code (**LLVM backend**), using **Automatic Reference Counting (ARC)** instead of a Garbage Collector (GC).

Zap is built for developers who want to write **high-performance applications** -servers, CLI tools, tooling, and embedded software **quickly, safely, and without frustration**.

Zap behaves very similarly to Go **by design**. It does not try to reinvent systems programming. Its goal is to **solve the real, long-standing problems of Go**.

---

## What does Zap improve over Go?
> [!NOTE]
> not everything works, the language is in early alpha, and this is just a preview of the language

### Error handling

- `try / catch / throw`

No more `if err != nil` everywhere.

---

### Enums

- real enums
- exhaustive pattern matching (`match`, `each`)

Correctness enforced by the compiler.

---

### Generics

- full and static
- comptime-inspired
- simple, predictable, and powerful

---

### If-expressions / ternary

```zap
x = if cond { a } else { b }
```

Less boilerplate, clearer intent.

---

### Targets & optimization

- LLVM backend
- small binaries
- fast cold starts
- targets: x86, ARM, RISC-V, WebAssembly, embedded

---


## Contributing

Zap is at an early stage **feedback directly shapes the language**.

You can help by:

- opening issues
- discussing language design
- implementing features
- improving diagnostics
- writing documentation

Repository:
[https://github.com/thezaplang/zap](https://github.com/thezaplang/zap)

---

## Star History

<a href="https://www.star-history.com/?repos=thezaplang%2Fzap&type=date&legend=top-left">
 <picture>
   <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/image?repos=thezaplang/zap&type=date&theme=dark&legend=top-left" />
   <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/image?repos=thezaplang/zap&type=date&legend=top-left" />
   <img alt="Star History Chart" src="https://api.star-history.com/image?repos=thezaplang/zap&type=date&legend=top-left" />
 </picture>
</a>


**Zap**

> Go, without Go problems
