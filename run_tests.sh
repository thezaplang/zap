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
NC='\033[0m'

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
run_test "tests/concat_char.zap" 1 "Concat char + string is currently rejected"

# Logical operator tests
run_runtime_test "tests/logical_ops.zap" 0 "Logical operators (&&, ||) with short-circuiting"
run_test "tests/logical_type_error.zap" 1 "Logical operators type check"
run_runtime_test "tests/if_else_if.zap" 0 "If / else if / else chains"
run_runtime_test "tests/if_nested.zap" 0 "Nested if statements"
run_runtime_test "tests/if_state_merge.zap" 0 "If branches mutating shared state"
run_runtime_test "tests/if_return_paths.zap" 0 "If branches with multiple return paths"
run_test "tests/if_condition_error.zap" 1 "If condition type check"
run_runtime_test "tests/ternary_test.zap" 0 "Ternary operator"
run_test "tests/ternary_condition_error.zap" 1 "Ternary condition type check"
run_test "tests/ternary_type_error.zap" 1 "Ternary branch type check"
run_runtime_test "tests/enum_test.zap" 1 "Enum test"
run_runtime_test "tests/array_test.zap" 0 "Array declaration, initialization, and indexing"
run_test "tests/array_const_size.zap" 1 "Array size as a constant is currently rejected"

# Struct tests
run_runtime_test "tests/struct_test.zap" 0 "Basic struct member access"
run_runtime_test "tests/struct_nested_test.zap" 0 "Nested struct member access"
run_runtime_test "tests/struct_fn_test.zap" 0 "Structs as function parameters and return values"
run_runtime_test "tests/struct_array_test.zap" 0 "Arrays of structs"
run_runtime_test "tests/struct_types_test.zap" 0 "Structs with diverse field types"
run_runtime_test "tests/precedence_test.zap" 0 "Operator precedence (NOT vs Member access)"
run_runtime_test "tests/type_alias.zap" 42 "Type aliasing (alias Name = Type)"
run_runtime_test "tests/ref_test.zap" 0 "Reference type test"
run_runtime_test "tests/import_public/main.zap" 0 "Importing a public function through file namespace"
run_runtime_test "tests/import_flat/main.zap" 0 "Selective flat import with braces"
run_runtime_test "tests/import_alias/main.zap" 0 "Selective import alias with as"
run_runtime_test "tests/import_module_alias/main.zap" 0 "Module namespace alias with as"
run_runtime_test "tests/import_module_alias_same/main.zap" 0 "The same module may reuse the same alias"
run_test "tests/import_module_alias_conflict/main.zap" 1 "Different modules cannot reuse the same alias"
run_runtime_test "tests/import_folder/main.zap" 0 "Importing an entire folder as namespaces"
run_runtime_test "tests/import_canonical/main.zap" 0 "Import paths resolving to the same file share one module"
run_runtime_test "tests/import_type/main.zap" 0 "Using imported public struct types"
run_runtime_test "tests/import_std_io/main.zap" 0 "Importing the builtin std/io module"
run_runtime_test "tests/import_std_string/main.zap" 0 "Importing the builtin std/string module"
run_test "tests/import_private_fail/main.zap" 1 "Private module member access is rejected"

echo "-------------------------------"
echo "Results: $PASSED / $TOTAL passed"

if [ $PASSED -eq $TOTAL ]; then
    echo -e "${GREEN}ALL TESTS PASSED!${NC}"
    exit 0
else
    echo -e "${RED}SOME TESTS FAILED!${NC}"
    exit 1
fi
