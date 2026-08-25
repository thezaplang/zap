#!/usr/bin/env bash

# Build script for Zap compiler
# Creates build directory and compiles the project using Meson

set -e # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_TYPE="release"
BUILD_DIR="$SCRIPT_DIR/build"
MESON_ARGS=()

for arg in "$@"; do
  case "$arg" in
    --debug)
      BUILD_TYPE="debug"
      BUILD_DIR="$SCRIPT_DIR/build-debug"
      ;;
    --help|-h)
      echo "Usage: ./build.sh [--debug] [meson-compile-args...]"
      echo ""
      echo "Builds the Zap compiler using Meson."
      echo "--debug uses a separate build-debug directory with debug symbols."
      echo "Other arguments are passed directly to 'meson compile'."
      echo "Examples:"
      echo "  ./build.sh                (Release build in build/)"
      echo "  ./build.sh --debug        (Debug build in build-debug/)"
      echo "  ./build.sh --debug zapc   (Build only the zapc target)"
      exit 0
      ;;
    *) MESON_ARGS+=("$arg") ;;
  esac
done

echo -e "${YELLOW}Building Zap compiler...${NC}"

# Configure the build directory if it hasn't been set up yet.
# Checking for build.ninja is safer than checking the directory,
# which prevents failures if an empty 'build' folder was created manually.
if [ ! -f "$BUILD_DIR/build.ninja" ]; then
  echo -e "${YELLOW}Setting up build directory...${NC}"
  # shellcheck disable=SC2086 # word splitting is intentional
  meson setup "$BUILD_DIR" "$SCRIPT_DIR" "--buildtype=$BUILD_TYPE" $MESON_SETUP_FLAGS
elif ! python3 - "$BUILD_DIR/meson-info/meson-info.json" "$BUILD_DIR/meson-info/intro-buildoptions.json" "$SCRIPT_DIR" "$BUILD_TYPE" <<'PY'
import json
import os
import sys

try:
    with open(sys.argv[1], encoding="utf-8") as info_file:
        info = json.load(info_file)
    with open(sys.argv[2], encoding="utf-8") as options_file:
        options = json.load(options_file)
    configured_source = info["directories"]["source"]
    configured_buildtype = next(option["value"] for option in options if option["name"] == "buildtype")
    valid = not info.get("error", False) and (
        os.path.realpath(configured_source) == os.path.realpath(sys.argv[3])
    ) and configured_buildtype == sys.argv[4]
except (OSError, KeyError, StopIteration, TypeError, json.JSONDecodeError):
    valid = False

raise SystemExit(0 if valid else 1)
PY
then
  echo -e "${YELLOW}Build directory is stale; reconfiguring it...${NC}"
  # shellcheck disable=SC2086 # word splitting is intentional
  meson setup "$BUILD_DIR" "$SCRIPT_DIR" --wipe "--buildtype=$BUILD_TYPE" $MESON_SETUP_FLAGS
fi

# Build the project
echo -e "${YELLOW}Compiling...${NC}"
# shellcheck disable=SC2086 # word splitting is intentional
meson compile -C "$BUILD_DIR" "${MESON_ARGS[@]}" $MESON_BUILD_FLAGS

# Keep the language server next to zapc.  zapup adds this directory to PATH,
# while Meson otherwise writes the target under src/lsp/.
if [ -f "$BUILD_DIR/src/lsp/zap-lsp" ]; then
  cp "$BUILD_DIR/src/lsp/zap-lsp" "$BUILD_DIR/zap-lsp"
fi

# Check if build was successful
if [ -f "$BUILD_DIR/zapc" ]; then
  echo -e "${GREEN}Build successful!${NC}"
  echo -e "${GREEN}Executable: $BUILD_DIR/zapc${NC}"
else
  echo -e "${RED}Build failed! (zapc executable not found)${NC}"
  exit 1
fi

# vim: set tabstop=2 shiftwidth=2 expandtab:
