#!/bin/bash
set -e

VERSION="0.2.1"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

CPU_COUNT=$(nproc 2>/dev/null || echo 1)
BUILD_JOBS=$(( CPU_COUNT > 1 ? CPU_COUNT - 1 : 1 ))

echo "Configuring and building Zap compiler & LSP..."
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS="-O2" \
    -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" \
    -DLLVM_LINK_LLVM_DYLIB=OFF

cmake --build . --config Release --parallel "$BUILD_JOBS"

# Compile stdlib.o
echo "Compiling stdlib.o..."
cc -c "$SCRIPT_DIR/src/stdlib.c" -o "$BUILD_DIR/stdlib.o"

# Package VS Code extension
echo "Packaging VS Code extension..."
cd "$SCRIPT_DIR/src/lsp/vscode/zap"
npm install
npm run package
cd "$SCRIPT_DIR"

# Stage release layout
ARCH=$(uname -m)
STAGE_DIR="$SCRIPT_DIR/zap-${VERSION}-linux-${ARCH}"

rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR"

echo "Staging files to $STAGE_DIR..."
install -m 755 "$BUILD_DIR/zapc"     "$STAGE_DIR/zapc"
install -m 755 "$BUILD_DIR/zap-lsp"  "$STAGE_DIR/zap-lsp"
install -m 644 "$BUILD_DIR/stdlib.o" "$STAGE_DIR/stdlib.o"
cp -R "$SCRIPT_DIR/std"              "$STAGE_DIR/std"
cp -R "$SCRIPT_DIR/src/lsp"          "$STAGE_DIR/lsp"

# Write install.sh to stage directory
echo "Writing install.sh..."
cat << 'EOF' > "$STAGE_DIR/install.sh"
#!/usr/bin/env bash

set -euo pipefail

if [ -t 1 ]; then
  C_RESET=$'\033[0m'
  C_BOLD=$'\033[1m'
  C_DIM=$'\033[2m'
  C_RED=$'\033[31m'
  C_GREEN=$'\033[32m'
  C_YELLOW=$'\033[33m'
  C_BLUE=$'\033[34m'
  C_CYAN=$'\033[36m'
else
  C_RESET=''
  C_BOLD=''
  C_DIM=''
  C_RED=''
  C_GREEN=''
  C_YELLOW=''
  C_BLUE=''
  C_CYAN=''
fi

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PACKAGE_VERSION="0.2.1"
PACKAGE_NAME="zap"

banner() {
  printf '\n%s%sZap Installer %s%s\n' "$C_BOLD" "$C_CYAN" "$PACKAGE_VERSION" "$C_RESET"
  printf '%sInstall zap runtime, compiler, stdlib, and optional editor support.%s\n\n' "$C_DIM" "$C_RESET"
}

info() {
  printf '%s[info]%s %s\n' "$C_BLUE" "$C_RESET" "$1"
}

success() {
  printf '%s[ok]%s %s\n' "$C_GREEN" "$C_RESET" "$1"
}

warn() {
  printf '%s[warn]%s %s\n' "$C_YELLOW" "$C_RESET" "$1"
}

error() {
  printf '%s[error]%s %s\n' "$C_RED" "$C_RESET" "$1" >&2
}

require_path() {
  if [ ! -e "$1" ]; then
    error "Missing required file or directory: $1"
    exit 1
  fi
}

prompt() {
  local text=$1
  local default=${2-}
  local answer
  if [ -n "$default" ]; then
    printf '%s%s%s [%s]: ' "$C_BOLD" "$text" "$C_RESET" "$default" >&2
  else
    printf '%s%s%s: ' "$C_BOLD" "$text" "$C_RESET" >&2
  fi
  IFS= read -r answer
  if [ -z "$answer" ]; then
    answer=$default
  fi
  printf '%s' "$answer"
}

confirm() {
  local text=$1
  local default=${2:-Y}
  local suffix='[Y/n]'
  if [ "$default" = "N" ]; then
    suffix='[y/N]'
  fi
  local answer
  while true; do
    printf '%s%s%s %s: ' "$C_BOLD" "$text" "$C_RESET" "$suffix"
    IFS= read -r answer
    if [ -z "$answer" ]; then
      answer=$default
    fi
    case "$answer" in
      y|Y|yes|YES) return 0 ;;
      n|N|no|NO) return 1 ;;
    esac
  done
}

choose() {
  local title=$1
  shift
  local options=("$@")
  local i=1
  printf '%s%s%s\n' "$C_BOLD" "$title" "$C_RESET" >&2
  for option in "${options[@]}"; do
    printf '  %d. %s\n' "$i" "$option" >&2
    i=$((i + 1))
  done
  local answer
  while true; do
    printf 'Select an option [1-%d]: ' "${#options[@]}" >&2
    IFS= read -r answer
    case "$answer" in
      ''|*[!0-9]*) ;;
      *)
        if [ "$answer" -ge 1 ] && [ "$answer" -le "${#options[@]}" ]; then
          printf '%s' "$answer"
          return 0
        fi
        ;;
    esac
  done
}

ensure_line() {
  local line=$1
  local file=$2
  mkdir -p "$(dirname "$file")"
  touch "$file"
  if ! grep -Fqx "$line" "$file"; then
    printf '\n%s\n' "$line" >> "$file"
  fi
}

detect_rc_file() {
  case "$(basename "${SHELL:-}")" in
    zsh)   printf '%s/.zshrc' "$HOME" ;;
    bash)  printf '%s/.bashrc' "$HOME" ;;
    fish)  printf '%s/.config/fish/config.fish' "$HOME" ;;
    ksh)   printf '%s/.kshrc' "$HOME" ;;
    mksh)  printf '%s/.mkshrc' "$HOME" ;;
    yash)  printf '%s/.yashrc' "$HOME" ;;
    tcsh)  printf '%s/.tcshrc' "$HOME" ;;
    csh)   printf '%s/.cshrc' "$HOME" ;;
    ash)
      if [ -f "$HOME/.profile" ]; then printf '%s/.profile' "$HOME"
      else printf '%s/.ashrc' "$HOME"; fi ;;
    dash)
      if [ -f "$HOME/.profile" ]; then printf '%s/.profile' "$HOME"
      else printf '%s/.dashrc' "$HOME"; fi ;;
    *)
      if [ -f "$HOME/.profile" ]; then printf '%s/.profile' "$HOME"
      else printf '%s/.bashrc' "$HOME"; fi ;;
  esac
}

shell_path_line() {
  local shell_name
  shell_name=$(basename "${SHELL:-}")
  case "$shell_name" in
    fish)      printf 'fish_add_path "%s"' "$1" ;;
    csh|tcsh)  printf 'set path = ("%s" $path)' "$1" ;;
    *)         printf 'export PATH="%s:$PATH"' "$1" ;;
  esac
}

find_editor_cli() {
  local candidate
  for candidate in code code-insiders codium; do
    if command -v "$candidate" >/dev/null 2>&1; then
      printf '%s' "$candidate"
      return 0
    fi
  done
  return 1
}

maybe_confirm_reinstall() {
  local install_root=$1
  local version_file="$install_root/VERSION"

  if [ ! -f "$version_file" ]; then
    return 0
  fi

  local installed_version
  installed_version=$(tr -d '\r\n' < "$version_file")
  if [ "$installed_version" != "$PACKAGE_VERSION" ]; then
    return 0
  fi

  printf '\n'
  warn "Detected an existing $PACKAGE_NAME $PACKAGE_VERSION install in $install_root"
  if ! confirm "Reinstall $PACKAGE_NAME $PACKAGE_VERSION into this directory?" Y; then
    warn "Skipped reinstall."
    exit 0
  fi
}

install_core() {
  local install_root=$1
  local bin_dir=$2

  mkdir -p "$install_root" "$bin_dir"

  install -m 755 "$SCRIPT_DIR/zapc"    "$install_root/zapc"
  install -m 755 "$SCRIPT_DIR/zap-lsp" "$install_root/zap-lsp"
  install -m 644 "$SCRIPT_DIR/stdlib.o" "$install_root/stdlib.o"

  rm -rf "$install_root/std" "$install_root/lsp"
  cp -R "$SCRIPT_DIR/std" "$install_root/std"
  cp -R "$SCRIPT_DIR/lsp" "$install_root/lsp"
  printf '%s\n' "$PACKAGE_VERSION" > "$install_root/VERSION"

  ln -sfn "$install_root/zapc"    "$bin_dir/zapc"
  ln -sfn "$install_root/zap-lsp" "$bin_dir/zap-lsp"
}

install_lazy_spec() {
  local install_root=$1
  local target_root="${XDG_CONFIG_HOME:-$HOME/.config}/nvim/lua/plugins"
  local target_file="$target_root/zap.lua"

  mkdir -p "$target_root"
  cat > "$target_file" <<LUA_EOF
return {
  {
    dir = "$install_root/lsp/nvim",
    name = "zap.nvim",
    lazy = false,
  },
}
LUA_EOF
  success "Created lazy.nvim spec at $target_file"
}

install_init_lua() {
  local install_root=$1
  local init_lua="${XDG_CONFIG_HOME:-$HOME/.config}/nvim/init.lua"
  local line="vim.opt.runtimepath:append(\"$install_root/lsp/nvim\")"

  ensure_line "$line" "$init_lua"
  success "Updated $init_lua"
}

install_vscode_extension() {
  local install_root=$1
  local cli=$2
  local vsix="$install_root/lsp/vscode/zap/zap-0.0.1.vsix"

  "$cli" --install-extension "$vsix" --force >/dev/null
  success "Installed VS Code extension with $cli"
}

banner

require_path "$SCRIPT_DIR/zapc"
require_path "$SCRIPT_DIR/zap-lsp"
require_path "$SCRIPT_DIR/stdlib.o"
require_path "$SCRIPT_DIR/std"
require_path "$SCRIPT_DIR/lsp/nvim"
require_path "$SCRIPT_DIR/lsp/vscode/zap/zap-0.0.1.vsix"

printf '%sPackage root:%s %s\n\n' "$C_DIM" "$C_RESET" "$SCRIPT_DIR"

choice=$(choose "Install target" \
  "$HOME/.local/share/$PACKAGE_NAME/$PACKAGE_VERSION with symlinks in $HOME/.local/bin (recommended)" \
  "Custom install directories")
printf '\n'

INSTALL_ROOT="$HOME/.local/share/$PACKAGE_NAME/$PACKAGE_VERSION"
BIN_DIR="$HOME/.local/bin"

if [ "$choice" = "2" ]; then
  INSTALL_ROOT=$(prompt "Zap runtime directory" "$HOME/.local/share/$PACKAGE_NAME/$PACKAGE_VERSION")
  printf '\n'
  BIN_DIR=$(prompt "Directory for zapc and zap-lsp symlinks" "$HOME/.local/bin")
  printf '\n'
fi

info "Installing Zap runtime into $INSTALL_ROOT"
info "Creating command symlinks in $BIN_DIR"
maybe_confirm_reinstall "$INSTALL_ROOT"
install_core "$INSTALL_ROOT" "$BIN_DIR"
success "Installed zapc, zap-lsp, stdlib.o, std/, and bundled LSP assets"

if printf '%s' ":$PATH:" | grep -Fq ":$BIN_DIR:"; then
  success "$BIN_DIR is already in PATH"
else
  if confirm "Add $BIN_DIR to PATH in your shell config?" Y; then
    RC_FILE=$(detect_rc_file)
    ensure_line "$(shell_path_line "$BIN_DIR")" "$RC_FILE"
    success "Updated $RC_FILE"
  else
    warn "Skipped PATH update. You may need to add $BIN_DIR manually."
  fi
fi

printf '\n'
if confirm "Install Neovim support?" Y; then
  NVIM_CHOICE=$(choose "Neovim setup" \
    "lazy.nvim spec" \
    "Append runtimepath to init.lua" \
    "Skip editor config")
  printf '\n'
  case "$NVIM_CHOICE" in
    1) install_lazy_spec "$INSTALL_ROOT" ;;
    2) install_init_lua "$INSTALL_ROOT" ;;
    3) warn "Skipped Neovim config" ;;
  esac
fi

printf '\n'
if confirm "Install VS Code extension from bundled .vsix?" Y; then
  if EDITOR_CLI=$(find_editor_cli); then
    install_vscode_extension "$INSTALL_ROOT" "$EDITOR_CLI"
  else
    warn "No VS Code CLI found."
    printf 'Run manually:\n  code --install-extension "%s/lsp/vscode/zap/zap-0.0.1.vsix" --force\n' "$INSTALL_ROOT"
  fi
fi

printf '\n%s%sDone.%s\n' "$C_BOLD" "$C_GREEN" "$C_RESET"
printf 'zapc:    %s\n' "$BIN_DIR/zapc"
printf 'zap-lsp: %s\n' "$BIN_DIR/zap-lsp"
printf 'runtime: %s\n' "$INSTALL_ROOT"
printf '\nOpen a new shell or reload your shell config if PATH changed.\n'
EOF

chmod +x "$STAGE_DIR/install.sh"

# Archive staged directory to .tar.gz
TAR_FILE="$SCRIPT_DIR/zap-${VERSION}-linux-${ARCH}.tar.gz"
echo "Creating archive $TAR_FILE..."
tar -czf "$TAR_FILE" -C "$SCRIPT_DIR" "$(basename "$STAGE_DIR")"

echo "Release build successful! Archive created at: $TAR_FILE"
