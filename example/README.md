# Zap examples

These examples are short, production-shaped programs intended for the Zap
website and for people evaluating the language. Every example uses the
automatic prelude: common APIs such as `println`, `List<>`, `HashMap<>`, and
`toString` are already in scope.

## Start here

| Example | What it shows |
| --- | --- |
| `01_hello_zap.zp` | A small Zap program with no setup noise |
| `02_safe_domain_modeling.zp` | Typed domain data and explicit state |
| `03_straightforward_errors.zp` | Clear, local failure handling and `?` propagation |
| `04_collections.zp` | Prelude-provided `List<>` and `HashMap<>` |
| `05_generics.zp` | Reusable code with inferred type arguments |
| `06_explicit_mutation.zp` | Mutation that is visible at the call site |
| `07_predictable_lifetimes.zp` | Deterministic ARC cleanup |
| `08_weak_references.zp` | Non-owning back references without dangling access |
| `09_predictable_ownership.zp` | Ownership-taking APIs with stable source semantics |
| `10_incremental_c_ffi.zp` | Sharing C-layout records and callbacks with an existing C API |
| `11_isolated_unsafe.zp` | Keeping raw-pointer work behind a narrow boundary |
| `12_polymorphism.zp` | Classes, inheritance, and dynamic dispatch |
| `13_managed_cycles.zp` | Reclaiming an unreachable strong-reference cycle |
| `14_memory_safety.zp` | Preventing dangling views and use-after-free |
| `15_json_document.zp` | Building, parsing, and safely reading nested JSON |
| `16_terminal.zp` | ANSI colors, text styles, cursor controls, and screen clearing |
| `17_interfaces.zp` | Shared contracts across unrelated classes, dispatched dynamically |

## Memory safety boundary

Ordinary Zap code is memory-safe: managed references use automatic ownership,
and a borrowed `StringView` cannot outlive the `String` allocation behind it.
`14_memory_safety.zp` demonstrates two cases that commonly become
use-after-free bugs with raw pointers: returning a view into a temporary value
and reassigning the variable that originally owned a borrowed allocation. Zap
tracks the owner and keeps it alive through the view's last use.

Raw-pointer operations require an explicit `unsafe` boundary.
`11_isolated_unsafe.zp` shows how to keep code that deliberately crosses that
boundary small and reviewable.

## Build and run

From the repository root:

```bash
./build/zapc example/03_straightforward_errors.zp -o /tmp/zap-example
/tmp/zap-example
```

The module example is compiled from its entrypoint:

```bash
./build/zapc example/modules/main.zp -o /tmp/zap-modules
/tmp/zap-modules
```

Only examples that cross a real platform boundary use explicit imports or
`unsafe`. Ordinary application code relies on the prelude.
