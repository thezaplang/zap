#!/usr/bin/env bash

set -euo pipefail

VERSION="0.4.0"
EXTENSION_VSIX="zap-${VERSION}.vsix"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build-release"
ARCH=$(uname -m)
STAGE_DIR="$SCRIPT_DIR/zap-v${VERSION}-linux-${ARCH}"
TAR_FILE="$SCRIPT_DIR/zap-v${VERSION}-linux-${ARCH}.tar.gz"

echo -e "\033[1;33mConfiguring Zap compiler & LSP for release...\033[0m"

rm -rf "$BUILD_DIR"

LDFLAGS="-static-libstdc++ -static-libgcc" meson setup "$BUILD_DIR" "$SCRIPT_DIR" \
    --buildtype=release \
    -Dinclude_lsp=true

echo -e "\033[1;33mCompiling...\033[0m"
meson compile -C "$BUILD_DIR"

echo -e "\033[1;33mPackaging VS Code extension...\033[0m"
EXTENSION_SOURCE_DIR="$SCRIPT_DIR/src/lsp/vscode/zap"
EXTENSION_BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/zap-vscode.XXXXXX")"
cleanup() {
  rm -rf "$EXTENSION_BUILD_DIR"
}
trap cleanup EXIT INT TERM

tar -C "$EXTENSION_SOURCE_DIR" \
    --exclude='./node_modules' \
    --exclude='./bin' \
    --exclude='./out' \
    --exclude='./package-lock.json' \
    --exclude='./zap-*.vsix' \
    -cf - . | tar -C "$EXTENSION_BUILD_DIR" -xf -

(
  cd "$EXTENSION_BUILD_DIR"
  npm install --no-package-lock

  npm run package
)

test -f "$EXTENSION_BUILD_DIR/$EXTENSION_VSIX"

echo -e "\033[1;33mStaging files to $STAGE_DIR...\033[0m"
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR"

install -m 755 "$BUILD_DIR/zapc" "$STAGE_DIR/zapc"
install -m 755 "$BUILD_DIR/src/lsp/zap-lsp" "$STAGE_DIR/zap-lsp"
install -m 644 "$BUILD_DIR/stdlib.o" "$STAGE_DIR/stdlib.o"
cp -R "$SCRIPT_DIR/core" "$STAGE_DIR/core"
cp -R "$SCRIPT_DIR/std" "$STAGE_DIR/std"

install -m 644 "$EXTENSION_BUILD_DIR/$EXTENSION_VSIX" \
    "$STAGE_DIR/$EXTENSION_VSIX"

echo -e "\033[1;33mCreating archive $TAR_FILE...\033[0m"
tar -czf "$TAR_FILE" -C "$SCRIPT_DIR" "$(basename "$STAGE_DIR")"

echo -e "\033[0;32mRelease build successful! Archive created at: $TAR_FILE\033[0m"
