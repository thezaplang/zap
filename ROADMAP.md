# Zap Language — Roadmap to 0.1.0

> **Goal:** All core language features implemented and functional.

---

## Phase 1 — Type System Foundation

- [x] Primitive integer types — `Int8`, `Int16`, `Int32`, `Int64`, `UInt8`, `UInt16`, `UInt32`, `UInt64`
- [x] Type inference for integer literals
- [x] Float types — `Float32`, `Float64`
- [x] `Bool`, `Char`, `Byte`
- [x] Type aliases

---

## Phase 2 — Module System

- [ ] `import "path";`
- [ ] Each file is its own namespace
- [ ] Circular import detection
- [ ] `main` function resolution across files

---

## Phase 3 — Access Control

- [ ] `pub` keyword
- [ ] `priv` keyword (or default-private semantics)
- [ ] Visibility enforced at compile time

---

## Phase 4 — References & `ref`

- [ ] `ref` keyword
- [ ] Reference semantics in function signatures: `fun foo(x: ref Int64) Void {}`
- [ ] No dangling references to stack values
- [ ] `ref` vs value semantics enforced by the compiler

---

## Phase 5 — Unsafe Block

- [ ] `unsafe {}` block syntax
- [ ] Raw pointer type `*T` — only valid inside `unsafe`
- [ ] Address-of operator `&` — only valid inside `unsafe`
- [ ] Pointer arithmetic inside `unsafe`

---

## Phase 6 — Classes & ARC

- [ ] `class` declaration — always heap-allocated
- [ ] Class instances always passed by reference (implicit)
- [ ] Constructor (`init`) and destructor (`deinit`)
- [ ] Fields with `pub`/`priv` visibility
- [ ] Methods
- [ ] **ARC**
  - [ ] Retain/release on assignment and scope exit
  - [ ] Strong references (default)
  - [ ] Weak references
  - [ ] Cycle detection

---

## Release Criteria

- [ ] Non-trivial program compiles and runs end-to-end
- [ ] ARC: no leaks or double-frees on a representative test suite
- [ ] Useful compiler errors for all new features
- [ ] Documentation updated to reflect current language state
- [ ] `examples/` covering each phase
