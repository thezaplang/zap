# zap.nvim

Neovim support for Zap:

- `*.zp` filetype detection
- syntax highlighting
- diagnostics
- completion through LSP
- `gd` go to definition
- up-to-date keyword highlighting including `iftype`, `where`, and `weak`

This runtime works on Neovim `0.11+`.

## Check your version

```bash
nvim --version | head -n 1
```

You want:

```bash
NVIM v0.11.x
```

## Installers

For `lazy.nvim` / NvChad:

```bash
./src/lsp/nvim/install-lazy.sh
```

For plain `init.lua`:

```bash
./src/lsp/nvim/install-init.lua.sh
```

## Option 1: lazy.nvim / NvChad

Add a plugin spec that points at this directory:

```lua
{
  dir = "/path/to/zap/src/lsp/nvim",
  name = "zap.nvim",
  lazy = false,
}
```

If you want, you can also `dofile()` the ready-made spec from this repo:

```lua
dofile("/path/to/zap/src/lsp/nvim/lazy.lua")
```

After that:

1. run `:Lazy sync`
2. restart Neovim
3. open a `.zp` file

## Option 2: plain init.lua

Append the runtime path manually:

```lua
vim.opt.runtimepath:append("/path/to/zap/src/lsp/nvim")
```

Restart Neovim and open a `.zp` file.

## What gets configured automatically

When the runtime is loaded it:

- sets `*.zp` to `filetype=zap`
- enables syntax highlighting
- enables built-in LSP for Zap
- sets `omnifunc` to `vim.lsp.omnifunc`
- binds `gd` to definition

Server lookup order:

1. `zap-lsp` from `PATH`
2. repo-local `build/zap-lsp`

Stdlib lookup order:

1. `<project-root>/std`
2. repo-local `std`

## Quick checks

Inside Neovim:

```vim
:set filetype?
:echo exists(':ZapNvimInfo')
:ZapNvimInfo
```

Expected:

- `filetype=zap`
- `exists(':ZapNvimInfo')` returns `2`

You can also quickly verify modern keyword highlighting by opening a `.zp` file containing:

```zap
iftype T == Int { }
class Box<T> where T: Animal { }
var node: weak Node;
```

## Notes

- No `nvim-lspconfig` is required on Neovim `0.11+`.
- `nvim-cmp` is optional. If installed, the runtime will expose LSP capabilities to it.
