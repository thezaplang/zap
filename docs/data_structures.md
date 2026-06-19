# Data Structures

Zap provides first-class support for core data structures used in systems programming:

- **Arrays** for fixed-size homogeneous collections
- **Records/Structs** for named field aggregates
- **Enums** for closed sets of named variants

This page reflects currently supported syntax and behavior.

---

## Arrays

Arrays are fixed-size and strongly typed. The size is part of the type.

```/dev/null/examples.zp#L1-4
var a: [5]Int;
var b: [3]Int = {1, 2, 3};
var c: [4]UInt8 = {1, 2, 3, 4};
var first: Int = b[0];
```

### Rules

- Type syntax is `[N]T`
- `N` is the declared compile-time size
- All elements in an initializer must be type-compatible
- Index expression must be an integer type
- Indexing is zero-based

### Common diagnostics

- Element type mismatch:
  - `Array elements must have the same type. Expected 'Int', but got 'String'`
- Invalid index type:
  - `Array index must be an integer, but got 'Bool'`
- Indexing unsupported type:
  - `Type 'Int' does not support indexing.`

---

## Records and Structs

Zap supports user-defined aggregate data types with named fields.

## `record`

Use `record` for named product types:

```/dev/null/examples.zp#L1-8
record Person {
    name: String,
    age: Int,
    email: String
}

fun age_of(p: Person) Int {
    return p.age;
}
```

## `struct`

`struct` is also available for field-based aggregate modeling:

```/dev/null/examples.zp#L1-8
struct Vec2 {
    x: Float,
    y: Float
}

fun length_sq(v: Vec2) Float {
    return v.x * v.x + v.y * v.y;
}
```

> Both forms are supported in current Zap tooling/tests. Use project conventions consistently.

### Packed layout

Use `@{packed}` on a struct when its LLVM representation must omit layout padding between fields:

```/dev/null/examples.zp#L1-5
@{packed}
struct Packet {
    tag: UInt8,
    value: Int32,
}
```

Packed layout is intended for ABI- or byte-layout-sensitive data. It should be used deliberately because unaligned field access may be slower or more constrained on some targets.

### Struct/record literals

Fielded aggregate literals are supported in typed contexts (for example where the target type is known), with rules enforced by semantic analysis:

- field must exist on the target type
- value must be assignable to field type
- required fields must be initialized

Typical diagnostics include:

- `Field 'foo' not found in struct 'TypeName'`
- `Cannot assign type 'X' to field 'foo' of type 'Y'`
- `Field 'bar' of struct 'TypeName' is not initialized.`

---

## Enums

Enums define a closed set of named variants.

```/dev/null/examples.zp#L1-6
enum Color { Red, Green, Blue }

fun is_red(c: Color) Bool {
    return c == Color.Red;
}
```

Trailing commas in enum declarations are accepted in current compiler behavior.

```/dev/null/examples.zp#L1-4
enum Size {
    Small,
    Medium,
    Large,
}
```

### Notes

- Enum variants are accessed via member syntax (`EnumName.Variant`)
- Enums are statically typed and checked at compile time

---

## Choosing Between Them

- Use **arrays** when:
  - size is fixed and known
  - you need contiguous homogeneous storage

- Use **record/struct** when:
  - you need a custom type with named fields

- Use **enum** when:
  - value must be one of a known finite set

---

## Related Documentation

- [Variables and Types](variables.md)
- [Functions](functions.md)
- [Control Flow](control_flow.md)
- [Classes](classes.md)
- [Diagnostic Codes](diagnostic_codes.md)

---
