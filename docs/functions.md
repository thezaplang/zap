# Functions

Functions are declared with the `fun` keyword.

Zap supports:

- Explicit parameter types
- Optional return type (omitted => `Void`)
- Generic functions
- Overloading
- Named arguments
- Variadic parameters (`...T`) and C variadics (`...`)
- `ref` parameters
- Methods on classes

---

## Basic Function Declaration

Return type is written **after** the parameter list (no `:` before return type).

```zap
fun add(a: Int, b: Int) Int {
    return a + b;
}
```

If a function does not return a value, omit the return type:

```zap
fun log_message(text: String) {
    // ...
}
```

Equivalent explicit form:

```zap
fun log_message(text: String) Void {
    // ...
}
```

---

## Calling Functions

Standard call syntax:

```zap
var total: Int = add(10, 20);
```

### Builtin `sizeof`

`sizeof(x)` returns the size in bytes as an `Int` compile-time constant. The
argument can be a type name or an expression:

```zap
var intBytes: Int = sizeof(Int32); // 4

struct Packet {
    tag: UInt8,
    value: Int32,
}

var packet: Packet = Packet{tag: 1, value: 2};
var packetBytes: Int = sizeof(packet);
```

---

## Parameters

### By Value (default)

```zap
fun inc(x: Int) Int {
    return x + 1;
}
```

### By Reference (`ref`)

Use `ref` when the callee should modify caller-owned data:

```zap
fun increment(ref x: Int) {
    x = x + 1;
}

fun main() Int {
    var n: Int = 10;
    increment(ref n);
    return n; // 11
}
```

Notes:

- `ref` must match on both declaration and call site.
- `ref` arguments must be assignable values (l-values).
- Passing incompatible ref/value style is a semantic error.

---

## Return Values

Use `return expr;` in non-void functions.

```zap
fun max(a: Int, b: Int) Int {
    if a > b {
        return a;
    }
    return b;
}
```

For `Void` functions, `return;` is optional but allowed.

---

## Function Overloading

Zap supports multiple functions with the same name but different signatures.

```zap
fun area(r: Float) Float {
    return 3.14159 * r * r;
}

fun area(w: Float, h: Float) Float {
    return w * h;
}
```

Resolver prefers the best type-compatible candidate. Ambiguous calls are rejected.

---

## Named Arguments

You can pass arguments by name (when supported by the function signature).

```zap
fun clamp(value: Int, low: Int, high: Int) Int {
    if value < low { return low; }
    if value > high { return high; }
    return value;
}

var x: Int = clamp(value: 120, low: 0, high: 100);
```

Rule: positional arguments cannot follow named arguments.

---

## Generic Functions

Declare type parameters after function name.

```zap
fun identity<T>(x: T) T {
    return x;
}

var a: Int = identity<Int>(42);
var b: String = identity("zap");
```

Type arguments may often be inferred from call arguments.

---

## Variadic Functions

### Zap Variadic Packs (`...T`)

```zap
fun sum(...Int) Int {
    var total: Int = 0;
    // implementation-specific traversal
    return total;
}
```

### C Variadics (`...` in `extern`)

Used for FFI-style variadic calls in external declarations.

---

## Recursion

Fully supported:

```zap
fun fact(n: Int) Int {
    if n <= 1 {
        return 1;
    }
    return n * fact(n - 1);
}
```

---

## Methods (Class Member Functions)

Inside classes, methods are declared with `fun`. Instance methods receive `self` implicitly.

```zap
class Counter {
    priv value: Int;

    fun init(v: Int) {
        self.value = v;
    }

    pub fun inc(step: Int) Int {
        self.value = self.value + step;
        return self.value;
    }
}
```

---

## Practical Notes

- End statements with `;`.
- Keep return types explicit on public APIs for readability.
- Prefer small functions with clear parameter names.
- Use `ref` only when mutation is intentional and necessary.
- Overloads should remain unambiguous to avoid confusing call sites.
- If a non-void function misses return paths, compiler emits a warning.

---
