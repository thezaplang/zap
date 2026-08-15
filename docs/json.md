# JSON

Zap provides a mutable JSON document model in `std/json`. It supports all JSON
value kinds, strict parsing, typed access, deterministic serialization, and
structured errors.

## Building JSON

Use `JsonObject` and `JsonArray` to build a document without manually escaping
strings:

```zap
import "std/json";

fun payload() String!json.JsonError {
    var user: json.JsonObject = new json.JsonObject();
    user.put("id", 42);
    user.put("name", "Ada");
    user.put("active", true);

    var roles: json.JsonArray = new json.JsonArray();
    roles.push("admin");
    roles.push("author");
    user.put("roles", roles);

    return user.stringify()?;
}
```

`put` and `push` are overloaded for strings, booleans, signed and unsigned
integers, objects, arrays, and `JsonValue`. Use `putNull` or `pushNull` for a
JSON null.

Floating-point overloads are failable because JSON cannot represent NaN or
infinity:

```zap
user.put("ratio", 1.5)?;
```

Object fields are serialized in insertion order. Replacing an existing key
changes its value without moving the key.

## Parsing and typed access

`parse` accepts any top-level JSON value and returns `JsonValue!JsonError`:

```zap
fun readName(source: String) String!json.JsonError {
    var root: json.JsonValue = json.parse(source)?;
    var object: json.JsonObject = root.asObject()?;
    return object.getString("name")?;
}
```

`JsonValue` exposes `kind`, `isString`, `isNumber`, `isBoolean`, `isNull`,
`isObject`, and `isArray`. Its `as...` methods return a typed value or a
`TypeMismatch` error. Objects provide typed `get...` methods; arrays provide
the same operations by index.

Missing keys and invalid indices are errors rather than null sentinels.

## Numbers

Parsed numbers retain their original, validated JSON spelling. This makes the
following round trip lossless even when the value is larger than `UInt`:

```zap
var value: json.JsonValue = json.parse("123456789012345678901234567890")?;
var text: String = value.stringify()?;
```

Use `asNumberText` to obtain that spelling. `asInt` and `asUInt` accept plain
integer notation and check the target range. `asFloat64` accepts decimal and
exponent notation and reports values outside the finite `Float64` range.

`JsonValue.numberText(text)` constructs a number from an already formatted
string and validates the JSON number grammar.

## Errors and depth limits

`JsonError` contains:

- `kind`: a `JsonErrorKind` describing the failure;
- `position`: the byte offset for parser errors, or `-1` for access and
  serialization errors;
- `message`: a human-readable explanation.

Handle errors locally with `or err` or propagate them with `?`:

```zap
var root: json.JsonValue = json.parse(source) or err {
    eprintln("Invalid JSON at byte " + toString(err.position) + ": " + err.message);
    return 1;
};
```

Parsing and serialization default to a maximum depth of 128. Use
`parse(text, maxDepth)` or `stringify(maxDepth)` to choose a different positive
limit. The serializer applies the same limit to programmatically created
cycles, returning `NestingTooDeep` instead of recursing indefinitely.

## Current boundaries

`std/json` does not automatically serialize user structs or classes. Zap does
not currently provide reflection, derive support, or macros needed for that
feature. Pretty printing and JSON literal syntax are also outside the current
API.
