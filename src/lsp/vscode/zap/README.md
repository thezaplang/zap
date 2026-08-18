# Zap VS Code Extension

Syntax highlighting and LSP support for Zap.

## Requirements

- Zap installed with `zapup`, with `zap-lsp` available in `PATH`
- VS Code or VSCodium

## Project configuration

The extension launches the `zap-lsp` installed by `zapup`; it does not bundle
the server or keep a separate workspace configuration. Imports are defined in
the nearest `thor.toml` and follow Thor's `[imports]` table:

```toml
[imports]
"@vendor" = "./vendor/package"
```

Relative targets are resolved from that `thor.toml`. The extension watches both
`thor.toml` and Zap source files so diagnostics refresh after configuration or
dependency changes.

## Build the Extension

From this directory:

```bash
npm install
npm run package
```

That produces a `.vsix` file in this directory. It contains only the extension;
install Zap separately with `zapup`.

## Install the Extension

In VS Code:

1. Open Extensions
2. Open the `...` menu
3. Choose `Install from VSIX...`
4. Select the generated `.vsix`

## Color Customization (Function vs Generics)

If your theme makes function names and generic parameters look too similar, add token color overrides in your VS Code settings:

```json
{
  "editor.tokenColorCustomizations": {
    "textMateRules": [
      {
        "scope": "entity.name.function.zap",
        "settings": { "foreground": "#82AAFF", "fontStyle": "bold" }
      },
      {
        "scope": "entity.name.type.parameter.zap",
        "settings": { "foreground": "#FFCB6B", "fontStyle": "italic" }
      },
      {
        "scope": [
          "punctuation.definition.generic.begin.zap",
          "punctuation.definition.generic.end.zap",
          "punctuation.separator.generic.zap"
        ],
        "settings": { "foreground": "#C792EA" }
      }
    ]
  }
}
```

After updating settings, run **Developer: Reload Window**.

## Notes

- The server provides diagnostics, completion, definition, hover, and signature help.
- Requests can be cancelled while another analysis is running; cancelled requests return the standard LSP cancellation error.
- The extension starts the server over stdio, so it also works in VSCodium.
