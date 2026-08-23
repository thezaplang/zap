# Pattern Matching with `case`

`case` selects the first arm whose pattern matches a scrutinee expression. It
is a statement: after an arm completes, control continues after the whole
`case`. There is no implicit fallthrough and `break` retains its normal loop
meaning.

```zap
case value {
    0, 1 {
        println("small");
    }
    else {
        println("other");
    }
}
```

The scrutinee is evaluated once, before any pattern tests. An `else` arm is a
wildcard and must be last.

## Literal patterns

Zap supports integer, `Bool`, `Char`, and `String` literals as patterns. The
literal must be representable by the type of the scrutinee.

```zap
fun describe(enabled: Bool, letter: Char) {
    case enabled {
        true { println("enabled"); }
        else { println("disabled"); }
    }

    case letter {
        'y' { println("yes"); }
        else { println("another letter"); }
    }
}
```

Literal cases require `else`, because their set of possible values is open.

## Enum variants

For ordinary enums, name the variant with its enum type. A `case` that covers
every declared variant is exhaustive, so it does not need `else`.

```zap
enum Color { Red, Green, Blue }

fun label(color: Color) String {
    case color {
        Color.Red { return "red"; }
        Color.Green { return "green"; }
        Color.Blue { return "blue"; }
    }
}
```

Zap rejects duplicate variants, variants belonging to another enum, and an
`else` arm after all variants have already been covered.

## Enum payloads

Enums with payloads use `()` for an empty variant, an identifier to bind one
payload, `_` to ignore one, a literal to match it, or a record pattern to
destructure a record/`struct` payload.

```zap
enum Result {
    Empty,
    Value(Int),
    Error(String),
}

fun printResult(result: Result) {
    case result {
        Result.Value(42) {
            println("answer");
        }
        Result.Empty() {
            println("nothing");
        }
        Result.Value(value) {
            println(value);
        }
        Result.Error(_) {
            println("failed");
        }
    }
}
```

The payload binding is an immutable local available only in its arm. Zap tests
the variant tag before accessing a payload, so a payload is never read from a
non-matching variant.

For a record payload, place its record pattern inside the variant pattern.
Its field bindings are immutable locals available in the arm. A binding-only
record payload pattern covers the whole variant.

```zap
struct User { id: Int, score: Int }
enum Result { Empty, Value(User) }

fun score(result: Result) Int {
    case result {
        Result.Value(User { id: 1, score }) { return score; }
        Result.Value(User { id, score }) { return id + score; }
        Result.Empty() { return 0; }
    }
}
```

## Record patterns

Records and `struct`s can be destructured by field. A bare field name binds it
as an immutable variable; a field followed by `:` may match a literal, an enum
variant (including a tagged-union payload binding), or a nested record pattern.
Field constraints do not contribute to exhaustiveness yet, so they need an
irrefutable record arm or `else`.

```zap
record Point { x: Int, y: Int }

case point {
    Point { x: 0, y } { println(y); }
    Point { x, y } { println(x + y); }
}
```

Because a record pattern names the exact scrutinee type, an arm that only
binds fields is exhaustive. More specific field patterns must appear first.

```zap
record Response { result: Result, code: Int }

case response {
    Response { result: Result.Value(value), code } {
        println(value + code);
    }
    Response { result: Result.Empty(), code } {
        println(code);
    }
    else {}
}
```

## Current limits

Zap 0.4.1 does not support ranges, guards, array destructuring, float
patterns, named constants as patterns, or `case` as an expression. A payload
variant has one payload value, and an arm that binds or destructures it cannot
combine alternatives such as `Result.Value(x), Result.Empty()`. Within a
record field, literal and enum patterns may be used. Array destructuring and
guards are not supported yet.
