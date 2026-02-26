#!/bin/bash

# Build the compiler first
./build.sh > /dev/null 2>&1
if [ $? -ne 0 ]; then
    echo "Failed to build the compiler"
    exit 1
fi

ZAPC="./build/zapc"
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m' # No Color

TOTAL=0
PASSED=0

run_test() {
    local file=$1
    local expected_exit_code=$2
    local description=$3

    ((TOTAL++))
    echo -n "Running $description ($file)... "
    
    $ZAPC "$file" > /dev/null 2>&1
    local exit_code=$?
    
    if [ $exit_code -eq $expected_exit_code ]; then
        echo -e "${GREEN}PASS${NC}"
        ((PASSED++))
    else
        echo -e "${RED}FAIL${NC} (expected $expected_exit_code, got $exit_code)"
    fi
}

echo "--- Zap Compiler Test Suite ---"

# Valid code (should pass)
run_test "tests/valid.zap" 0 "Valid program"
run_test "tests/return_void.zap" 0 "Return in void function"

# Warning test: compile and check stderr for the warning message
run_warning_test() {
    local file=$1
    local description=$2

    ((TOTAL++))
    echo -n "Running $description ($file)... "

    tmpfile=$(mktemp)
    $ZAPC "$file" 2> "$tmpfile" > /dev/null
    local exit_code=$?

    if [ $exit_code -ne 0 ]; then
        echo -e "${RED}FAIL${NC} (compiler error)"
        return
    fi

    if grep -qi "has non-void return type but no return" "$tmpfile"; then
        echo -e "${GREEN}PASS${NC}"
        ((PASSED++))
    else
        echo -e "${RED}FAIL${NC} (warning not emitted)"
    fi
    rm -f "$tmpfile"
}

# Runtime test: compile, run produced binary, check its exit code
run_runtime_test() {
    local file=$1
    local expected_exit_code=$2
    local description=$3

    ((TOTAL++))
    echo -n "Running $description ($file)... "

    $ZAPC "$file" > /dev/null 2>&1
    local exit_code=$?
    if [ $exit_code -ne 0 ]; then
        echo -e "${RED}FAIL${NC} (compile failed)"
        return
    fi

    binfile="${file%.*}"
    if [ ! -x "$binfile" ]; then
        echo -e "${RED}FAIL${NC} (binary not found)"
        return
    fi

    ./$binfile > /dev/null 2>&1
    local run_code=$?
    rm -f "$binfile"

    if [ $run_code -eq $expected_exit_code ]; then
        echo -e "${GREEN}PASS${NC}"
        ((PASSED++))
    else
        echo -e "${RED}FAIL${NC} (expected $expected_exit_code, got $run_code)"
    fi
}

# Warning test: non-void function without return should emit warning
run_warning_test "tests/warn_missing_return.zap" "Warning: missing return in non-void function"

# Runtime test: main without explicit return type should default to Int and return 0
run_runtime_test "tests/main_implicit.zap" 0 "Main implicit return type and implicit return 0"

# Lexer errors (exit code 1)
run_test "tests/lexer_error.zap" 1 "Lexer error: Unterminated string"

# Syntax errors (exit code 1)
run_test "tests/syntax_error.zap" 1 "Syntax error: Missing semicolons"

# Semantic errors (exit code 1)
run_test "tests/sema_error.zap" 1 "Semantic error: Undefined variable"

# Multiple errors (exit code 1)
run_test "tests/syntax_error.zap" 1 "Multiple syntax errors"

# Concat tests
run_runtime_test "tests/concat.zap" 0 "Concat literal strings"
run_runtime_test "tests/concat_char.zap" 0 "Concat char + string"

# Logical operator tests
run_runtime_test "tests/logical_ops.zap" 0 "Logical operators (&&, ||) with short-circuiting"
run_test "tests/logical_type_error.zap" 1 "Logical operators type check"

echo "-------------------------------"
echo "Results: $PASSED / $TOTAL passed"

if [ $PASSED -eq $TOTAL ]; then
    echo -e "${GREEN}ALL TESTS PASSED!${NC}"
    exit 0
else
    echo -e "${RED}SOME TESTS FAILED!${NC}"
    exit 1
fi
