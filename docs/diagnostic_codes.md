# Zap Diagnostic Codes Reference

This document describes the diagnostic codes emitted by the Zap compiler and Zap language server.

Diagnostic codes are designed to be:

- **Stable identifiers** for classes of issues
- **Searchable** in CI logs and IDEs
- **Useful for automation** (tests, lint gates, triage)

---

## Diagnostic Format

Human-readable compiler output typically looks like:

error: S2002 Cannot assign expression of type 'String' to type 'Int'
 --> path/to/file.zp:12:3
 12 |   n = text;
      ^^^^^^^^

In LSP diagnostics, the code is available in the `code` field.

---

## Code Families

- `P1xxx` — **Parser** diagnostics
- `S2xxx` — **Semantic** diagnostics
- `W1xxx` — **Warnings**
- `N1xxx` — **Notes / help / informational**
- `E0000` — **Generic fallback error** (uncategorized)
- `W0000`, `N0000` — **Generic fallback warning/note**

---

## Parser Diagnostics (`P1xxx`)

### `P1001` — Missing statement terminator (`;`)
Emitted when parser expected `;` and did not find it.

Typical messages:
- `Expected ';' ...`

---

### `P1002` — Missing primary expression
Emitted when an expression was expected, but token stream had no valid primary expression.

Typical messages:
- `Expected primary expression, got ...`

---

### `P1003` — Unexpected token
Emitted for token sequences that cannot be parsed in the current context.

Typical messages:
- `Unexpected token ...`

---

### `P1004` — Token mismatch (expected vs got)
Emitted for general parser mismatches where parser expected one token and found another.

Typical messages:
- `Expected X, but got Y`

---

### `P1005` — Unexpected end of file
Emitted when parser expected more input but reached EOF.

Typical messages:
- `... but reached end of file`

---

## Semantic Diagnostics (`S2xxx`)

### `S2001` — Undefined identifier
Emitted when a referenced symbol does not exist in visible scope.

Typical messages:
- `Undefined identifier: ...`

---

### `S2002` — Assignment/type compatibility error
Emitted when value type cannot be assigned to target type.

Typical messages:
- `Cannot assign expression of type '...' to type '...'`
- `Cannot assign type '...' to field '...' of type '...'`

---

### `S2003` — Condition must be `Bool`
Emitted when `if` / `while` / ternary condition is not boolean.

Typical messages:
- `If condition must be Bool, got '...'`
- `While condition must be Bool, got '...'`
- `Ternary condition must be Bool, got '...'`

---

### `S2004` — Invalid comparison types
Emitted when operands of comparison are incompatible.

Typical messages:
- `Cannot compare '...' and '...'`

---

### `S2005` — Operator not applicable to operand types
Emitted when an operator cannot be used with provided operand type(s).

Typical messages:
- `Operator '+' cannot be applied to '...' and '...'`
- `Operator '!' cannot be applied to type '...'`

---

### `S2006` — Invalid cast
Emitted when explicit cast is not allowed between source and target types.

Typical messages:
- `Cannot cast from '...' to '...'`

---

### `S2007` — Type does not support indexing
Emitted for indexing on non-indexable types.

Typical messages:
- `Type '...' does not support indexing.`

---

### `S2008` — Non-integer index
Emitted when index expression is not integer-like.

Typical messages:
- `Array index must be an integer, but got '...'`

---

### `S2009` — Heterogeneous array literal
Emitted when array literal elements are not type-compatible.

Typical messages:
- `Array elements must have the same type. Expected '...', but got '...'`

---

### `S2010` — Invalid dereference
Emitted when dereferencing non-pointer value.

Typical messages:
- `Cannot dereference non-pointer type '...'`

---

### `S2011` — Invalid weak-reference usage
Emitted when weak references are used in disallowed ways.

Typical messages:
- `Weak references cannot ...`

---

### `S2012` — No matching overload
Emitted when overload resolution fails for a call.

Typical messages:
- `No matching overload for function '...'`

---

### `S2013` — Ambiguous overload call
Emitted when multiple overload candidates are equally valid.

Typical messages:
- `Call to function '...' is ambiguous between ...`

---

### `S2014` — Invalid immutable binding usage
Emitted when an immutable `let` binding has no initializer or an operation
would mutate its value storage.

Typical messages:
- `Immutable binding '...' must be initialized.`
- `Cannot mutate immutable binding '...'.`

---

### `S2015` — Invalid constant initializer
Emitted when a `const` initializer needs runtime evaluation.

Typical messages:
- `Constant '...' must be initialized with a compile-time expression: ...`

---

## Warnings (`W1xxx`)

### `W1001` — Non-void function may not return on all paths
Emitted when function has non-void return type and control flow analysis finds missing return path.

Typical messages:
- `Function '...' has non-void return type but no return on some paths.`

---

## Notes and Help (`N1xxx`)

### `N1000` — Help note
Emitted as contextual hint attached to an error.

Typical messages:
- `help: add ';' at the end of the statement.`
- `help: insert an expression ...`
- `help: use a Bool expression ...`

---

### `N1001` — Error-cap note
Emitted once when compiler stops reporting after reaching max error threshold.

Typical messages:
- `Too many errors emitted; stopping after ...`

---

## Fallback Codes

### `E0000` — Uncategorized error
Used when no specific code mapping matches an error message.

### `W0000` — Uncategorized warning
Used when no specific warning mapping exists.

### `N0000` — Uncategorized note
Used when no specific note mapping exists.

---

## Best Practices

- Prefer matching and asserting **codes** in tests, not full free-form messages.
- Keep messages human-friendly; treat code as the stable contract.
- When adding a new diagnostic category:
  1. Add code mapping in compiler diagnostics logic
  2. Add/extend tests to assert the new code
  3. Update this document

---

## Maintenance Checklist (for contributors)

When introducing or changing diagnostics:

- [ ] Is the code family correct (`P`, `S`, `W`, `N`)?
- [ ] Is a specific code used instead of fallback?
- [ ] Is there a regression test asserting the code?
- [ ] Does LSP receive the same code?
- [ ] Is this document updated?

---
