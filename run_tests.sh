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
run_test "tests/modulo_test.zap" 0 "Modulo operator test"

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

    binfile="${file%.*}"
    $ZAPC "$file" -o "$binfile" > /dev/null 2>&1
    local exit_code=$?
    if [ $exit_code -ne 0 ]; then
        echo -e "${RED}FAIL${NC} (compile failed)"
        return
    fi

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

# Warning + Runtime test: check for warning AND exit code
run_warning_runtime_test() {
    local file=$1
    local expected_exit_code=$2
    local warning_pattern=$3
    local description=$4

    ((TOTAL++))
    echo -n "Running $description ($file)... "

    tmpfile=$(mktemp)
    binfile="${file%.*}"
    $ZAPC "$file" -o "$binfile" 2> "$tmpfile"
    local compile_code=$?

    if [ $compile_code -ne 0 ]; then
        echo -e "${RED}FAIL${NC} (compile failed)"
        rm -f "$tmpfile"
        return
    fi

    if ! grep -qi "$warning_pattern" "$tmpfile"; then
        echo -e "${RED}FAIL${NC} (warning '$warning_pattern' not emitted)"
        rm -f "$tmpfile" "$binfile"
        return
    fi

    ./$binfile > /dev/null 2>&1
    local run_code=$?
    rm -f "$binfile" "$tmpfile"

    if [ $run_code -eq $expected_exit_code ]; then
        echo -e "${GREEN}PASS${NC}"
        ((PASSED++))
    else
        echo -e "${RED}FAIL${NC} (expected exit $expected_exit_code, got $run_code)"
    fi
}

# Warning test: non-void function without return should emit warning
run_warning_test "tests/warn_missing_return.zap" "Warning: missing return in non-void function"

# Global variable test
run_warning_runtime_test "tests/global_var_test.zap" 0 "Global variables are discouraged" "Global variable with warning"

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
run_runtime_test "tests/enum_test.zap" 1 "Enum test"
run_runtime_test "tests/array_test.zap" 0 "Array declaration, initialization, and indexing"
run_runtime_test "tests/array_const_size.zap" 0 "Array size as a constant"

# If expression tests
run_runtime_test "tests/if_expr.zap" 2 "If expression result"
run_runtime_test "tests/if_advanced.zap" 0 "Advanced if expressions (nesting, math, complex cond)"
run_warning_runtime_test "repro.zap" 0 "Global variables are discouraged" "If expression in function call and var decl"

# Struct tests
run_runtime_test "tests/struct_test.zap" 0 "Basic struct member access"
run_runtime_test "tests/struct_nested_test.zap" 0 "Nested struct member access"
run_runtime_test "tests/struct_fn_test.zap" 0 "Structs as function parameters and return values"
run_runtime_test "tests/struct_array_test.zap" 0 "Arrays of structs"
run_runtime_test "tests/struct_types_test.zap" 0 "Structs with diverse field types"
run_runtime_test "tests/precedence_test.zap" 0 "Operator precedence (NOT vs Member access)"

echo "-------------------------------"
echo "Results: $PASSED / $TOTAL passed"

if [ $PASSED -eq $TOTAL ]; then
    echo -e "${GREEN}ALL TESTS PASSED!${NC}"
    exit 0
else
    echo -e "${RED}SOME TESTS FAILED!${NC}"
    exit 1
fi
