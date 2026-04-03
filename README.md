<h1 align="center">Zap Programming Language</h1>

<p align="center">
  <img src="art/Logo.svg" alt="Zap Logo" width="275" />
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Status-Early%20Alpha-FF9800?style=for-the-badge" />
  <img src="https://img.shields.io/badge/License-Apache%202.0-4CAF50?style=for-the-badge&logo=apache&logoColor=white" />
  <a href="https://github.com/thezaplang/zap">
    <img src="https://img.shields.io/github/stars/thezaplang/zap?style=for-the-badge" />
  </a>
</p>

> Systems programming that doesn't get in your way.

You want predictable performance. No GC pauses. Real enums.
Error handling that doesn't look like noise.

**Zap is a systems language built for developers who know Go
or are ready to step into systems programming.** ARC memory
model, LLVM backend, modern syntax. Write low-level software
without low-level frustration.

[Discord](https://discord.gg/cVGqffBA6m) · [Roadmap](ROADMAP.md)

---

## Why Zap?

> [!WARNING]
> Early alpha — not everything is implemented yet.

| Problem | Zap's answer |
|---|---|
| GC pauses & unpredictable latency | ARC, memory freed deterministically |
| No real enums | Enums with exhaustive pattern matching |
| Verbose error handling | Failable functions |
| Limited generics | Full static generics |
| Weak expression model | `if` as an expression, no ternary needed |
| Single-platform compilers | LLVM: x86, ARM, RISC-V, WASM, embedded |
| No lightweight concurrency | Fibers, like goroutines without the runtime cost |

---

## Error Handling

> [!WARNING]
> Not yet implemented.

Zap uses **failable functions**: functions that can fail declare it explicitly in their return type.

```zap
enum MathError { DivisionByZero, Overflow }

fun divide(a: Int, b: Int): Int!MathError {
    if b == 0 { fail MathError.DivisionByZero; }
    return a / b;
}

fun main(): Int {
    // propagate up with ?
    var x: Int = divide(10, 2)?;

    // fallback value
    var y: Int = divide(10, 0) or 0;

    // handle locally
    var z: Int = divide(10, 0) or err {
        return 1;
    };

    return 0;
}
```

---

## Contributing

Zap is in early alpha. **Your feedback directly shapes the language.**

- open issues
- discuss language design
- implement features
- improve diagnostics
- write documentation

---

## Star History

<a href="https://www.star-history.com/?repos=thezaplang%2Fzap&type=date&legend=top-left">
 <picture>
   <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/image?repos=thezaplang/zap&type=date&theme=dark&legend=top-left" />
   <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/image?repos=thezaplang/zap&type=date&legend=top-left" />
   <img alt="Star History Chart" src="https://api.star-history.com/image?repos=thezaplang/zap&type=date&legend=top-left" />
 </picture>
</a>
