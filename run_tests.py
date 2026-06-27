#!/usr/bin/env python3
import os
import sys
import shutil
import subprocess
import threading
import concurrent.futures
import time
import argparse

# Console colors
if sys.stdout.isatty():
    GREEN = '\033[0;32m'
    RED = '\033[0;31m'
    YELLOW = '\033[1;33m'
    NC = '\033[0m'
else:
    GREEN = RED = YELLOW = NC = ''

# Special test configurations
SPECIAL_CASES = {
    # Compile-only exit 0
    "tests/valid.zp": {"type": "compile", "exit": 0, "desc": "Valid program"},
    "tests/return_void.zp": {"type": "compile", "exit": 0, "desc": "Return in void function"},
    "tests/modulo_test.zp": {"type": "compile", "exit": 0, "desc": "Modulo operator test"},
    "tests/failable_error_class_compile_test.zp": {"type": "compile", "exit": 0, "desc": "Failable: @error class can be used as error type E in T!E"},
    "tests/global_numeric_pointer_compile_test.zp": {"type": "compile", "exit": 0, "desc": "Numeric constants initialize global pointers"},

    # Compile-only exit 1 (non-matching filename)
    "tests/array_const_size.zp": {"type": "compile", "exit": 1, "desc": "Array size as a constant is currently rejected"},
    "tests/import_module_alias_conflict/main.zp": {"type": "compile", "exit": 1, "desc": "Different modules cannot reuse the same alias"},
    "tests/import_cycle/main.zp": {"type": "compile", "exit": 1, "desc": "Cyclic imports are rejected"},
    "tests/import_private_fail/main.zp": {"type": "compile", "exit": 1, "desc": "Private module member access is rejected"},
    "tests/diagnostics/07_attributes_parser_sync.zp": {"type": "compile", "exit": 1, "desc": "Diagnostics: parser synchronization with malformed attributes"},
    "tests/import_map_diagnostic/main.zp": {
        "type": "compile",
        "exit": 1,
        "compile_flags": ["--import-map", "@lib=tests/import_map_diagnostic/src/lib"],
        "stderr_pattern": "broken.zp:2:10",
        "desc": "Diagnostics in import-map targets report the imported file"
    },

    # Runtime with non-zero exit
    "tests/type_inference_test.zp": {"type": "runtime", "exit": 0, "desc": "Type inference: var/const without annotation, struct field type-directed binding"},
    "tests/return_type_infer_test.zp": {"type": "runtime", "exit": 0, "desc": "Type inference: function return type inferred from return statements"},
    "tests/generic_struct_infer_test.zp": {"type": "runtime", "exit": 0, "desc": "Type inference: generic struct type args inferred from field values"},
    "tests/enum_test.zp": {"type": "runtime", "exit": 1, "desc": "Enum test"},
    "tests/type_alias.zp": {"type": "runtime", "exit": 42, "desc": "Type aliasing (alias Name = Type)"},
    "tests/global_string_concat_test.zp": {"type": "runtime", "exit": 42, "desc": "Global var initialized with literal '+' concatenation is constant-folded"},
    "tests/failable_class_return_test.zp": {"type": "runtime", "exit": 42, "desc": "Failable function returning a class instance (success and fail paths)"},
    "tests/control_flow.zp": {"type": "runtime", "exit": 1, "desc": "Control flow return value"},
    "tests/compound_assign_test.zp": {"type": "runtime", "exit": 42, "desc": "Compound assignment (+= -= *= /= %= <<= >>= |= &= ^=) and ++/--"},
    "tests/compound_assign_eval_once_test.zp": {"type": "runtime", "exit": 1, "desc": "Compound assignment evaluates its target exactly once"},
    "tests/array_dynamic_index_test.zp": {"type": "runtime", "exit": 45, "desc": "Assigning through a runtime-computed array index"},
    "tests/global_pointer_initializer_test.zp": {"type": "runtime", "exit": 74, "desc": "Global pointers preserve scalar and array-element addresses"},
    "tests/function_call_test.zp": {"type": "runtime", "exit": 1, "desc": "Function call tests"},

    # Runtime with custom run arguments
    "tests/process_args_test.zp": {"type": "runtime", "exit": 0, "run_args": ["alpha", "beta", "gamma"], "desc": "Process argument access"},

    # Custom compile flags and output check
    "tests/nostdlib_ext_main_object.zp": {
        "type": "compile",
        "exit": 0,
        "compile_flags": ["-nostdlib", "-c"],
        "output_file": "tests/nostdlib_ext_main_object.o",
        "desc": "Object compile with -nostdlib and external main"
    },
    "tests/core_nostdlib_test.zp": {
        "type": "compile",
        "exit": 0,
        "compile_flags": ["-nostdlib", "-c"],
        "output_file": "tests/core_nostdlib_test.o",
        "desc": "Core StringView helpers remain available with -nostdlib"
    },
    "tests/core_implicit_disabled_error.zp": {
        "type": "compile",
        "exit": 1,
        "compile_flags": ["-nostdlib", "-c"],
        "desc": "Core is not implicitly imported with -nostdlib"
    },
    "tests/inline_asm_x86_io_test.zp": {
        "type": "compile",
        "exit": 0,
        "compile_flags": ["-nostdlib", "-c"],
        "output_file": "tests/inline_asm_x86_io_test.o",
        "desc": "Inline asm accepts GCC-style x86 port I/O constraints"
    },

    # Warnings
    "tests/warn_missing_return.zp": {
        "type": "warning",
        "stderr_pattern": "has non-void return type but no return",
        "desc": "Warning: missing return in non-void function"
    },
    "tests/attribute_unknown_warning.zp": {
        "type": "warning",
        "stderr_pattern": "unknown attribute 'unknownAttr'",
        "desc": "Warning: unknown attribute is reported"
    },

    # Diagnostics
    "tests/diagnostics/01_caret_alignment.zp": {
        "type": "diagnostic",
        "exit": 1,
        "diagnostics": ["S2002", "S2003", "S2009", "N1000"],
        "desc": "Diagnostics: caret alignment and semantic codes"
    },
    "tests/diagnostics/02_parser_sync.zp": {
        "type": "diagnostic",
        "exit": 1,
        "diagnostics": ["P1002", "P1003", "P1004", "N1000"],
        "desc": "Diagnostics: parser synchronization with coded parse errors"
    },
    "tests/diagnostics/03_type_messages.zp": {
        "type": "diagnostic",
        "exit": 1,
        "diagnostics": ["S2002", "S2003", "S2007", "S2008", "S2010", "N1000"],
        "desc": "Diagnostics: user-facing semantic type messages with codes"
    },
    "tests/diagnostics/04_codes_parser.zp": {
        "type": "diagnostic",
        "exit": 1,
        "diagnostics": ["P1002", "P1003", "P1004", "N1000"],
        "desc": "Diagnostics: parser code coverage regression sample"
    },
    "tests/diagnostics/05_codes_semantic.zp": {
        "type": "diagnostic",
        "exit": 1,
        "diagnostics": ["S2001", "S2002", "S2003", "S2005", "S2006", "S2007", "S2008", "S2009", "S2010", "S2012", "N1000", "E0000"],
        "desc": "Diagnostics: semantic code coverage regression sample"
    },
    "tests/diagnostics/06_codes_cascade_stress.zp": {
        "type": "diagnostic",
        "exit": 1,
        "diagnostics": ["P1002", "P1003", "P1004", "N1000"],
        "desc": "Diagnostics: cascade stress remains bounded and coded"
    },
}

# Duplicate / extra compiler verification runs
EXTRA_TESTS = [
    {
        "file": "tests/valid.zp",
        "type": "compile",
        "desc": "Emit LLVM IR for a valid program",
        "compile_flags": ["-S", "-emit-llvm"],
        "output_file": "/tmp/zap-valid.ll"
    },
    {
        "file": "tests/valid.zp",
        "type": "compile",
        "desc": "Emit LLVM IR with explicit target triple",
        "compile_flags": ["--target=x86_64-pc-windows-msvc", "-S", "-emit-llvm"],
        "output_file": "/tmp/zap-target.ll",
        "output_pattern": 'target triple = "x86_64-pc-windows-msvc"'
    },
    {
        "file": "tests/valid.zp",
        "type": "compile",
        "desc": "Emit freestanding LLVM IR without host main args",
        "compile_flags": ["--freestanding", "--target=x86_64-unknown-none", "-S", "-emit-llvm"],
        "output_file": "/tmp/zap-freestanding.ll",
        "output_pattern": "define i64 @main()"
    },
    {
        "file": "tests/valid.zp",
        "type": "compile",
        "desc": "Freestanding executable link is rejected",
        "compile_flags": ["--freestanding"],
        "exit": 1,
        "stderr_pattern": "cannot link executable in freestanding mode"
    },
    {
        "file": "tests/logical_ops.zp",
        "type": "compile",
        "desc": "Emit LLVM IR for logical operators",
        "compile_flags": ["-S", "-emit-llvm"],
        "output_file": "/tmp/zap-logical.ll"
    },
    {
        "file": "tests/function_call_test.zp",
        "type": "compile",
        "desc": "Emit LLVM IR for function calls",
        "compile_flags": ["-S", "-emit-llvm"],
        "output_file": "/tmp/zap-calls.ll"
    },
    {
        "file": "tests/struct_test.zp",
        "type": "compile",
        "desc": "Emit LLVM IR for struct member access",
        "compile_flags": ["-S", "-emit-llvm"],
        "output_file": "/tmp/zap-struct.ll"
    },
    {
        "file": "tests/struct_packed_ir_test.zp",
        "type": "compile",
        "desc": "Emit LLVM IR for packed structs",
        "compile_flags": ["-S", "-emit-llvm"],
        "output_file": "/tmp/zap-packed-struct.ll",
        "output_pattern": "PackedBits\" = type <{ i8, i32, i8 }>"
    },
    {
        "file": "tests/struct_packed_repr_c_ir_test.zp",
        "type": "compile",
        "desc": "Emit LLVM IR for packed @repr(\"C\") structs",
        "compile_flags": ["-S", "-emit-llvm"],
        "output_file": "/tmp/zap-packed-repr-c-struct.ll",
        "output_pattern": "%CPacket = type <{ i8, i32, i8 }>"
    },
    {
        "file": "tests/generic_packed_struct_ir_test.zp",
        "type": "compile",
        "desc": "Emit LLVM IR for generic packed structs",
        "compile_flags": ["-S", "-emit-llvm"],
        "output_file": "/tmp/zap-generic-packed-struct.ll",
        "output_pattern": "PackedBox$g$i32\" = type <{ i8, i32, i8 }>"
    },
    {
        "file": "tests/inline_asm_x86_io_test.zp",
        "type": "compile",
        "desc": "Emit LLVM IR for GCC-style x86 inline asm",
        "compile_flags": ["-nostdlib", "-S", "-emit-llvm"],
        "output_file": "/tmp/zap-inline-asm-x86-io.ll",
        "output_pattern": "asm sideeffect \"inb $1, $0\", \"={ax},N{dx}\""
    },
    {
        "file": "tests/enum_payload_test.zp",
        "type": "compile",
        "desc": "Emit LLVM IR for enum payloads",
        "compile_flags": ["-S", "-emit-llvm"],
        "output_file": "/tmp/zap-enum-payload.ll",
        "output_pattern": "Value\" = type { i32, i32 }"
    },
    {
        "file": "tests/prelude_implicit_collection_test.zp",
        "type": "compile",
        "desc": "Disabling prelude with -noprelude fails compilation",
        "compile_flags": ["-noprelude"],
        "exit": 1
    }
]

def discover_tests():
    """Finds all test files in the tests/ directory based on conventions."""
    test_files = []
    for root, _, files in os.walk("tests"):
        for file in files:
            if file.endswith(".zp"):
                # Skip scratch files starting with tmp_
                if file.startswith("tmp_"):
                    continue
                    
                file_path = os.path.join(root, file)
                
                # Rules to determine if it is a standalone test entry point:
                is_test = False
                if root == "tests":
                    is_test = True
                elif root == os.path.join("tests", "diagnostics"):
                    is_test = True
                elif file == "main.zp" or file.startswith("main_"):
                    is_test = True
                    
                if is_test:
                    test_files.append(file_path)
                    
    test_files.sort()
    return test_files

def get_test_config(file_path):
    """Returns the test execution configuration based on defaults or SPECIAL_CASES."""
    # Check if we have exact match
    # Standardize path separators just in case
    key = file_path.replace("\\", "/")
    if key in SPECIAL_CASES:
        config = SPECIAL_CASES[key].copy()
        config["file"] = file_path
        return config
        
    # Default fallbacks
    filename = os.path.basename(file_path)
    config = {
        "file": file_path,
        "desc": f"Test {file_path}"
    }
    
    # Check if this filename specifies it is an error or failure test
    # but not a regular test file (ending with _test.zp)
    is_test_suffix = filename.endswith("_test.zp")
    
    # Classify error tests
    if not is_test_suffix and ("error" in filename or "fail" in filename or "outside" in filename):
        config["type"] = "compile"
        config["exit"] = 1
    # Classify warning tests
    elif not is_test_suffix and "warn" in filename:
        config["type"] = "warning"
        config["exit"] = 0
        config["stderr_pattern"] = "has non-void return type but no return"
    # Normal runtime tests
    else:
        config["type"] = "runtime"
        config["exit"] = 0
        
    return config

def execute_test(test_item, zapc_path):
    """Executes a single test case according to its configuration."""
    file_path = test_item["file"]
    test_type = test_item.get("type", "runtime")
    expected_exit = test_item.get("exit", 1 if test_type == "diagnostic" else 0)
    compile_flags = test_item.get("compile_flags", [])
    run_args = test_item.get("run_args", [])
    stderr_pattern = test_item.get("stderr_pattern", None)
    diagnostics = test_item.get("diagnostics", [])
    output_file = test_item.get("output_file", None)
    output_pattern = test_item.get("output_pattern", None)
    
    to_cleanup = []
    
    try:
        if test_type == "runtime":
            # Generate a unique binary name per thread to prevent collision
            tid = threading.get_ident()
            bin_path = f"{file_path}_{tid}.bin"
            to_cleanup.append(bin_path)
            
            # Compile step
            cmd = [zapc_path, file_path] + compile_flags + ["-o", bin_path]
            res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            
            if res.returncode != 0:
                return False, f"Compilation failed with exit code {res.returncode}\nStderr:\n{res.stderr}"
            
            if not os.path.exists(bin_path):
                return False, f"Compiled binary not found at: {bin_path}"
                
            # Run step
            run_cmd = [bin_path] + run_args
            res_run = subprocess.run(run_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            
            if res_run.returncode != expected_exit:
                return False, f"Runtime failed: expected exit code {expected_exit}, got {res_run.returncode}\nStderr:\n{res_run.stderr}\nStdout:\n{res_run.stdout}"
                
            return True, None
            
        elif test_type == "compile":
            cmd = [zapc_path, file_path] + compile_flags
            if output_file:
                cmd += ["-o", output_file]
                to_cleanup.append(output_file)
                
            res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            
            if res.returncode != expected_exit:
                return False, f"Compiler exit code: expected {expected_exit}, got {res.returncode}\nStderr:\n{res.stderr}"
                
            if output_file:
                if not os.path.exists(output_file):
                    return False, f"Expected output file not found: {output_file}"
                if output_pattern:
                    with open(output_file, "r", encoding="utf-8", errors="replace") as f:
                        output_text = f.read()
                    if output_pattern not in output_text:
                        return False, f"Expected output pattern '{output_pattern}' not found in {output_file}"
                    
            if stderr_pattern:
                if stderr_pattern.lower() not in res.stderr.lower():
                    return False, f"Expected warning pattern '{stderr_pattern}' not found in stderr:\n{res.stderr}"
                    
            return True, None
            
        elif test_type == "warning":
            cmd = [zapc_path, file_path] + compile_flags
            res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            
            if res.returncode != expected_exit:
                return False, f"Compiler exit code: expected {expected_exit}, got {res.returncode}\nStderr:\n{res.stderr}"
                
            if stderr_pattern:
                if stderr_pattern.lower() not in res.stderr.lower():
                    return False, f"Expected warning pattern '{stderr_pattern}' not found in stderr:\n{res.stderr}"
                    
            return True, None
            
        elif test_type == "diagnostic":
            cmd = [zapc_path, file_path] + compile_flags
            res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            
            if res.returncode != expected_exit:
                return False, f"Compiler exit code: expected {expected_exit}, got {res.returncode}\nStderr:\n{res.stderr}"
                
            missing_codes = []
            for code in diagnostics:
                if code not in res.stderr:
                    missing_codes.append(code)
            if missing_codes:
                return False, f"Missing diagnostic codes: {', '.join(missing_codes)}\nStderr:\n{res.stderr}"
                
            return True, None
            
        else:
            return False, f"Unknown test type: {test_type}"
            
    except Exception as e:
        return False, f"Exception during test run: {str(e)}"
        
    finally:
        for path in to_cleanup:
            try:
                if os.path.exists(path):
                    if os.path.isdir(path):
                        shutil.rmtree(path)
                    else:
                        os.remove(path)
            except Exception:
                pass

def build_compiler():
    """Builds the Zap compiler using build.sh."""
    print(f"{YELLOW}Building compiler...{NC}")
    if os.name == 'posix':
        res = subprocess.run(["./build.sh"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    else:
        res = subprocess.run(["build.bat"], shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        
    if res.returncode != 0:
        print(f"{RED}Failed to build the compiler!{NC}")
        print(res.stderr.decode(errors='replace'))
        sys.exit(1)
        
    zapc = "./build/zapc"
    if os.name == 'nt':
        zapc += ".exe"
        
    if not os.path.exists(zapc):
        print(f"{RED}Compiler executable not found at {zapc}{NC}")
        sys.exit(1)
        
    print(f"{GREEN}Compiler built successfully!{NC}\n")
    return zapc

def main():
    parser = argparse.ArgumentParser(description="Zap Compiler Test Suite Runner")
    parser.add_argument("tests", nargs="*", help="Optional list of specific test files to run")
    parser.add_argument("-j", "--jobs", type=int, default=None, help="Number of parallel test jobs (default: CPU cores - 1)")
    parser.add_argument("-l", "--list", action="store_true", help="List discovered tests and exit")
    parser.add_argument("-v", "--verbose", action="store_true", help="Print detailed failure reasons")
    args = parser.parse_args()

    # Step 1: Discover test files
    discovered = discover_tests()
    
    # Generate test suite list
    test_suite = []
    for f in discovered:
        # Check if we should filter tests
        if args.tests:
            # Match if any arg matches or is a prefix/suffix
            matched = False
            for target in args.tests:
                if target in f or f in target:
                    matched = True
                    break
            if not matched:
                continue
                
        test_suite.append(get_test_config(f))

    # Add extra compiler validation runs if we are running everything or matching them
    for extra in EXTRA_TESTS:
        if args.tests:
            matched = False
            for target in args.tests:
                if target in extra["file"] or extra["file"] in target or target in extra.get("desc", ""):
                    matched = True
                    break
            if not matched:
                continue
        test_suite.append(extra)

    if args.list:
        print(f"Discovered {len(test_suite)} tests:")
        for t in test_suite:
            desc = t.get("desc", t["file"])
            print(f" - {desc} ({t['file']}) [{t.get('type', 'runtime')}]")
        return

    if not test_suite:
        print(f"{YELLOW}No matching tests found.{NC}")
        return

    # Step 2: Build the compiler
    zapc_path = build_compiler()

    print("--- Zap Compiler Test Suite ---")
    total_tests = len(test_suite)
    
    # Configure parallel jobs
    num_jobs = args.jobs
    if num_jobs is None:
        num_jobs = max(1, os.cpu_count() - 1)
        
    progress_lock = threading.Lock()
    completed = 0
    passed = 0
    failed_details = []

    def run_wrapper(test_item):
        nonlocal completed, passed
        desc = test_item.get("desc", test_item["file"])
        file_path = test_item["file"]
        
        ok, err_msg = execute_test(test_item, zapc_path)
        
        with progress_lock:
            completed += 1
            if ok:
                passed += 1
                status_str = f"{GREEN}PASS{NC}"
            else:
                status_str = f"{RED}FAIL{NC}"
                failed_details.append((desc, file_path, err_msg))
                
            print(f"[{completed:3d}/{total_tests:3d}] Running {desc} ({file_path})... {status_str}")

    start_time = time.time()
    
    # Run tests using thread pool
    with concurrent.futures.ThreadPoolExecutor(max_workers=num_jobs) as executor:
        executor.map(run_wrapper, test_suite)
        
    duration = time.time() - start_time
    
    # Print results
    print("-------------------------------")
    print(f"Results: {passed} / {total_tests} passed (took {duration:.2f} seconds)")
    
    if failed_details:
        print(f"\n{RED}Failed Tests Detail:{NC}")
        for idx, (desc, file_path, err_msg) in enumerate(failed_details, 1):
            print(f"{idx}. {desc} ({file_path})")
            if args.verbose or True: # Always show error details for convenience
                # Indent error message
                indented = "\n".join("   " + line for line in err_msg.splitlines())
                print(indented)
            print()
            
        print(f"{RED}SOME TESTS FAILED!{NC}")
        sys.exit(1)
    else:
        print(f"{GREEN}ALL TESTS PASSED!{NC}")
        sys.exit(0)

if __name__ == "__main__":
    main()
