# Zap Roadmap

This document tracks the planned development of the Zap programming language.

---

## Phase 0 - Foundations (current)

- [x] function declaration
- [x] function arguments
- [x] return statement
- [x] compiling expressions
- [ ] variable declaration
- [ ] variable reassigment
- [ ] strings in expressions
- [ ] if stmt/expression
- [ ] loops (loop, while, for)
- [ ] pointers & references
- [ ] ext stmt (external functions)
- [ ] arrays
- [ ] enum
- [ ] struct

## Phase 1 - Core

- [ ] Garbage collector

- [ ] modules system + name mangling
- [ ] generics
- [ ] methods for struct
- [ ] static methods
- [ ] basic stdlib
- [ ] pattern matching + each
- [ ] Error handling (Result<T, E> )

## Phase 2 - Concurrency & Runtime

- [ ] Lightweight threads (`zap`)
- [ ] Typed channels (`chan<T>`)
- [ ] `select`
- [ ] Scheduler improvements
- [ ] GC tuning
- [ ] `#no_gc` blocks

## Phase 3 - Stabilization (1.0)

- [ ] Syntax freeze
- [ ] Stable standard library
- [ ] Diagnostics and error messages
- [ ] Performance tuning
- [ ] Documentation
- [ ] Examples and guides

## Non-Goals

Zap explicitly does **not** aim to:

- Replace Rust in memory-critical systems
- Introduce a new concurrency model
- Become macro-heavy or metaprogramming-focused
- Add unnecessary complexity

---

## Contributing

The roadmap is intentionally flexible.
Early contributors can directly influence language design.

Open issues, propose changes, or discuss ideas in the repository.
