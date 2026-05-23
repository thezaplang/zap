# Variables and Types

This page documents variable declarations, constants, and core type usage in Zap.

## Variable Declarations (`var`)

Variables are declared with explicit types:

```/dev/null/examples.zp#L1-3
var count: Int = 10;
var name: String = "Zap";
var ready: Bool = true;
```

You can reassign variables as long as the assigned value is type-compatible:

```/dev/null/examples.zp#L1-3
var x: Int = 1;
x = 2;        // OK
x = "hello";  // error: type mismatch
```

---

## Constants (`const`)

Constants must be initialized at declaration time and cannot be reassigned.

```/dev/null/examples.zp#L1-6
const PI: Float = 3.14159;

fun main() {
    const LIMIT: Int = 100;
    // LIMIT = 101; // error: cannot assign to constant
}
```

Constants can be declared globally and locally.

---

## Global Variables

Zap supports global mutable variables using `global var`:

```/dev/null/examples.zp#L1-7
global var counter: Int = 0;

fun inc() Int {
    counter = counter + 1;
    return counter;
}
```

At top level, `global` must be followed by `var`.

---

## Statement Terminators

Zap uses semicolons (`;`) to terminate statements in declarations and assignments:

```/dev/null/examples.zp#L1-4
var a: Int = 1;
var b: Int = 2;
a = a + b;
```

Missing semicolons are parser errors.

---

## Primitive Types

Common built-in scalar types:

- Signed integers: `Int`, `Int8`, `Int16`, `Int32`, `Int64`
- Unsigned integers: `UInt`, `UInt8`, `UInt16`, `UInt32`, `UInt64`
- Floating point: `Float`, `Float32`, `Float64`
- Other: `Bool`, `Char`, `Void`

Examples:

```/dev/null/examples.zp#L1-8
var i: Int = 42;
var u: UInt8 = 255;
var f: Float = 3.5;
var d: Float64 = 1.0;
var ok: Bool = true;
var c: Char = 'A';
```

> `Void` is primarily used as a function return type.

---

## String Type

`String` is a first-class type:

```/dev/null/examples.zp#L1-4
var greeting: String = "hello";
var suffix: Char = '!';
var combined: String = greeting + suffix;
```

Strings can be indexed for reading, but indexed assignment is rejected.

---

## Arrays

Zap arrays are fixed-size and typed: `[N]Type`

```/dev/null/examples.zp#L1-4
var empty: [5]Int;
var nums: [3]Int = {1, 2, 3};
var first: Int = nums[0];
```

Rules:
- Size is part of the type (`[3]Int` differs from `[4]Int`)
- Elements must be type-compatible
- Index expression must be an integer type

---

## Type Aliases

You can create aliases for readability:

```/dev/null/examples.zp#L1-4
alias UserId = Int;

fun load(id: UserId) Int {
    return id;
}
```

---

## Pointers and `unsafe`

Raw pointers are available, but restricted to unsafe-enabled code paths.

```/dev/null/examples.zp#L1-9
fun read_first(ptr: *Int) Int {
    unsafe {
        return *ptr;
    }
}
```

Notes:
- Pointer types use `*Type` syntax.
- Dereference (`*expr`) and address-of operations are unsafe features.
- Cast syntax is `expr as Type` (for example `raw as *Int`).
- C-style casts like `(*Int) raw` are not supported by the parser.
- Unsafe usage is gated by compiler/runtime rules and context checks.

---

## Type Checking and Diagnostics

Zap enforces type compatibility at compile time. Typical errors include:

- Assigning incompatible types
- Using non-`Bool` conditions in `if`/`while`
- Indexing non-indexable types
- Using non-integer indices for arrays

Diagnostics now include stable codes (for example, semantic `S2xxx` and parser `P1xxx` families), making CI assertions and tooling integration easier.

For full list and meaning of codes, see:

- [Diagnostic Codes](diagnostic_codes.md)

---

## Best Practices

- Prefer explicit, domain-friendly aliases for key identifiers.
- Keep globals minimal; prefer function-local state.
- Use unsigned widths intentionally (`UInt8`, `UInt32`, etc.).
- Keep pointer logic isolated and audited in unsafe blocks.
- Treat diagnostic codes as stable contracts in tests.
