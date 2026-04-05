#!/usr/bin/env sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TARGET_ROOT="${XDG_CONFIG_HOME:-$HOME/.config}/nvim/lua/plugins"
TARGET_FILE="$TARGET_ROOT/zap.lua"

mkdir -p "$TARGET_ROOT"

cat > "$TARGET_FILE" <<EOF
return {
  {
    dir = "$SCRIPT_DIR",
    name = "zap.nvim",
    lazy = false,
  },
}
EOF

cat <<EOF
Created lazy.nvim spec:
  $TARGET_FILE

Next steps:
  1. Restart Neovim or run :Lazy sync
  2. Open a .zp file
EOF
