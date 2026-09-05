# Zap LSP 0.1.0

`zap-lsp` provides diagnostics, completion, definition, hover, signature help,
UTF-16 positions, document synchronization, and request cancellation over
standard LSP stdio transport.

## Build the server

From the repository root:

```bash
meson setup build -Dinclude_lsp=true
meson compile -C build zap-lsp
```

The binary is written to `build/zap-lsp`.

## Configure a workspace

Project imports come from the nearest `thor.toml`. The `[imports]` table uses
the same aliases as `thor build`; relative paths are resolved from the
directory containing the manifest:

```toml
[imports]
"@vendor" = "./vendor/package"
```

The server obtains `core` and `std` from its Zap installation. A release
installation keeps them next to `zap-lsp`; a source installation made by
`zapup --src` keeps them one directory above `build/zap-lsp`.

## Build and install the VS Code extension

Install Zap through `zapup` first so `zap-lsp` is on `PATH`, then run:

```bash
cd src/lsp/vscode/zap
npm install
npm run package
```

Install the generated `.vsix` with **Extensions → … → Install from VSIX…**.
The extension does not bundle or configure a language server; it launches the
`zap-lsp` installed by `zapup`.

## Shared compiler frontend

The compiler and server link the same static frontend libraries through the
Meson dependency `libzap_dep`. It includes syntax, semantic analysis, type
support, module loading, project configuration, and diagnostic stream support.
Consumers do not compile binder or other frontend implementation files themselves.
`zap_sema` owns the binder and constant evaluator; `zap_frontend` depends on it.

The entry point is `zap::frontend::FrontendSession` in
`frontend/frontend_session.hpp`. Its source loader accepts in-memory documents,
and `FrontendProject` retains the modules, bound root, diagnostics, and semantic
information needed by tooling. The `frontend-session` test links only
`libzap_dep` and exercises loading and binding through this interface.

`libzap_dep` is currently an internal Meson dependency, not an installed SDK or
single archive with a stable ABI. LLVM code generation and runtime objects are
outside it, but configuring this repository still requires LLVM and OpenSSL.
The server remains in this repository during this first extraction step.
