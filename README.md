<h1 align="center">Zap Programming Language</h1>
<br>
<p>
  <img src="art/Logo.svg" alt="Zap Logo" width="220"  align="left"/>
</p>
Systems programming that doesn't get in your way.

You want predictable performance. No GC pauses. Real enums.
Error handling that doesn't look like noise.

**Zap is a systems language built for developers who know Go
or are ready to step into systems programming.** ARC memory
model, LLVM backend, modern syntax. Write low-level software
without low-level frustration.
<br>
[Discord](https://discord.gg/cVGqffBA6m) · [Roadmap](ROADMAP.md) · [Documentation](docs/README.md)
<br clear="left">
<br>
<br>
## Why Zap?


<ul>
<li><b>deterministic memory management</b>: Even though Zap has automatic memory management, it deletes objects exactly when they are no longer needed.</li>
<li><b>productivity</b>: Zap allows you to use an object-oriented approach, has a very fast compile-time, and has its own <a href="https://github.com/thezaplang/thor">build tool</a>
<li><b>performance</b>: Zap, thanks to being free from the bloat of other languages, has a very fast compile time, and, thanks to LLVM and its own IR, the resulting application is really efficient.</li>
</ul>

<br>

## Cycles

If you know what ARC is, you probably also know that when two objects point to each other, ARC will never delete them even though they are not needed.
Swift added `weak` to prevent this. We also added weak in Zap, but it often happens that you simply don't see that there is a cycle somewhere.
Zap solves this problem thanks to its Cycle Collector, i.e., it is actually a garbage collector operating on objects causing the cycle, REST OF OBJECTS ARE NOT AFFECTED (cycle collector can be disabled if someone needs it!)

---

## Error Handling

Zap uses **failable functions**: functions that can fail declare it explicitly in their return type.

```zap
@error
enum MathError { DivisionByZero, Overflow }

fun divide(a: Int, b: Int) Int!MathError {
    if b == 0 { fail MathError.DivisionByZero; }
    return a / b;
}

fun main() Int {
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

## Documentation

Start here: **[docs/README.md](docs/README.md)**

Highlights:
- Language guide: variables, functions, control flow, data structures, classes, memory
- Generics coverage: function/type generics, constraints (`where`), and compile-time `iftype` (see docs sections and tests)
- Diagnostic code reference: **[docs/diagnostic_codes.md](docs/diagnostic_codes.md)**

## Contributing

Zap is in early alpha. **Your feedback directly shapes the language.**

- open issues
- discuss language design
- implement features
- improve diagnostics
- write documentation
