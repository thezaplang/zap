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
payload, and `_` to ignore one.

```zap
enum Result {
    Empty,
    Value(Int),
    Error(String),
}

fun printResult(result: Result) {
    case result {
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

## Current limits

Zap 0.4.1 does not support ranges, guards, nested patterns, record or array
destructuring, float patterns, named constants as patterns, or `case` as an
expression. A payload variant has one payload value, and an arm that binds it
cannot combine alternatives such as `Result.Value(x), Result.Empty()`.
