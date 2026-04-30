#!/bin/bash
set -e

VERSION="0.2.0"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

CPU_COUNT=$(nproc 2>/dev/null || echo 1)
BUILD_JOBS=$(( CPU_COUNT > 1 ? CPU_COUNT - 1 : 1 ))

cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS="-O2" \
    -DLLVM_LINK_LLVM_DYLIB=OFF

cmake --build . --config Release --parallel "$BUILD_JOBS"

# Compile stdlib.o
cc -c "$SCRIPT_DIR/src/stdlib.c" -o "$BUILD_DIR/stdlib.o"

# Stage release layout (without packaging)
ARCH=$(uname -m)
STAGE_DIR="$SCRIPT_DIR/zap-${VERSION}-linux-${ARCH}"

rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR"

install -m 755 "$BUILD_DIR/zapc"     "$STAGE_DIR/zapc"
install -m 755 "$BUILD_DIR/zap-lsp"  "$STAGE_DIR/zap-lsp"
install -m 644 "$BUILD_DIR/stdlib.o" "$STAGE_DIR/stdlib.o"
cp -R "$SCRIPT_DIR/std"              "$STAGE_DIR/std"
cp -R "$SCRIPT_DIR/src/lsp"          "$STAGE_DIR/lsp"

echo "Done. Staged to: $STAGE_DIR"
