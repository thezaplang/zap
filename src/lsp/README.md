# Zap LSP

The repository now contains:

- `build/zap-lsp`: the language server binary
- `src/lsp/vscode/zap`: the VS Code extension source
- `src/lsp/nvim`: a small Neovim runtime with syntax + LSP config

## Neovim

Add `src/lsp/nvim` to `runtimepath`. That gives you:

- filetype detection for `*.zp`
- syntax highlighting
- `omnifunc` wired to LSP
- a reusable `lspconfig` config module

Full Neovim instructions:

- [src/lsp/nvim/README.md](/home/funcieq/zap/src/lsp/nvim/README.md)

Quick install from this repo:

```bash
./src/lsp/nvim/install-lazy.sh
```

For plain `init.lua` setups:

```bash
./src/lsp/nvim/install-init.lua.sh
```

On Neovim 0.11+, opening a `.zp` file is enough.
No extra plugin is required for diagnostics, completion, go to definition, and syntax highlighting.

Version check:

```bash
nvim --version | head -n 1
```

You want `NVIM v0.11.x` or newer for the built-in setup path.

Example with `lazy.nvim`:

```lua
{
  dir = "/path/to/zap/src/lsp/nvim",
  name = "zap.nvim",
  config = function()
    vim.opt.runtimepath:append("/path/to/zap/src/lsp/nvim")
  end,
}
```

Manual example without a plugin manager:

```lua
vim.opt.runtimepath:append("/path/to/zap/src/lsp/nvim")
```

The runtime auto-configures:

- `*.zp` -> `filetype=zap`
- syntax highlighting
- built-in LSP on Neovim 0.11+
- `gd` for definition
- diagnostics
- completion via LSP/omnifunc

Server lookup order:

1. `zap-lsp` from `PATH`
2. repo-local `build/zap-lsp`

Stdlib lookup order:

1. `<project-root>/std`
2. repo-local `std`

Quick sanity checks inside Neovim:

```vim
:set filetype?
:echo exists(':ZapNvimInfo')
:ZapNvimInfo
```

Expected:

- `filetype=zap`
- `exists(':ZapNvimInfo')` returns `1`

## VS Code

From `src/lsp/vscode/zap`:

```bash
npm install
npm run package
```

That creates a `.vsix` you can install with `Install from VSIX...`.
