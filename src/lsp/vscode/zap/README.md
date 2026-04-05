# Zap VS Code Extension

Syntax highlighting and LSP support for Zap.

## Requirements

- A built `zap-lsp` binary when packaging the extension
- VS Code or VSCodium

## Default Behavior

The packaged `.vsix` bundles `zap-lsp`.

For imports like `std/io`, set `zap-lsp.stdlibPath` to your local `std/` directory.

## Optional Server Override

If you want to use a different server binary, set `zap-lsp.path`.

Example:

```json
{
  "zap-lsp.path": "/custom/path/to/zap-lsp"
}
```

Example stdlib setting:

```json
{
  "zap-lsp.stdlibPath": "/path/to/zap/std"
}
```

## Build the Extension

From this directory:

```bash
npm install
npm run package
```

That produces a `.vsix` file in this directory.

## Install the Extension

In VS Code:

1. Open Extensions
2. Open the `...` menu
3. Choose `Install from VSIX...`
4. Select the generated `.vsix`

## Notes

- The server currently provides diagnostics.
- The extension starts the server over stdio, so it also works in VSCodium.
