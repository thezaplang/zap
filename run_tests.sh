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
run_test "tests/valid.zp" 0 "Valid program"
run_test "tests/return_void.zp" 0 "Return in void function"
run_test "tests/modulo_test.zp" 0 "Modulo operator test"

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

run_runtime_args_test() {
    local file=$1
    local expected_exit_code=$2
    local description=$3
    shift 3

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

    ./$binfile "$@" > /dev/null 2>&1
    local run_code=$?
    rm -f "$binfile"

    if [ $run_code -eq $expected_exit_code ]; then
        echo -e "${GREEN}PASS${NC}"
        ((PASSED++))
    else
        echo -e "${RED}FAIL${NC} (expected $expected_exit_code, got $run_code)"
    fi
}

run_compile_args_test() {
    local file=$1
    local output=$2
    local description=$3
    shift 3

    ((TOTAL++))
    echo -n "Running $description ($file)... "

    rm -f "$output"
    $ZAPC "$file" "$@" -o "$output" > /dev/null 2>&1
    local exit_code=$?

    if [ $exit_code -ne 0 ]; then
        echo -e "${RED}FAIL${NC} (compile failed)"
        rm -f "$output"
        return
    fi

    if [ ! -f "$output" ]; then
        echo -e "${RED}FAIL${NC} (output not found)"
        return
    fi

    rm -f "$output"
    echo -e "${GREEN}PASS${NC}"
    ((PASSED++))
}

run_test_args() {
    local file=$1
    local expected_exit_code=$2
    local description=$3
    shift 3

    ((TOTAL++))
    echo -n "Running $description ($file)... "

    $ZAPC "$file" "$@" > /dev/null 2>&1
    local exit_code=$?

    if [ $exit_code -eq $expected_exit_code ]; then
        echo -e "${GREEN}PASS${NC}"
        ((PASSED++))
    else
        echo -e "${RED}FAIL${NC} (expected $expected_exit_code, got $exit_code)"
    fi
}

run_runtime_compile_args_test() {
    local file=$1
    local expected_exit_code=$2
    local description=$3
    shift 3

    ((TOTAL++))
    echo -n "Running $description ($file)... "

    binfile="${file%.*}"
    $ZAPC "$file" "$@" -o "$binfile" > /dev/null 2>&1
    local compile_code=$?
    if [ $compile_code -ne 0 ]; then
        echo -e "${RED}FAIL${NC} (compile failed)"
        rm -f "$binfile"
        return
    fi

    if [ ! -x "$binfile" ]; then
        echo -e "${RED}FAIL${NC} (binary not found)"
        rm -f "$binfile"
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
run_warning_test "tests/warn_missing_return.zp" "Warning: missing return in non-void function"

# Runtime test: main without explicit return type should default to Int and return 0
run_runtime_test "tests/main_implicit.zp" 0 "Main implicit return type and implicit return 0"
run_compile_args_test "tests/nostdlib_ext_main_object.zp" "tests/nostdlib_ext_main_object.o" "Object compile with -nostdlib and external main" -nostdlib -c
run_runtime_test "tests/ext_default_void_runtime.zp" 0 "External function without return type defaults to Void at runtime"
run_runtime_args_test "tests/process_args_test.zp" 0 "Process argument access" alpha beta gamma
run_runtime_test "tests/process_exec_test.zp" 0 "Process execution"
run_runtime_test "tests/process_cwd_test.zp" 0 "Current working directory access"
run_runtime_test "tests/fs_test.zp" 0 "Filesystem path checks"
run_runtime_test "tests/fs_mkdir_test.zp" 0 "Filesystem directory creation"
run_runtime_test "tests/fs_file_io_test.zp" 0 "Filesystem text file IO"
run_runtime_test "tests/path_test.zp" 0 "Path helpers"
run_runtime_test "tests/math_test.zp" 0 "Math stdlib helpers"
run_runtime_compile_args_test "tests/unsafe_runtime_test.zp" 0 "Unsafe pointers" --allow-unsafe
run_runtime_compile_args_test "tests/unsafe_heap_test.zp" 0 "Unsafe heap pointers" --allow-unsafe
run_runtime_compile_args_test "tests/unsafe_void_ptr_test.zp" 0 "Unsafe void pointers" --allow-unsafe
run_runtime_compile_args_test "tests/unsafe_struct_runtime_test.zp" 0 "Unsafe structs" --allow-unsafe
run_runtime_compile_args_test "tests/unsafe_return_runtime_test.zp" 0 "Unsafe block early return" --allow-unsafe
run_runtime_compile_args_test "tests/struct_pointer_field_test.zp" 0 "Struct pointer fields" --allow-unsafe
run_runtime_compile_args_test "tests/ptr_deref_field_test.zp" 0 "Pointer dereference field access (*ptr).field" --allow-unsafe
run_runtime_compile_args_test "tests/ptr_deref_nested_field_test.zp" 0 "Pointer dereference nested field access" --allow-unsafe

# Lexer errors (exit code 1)
run_test "tests/lexer_error.zp" 1 "Lexer error: Unterminated string"

# Syntax errors (exit code 1)
run_test "tests/syntax_error.zp" 1 "Syntax error: Missing semicolons"

# Semantic errors (exit code 1)
run_test "tests/sema_error.zp" 1 "Semantic error: Undefined variable"
run_test "tests/ext_default_void_type_error.zp" 1 "External function without return type cannot be used as Int"
run_test "tests/unsafe_requires_flag.zp" 1 "Unsafe features require --allow-unsafe"
run_test_args "tests/unsafe_scope_error.zp" 1 "Unsafe features are scoped" --allow-unsafe
run_test_args "tests/unsafe_fun_call_scope_error.zp" 1 "Unsafe function calls are scoped" --allow-unsafe
run_test_args "tests/unsafe_struct_scope_error.zp" 1 "Unsafe struct usage is scoped" --allow-unsafe

# Multiple errors (exit code 1)
run_test "tests/syntax_error.zp" 1 "Multiple syntax errors"

# Concat tests
run_runtime_test "tests/concat.zp" 0 "Concat literal strings"
run_runtime_test "tests/concat_char.zp" 0 "Concat String and Char"
run_runtime_test "tests/string_index_test.zp" 0 "String indexing"
run_runtime_test "tests/string_stdlib_test.zp" 0 "String stdlib helpers"
run_test "tests/string_index_assign_error.zp" 1 "String indexing is read-only"

# Logical operator tests
run_runtime_test "tests/logical_ops.zp" 0 "Logical operators (&&, ||) with short-circuiting"
run_test "tests/logical_type_error.zp" 1 "Logical operators type check"
run_runtime_test "tests/if_else_if.zp" 0 "If / else if / else chains"
run_runtime_test "tests/if_nested.zp" 0 "Nested if statements"
run_runtime_test "tests/if_state_merge.zp" 0 "If branches mutating shared state"
run_runtime_test "tests/if_return_paths.zp" 0 "If branches with multiple return paths"
run_test "tests/if_condition_error.zp" 1 "If condition type check"
run_runtime_test "tests/ternary_test.zp" 0 "Ternary operator"
run_test "tests/ternary_condition_error.zp" 1 "Ternary condition type check"
run_test "tests/ternary_type_error.zp" 1 "Ternary branch type check"
run_runtime_test "tests/enum_test.zp" 1 "Enum test"
run_runtime_test "tests/enum_trailing_comma_test.zp" 0 "Enum trailing comma"
run_runtime_test "tests/array_test.zp" 0 "Array declaration, initialization, and indexing"
run_test "tests/array_const_size.zp" 1 "Array size as a constant is currently rejected"

# Struct tests
run_runtime_test "tests/struct_test.zp" 0 "Basic struct member access"
run_runtime_test "tests/struct_nested_test.zp" 0 "Nested struct member access"
run_runtime_test "tests/struct_fn_test.zp" 0 "Structs as function parameters and return values"
run_runtime_test "tests/struct_array_test.zp" 0 "Arrays of structs"
run_runtime_test "tests/struct_types_test.zp" 0 "Structs with diverse field types"
run_test "tests/struct_compare_error.zp" 1 "Struct comparison is rejected"
run_runtime_test "tests/precedence_test.zp" 0 "Operator precedence (NOT vs Member access)"
run_runtime_test "tests/type_alias.zp" 42 "Type aliasing (alias Name = Type)"
run_runtime_test "tests/ref_test.zp" 0 "Reference type test"
run_runtime_test "tests/varargs_syntax_test.zp" 0 "Variadic syntax (... and ...T)"
run_runtime_test "tests/varargs_forwarding_test.zp" 0 "Variadic forwarding"
run_runtime_compile_args_test "tests/ext_c_varargs_test.zp" 0 "ext C varargs" --allow-unsafe
run_runtime_test "tests/std_io_printf_test.zp" 0 "std/io printf wrappers"
run_runtime_test "tests/import_public/main.zp" 0 "Importing a public function through file namespace"
run_runtime_test "tests/import_flat/main.zp" 0 "Selective flat import with braces"
run_runtime_test "tests/import_alias/main.zp" 0 "Selective import alias with as"
run_runtime_test "tests/import_module_alias/main.zp" 0 "Module namespace alias with as"
run_runtime_test "tests/import_module_alias_same/main.zp" 0 "The same module may reuse the same alias"
run_runtime_test "tests/import_std_string/main.zp" 0 "Importing std/string module namespace"
run_runtime_test "tests/import_std_error/main.zp" 0 "Importing std/error types"
run_test "tests/import_module_alias_conflict/main.zp" 1 "Different modules cannot reuse the same alias"
run_runtime_test "tests/import_folder/main.zp" 0 "Importing an entire folder as namespaces"
run_runtime_test "tests/import_canonical/main.zp" 0 "Import paths resolving to the same file share one module"
run_test "tests/import_cycle/main.zp" 1 "Cyclic imports are rejected"
run_runtime_test "tests/import_type/main.zp" 0 "Using imported public struct types"
run_runtime_test "tests/import_std_io/main.zp" 0 "Importing the builtin std/io module"
run_runtime_test "tests/import_std_string/main.zp" 0 "Importing the builtin std/string module"
run_test "tests/import_private_fail/main.zp" 1 "Private module member access is rejected"

echo "-------------------------------"
echo "Results: $PASSED / $TOTAL passed"

if [ $PASSED -eq $TOTAL ]; then
    echo -e "${GREEN}ALL TESTS PASSED!${NC}"
    exit 0
else
    echo -e "${RED}SOME TESTS FAILED!${NC}"
    exit 1
fi
