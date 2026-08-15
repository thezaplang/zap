# Zap Language Documentation

Welcome to the official documentation for the **Zap** programming language.

Zap is a modern systems programming language focused on predictable performance, explicit behavior, and developer-friendly syntax.

## Core Philosophy

- **Performance**: Native code generation via LLVM.
- **Predictability**: ARC-based memory management (no stop-the-world GC pauses).
- **Safety**: Strong static typing and clear compile-time diagnostics.
- **Expressiveness**: Practical syntax for control flow, data modeling, and generic code.
- **Tooling**: Compiler + LSP diagnostics designed for fast iteration.

---

## Documentation Map

### Language Basics

1. [Variables and Types](variables.md)  
   Variable/constant declarations, primitive types, arrays, and type basics.

2. [Functions](functions.md)  
   Function declarations, return types, parameters, calls, and recursion.

3. [Control Flow](control_flow.md)  
   `if`, `while`, ternary expressions, and condition typing rules.

---

### Data & Memory

4. [Data Structures](data_structures.md)  
   Records, enums, and arrays.

5. [Classes](classes.md)  
   Heap-only classes, methods, inheritance, visibility, and `new`.

6. [Memory Management](memory.md)  
   ARC model, object lifetime behavior, and performance implications.

7. [Strings](strings.md)  
   `String`, `StringView`, `TextBuf`, ownership/borrowing rules, and `std/string` behavior.

8. [Generics](generics.md)  
   Generic functions, generic structs/classes, constraints (`where`), and compile-time `iftype`.

9. [JSON](json.md)
   Building, parsing, typed access, lossless numbers, serialization, and errors.

---

### Diagnostics & Tooling

10. [Diagnostic Codes](diagnostic_codes.md)
   Full reference for parser/semantic/warning/note diagnostic codes (`Pxxxx`, `Sxxxx`, `Wxxxx`, `Nxxxx`), including examples and maintenance guidelines.

### Architecture RFCs

11. [Ownership-aware ARC RFC](rfc/ownership-aware-arc.md)
    Draft design for ownership-aware ZIR, predictable ARC semantics, borrow
    provenance, and scheduled cycle collection. It describes the target model;
    it is not yet a description of released compiler behavior.

---

## Notes

- Zap is currently in **early alpha**; some features and docs are evolving quickly.
- If diagnostics or language behavior changes, update both:
  - implementation/tests, and
  - the corresponding docs pages.

If you contribute new language features, please also update this index so the section map stays accurate.
