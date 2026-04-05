#!/usr/bin/env sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
INIT_LUA="${XDG_CONFIG_HOME:-$HOME/.config}/nvim/init.lua"
LINE="vim.opt.runtimepath:append(\"$SCRIPT_DIR\")"

mkdir -p "$(dirname "$INIT_LUA")"
touch "$INIT_LUA"

if ! grep -Fqx "$LINE" "$INIT_LUA"; then
  printf '\n%s\n' "$LINE" >> "$INIT_LUA"
fi

cat <<EOF
Updated init.lua:
  $INIT_LUA

Added:
  $LINE

Next steps:
  1. Restart Neovim
  2. Open a .zp file
EOF
