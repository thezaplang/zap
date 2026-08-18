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
