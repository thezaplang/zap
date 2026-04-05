#!/bin/bash

# Build script for Zap compiler
# Creates build directory and compiles the project using CMake

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo -e "${YELLOW}Building Zap compiler...${NC}"

# Create build directory if it doesn't exist
if [ ! -d "$SCRIPT_DIR/build" ]; then
    echo -e "${YELLOW}Creating build directory...${NC}"
    mkdir -p "$SCRIPT_DIR/build"
fi

# Change to build directory
cd "$SCRIPT_DIR/build"

CPU_COUNT=$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 1)
BUILD_JOBS=$((CPU_COUNT - 1))
if [ "$BUILD_JOBS" -lt 1 ]; then
    BUILD_JOBS=1
fi

# Run CMake to generate build files
echo -e "${YELLOW}Running CMake...${NC}"
cmake ..

# Build the project
echo -e "${YELLOW}Compiling...${NC}"
cmake --build . --config Release --parallel "$BUILD_JOBS"

# Check if build was successful
if [ -f "$SCRIPT_DIR/build/zapc" ]; then
    echo -e "${GREEN}Build successful!${NC}"
    echo -e "${GREEN}Executable: $SCRIPT_DIR/build/zapc${NC}"
else
    echo -e "${RED}Build failed!${NC}"
    exit 1
fi
