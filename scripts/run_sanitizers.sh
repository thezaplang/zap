#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd "$script_dir/.." && pwd)"
build_dir="${ZAP_SANITIZER_BUILD_DIR:-$repo_dir/build-sanitize}"

CC="${CC:-clang}" CXX="${CXX:-clang++}" meson setup "$build_dir" "$repo_dir" \
  --buildtype=debug \
  -Dzap_enable_sanitizers=true \
  -Dzap_enable_runtime_instrumentation=true \
  --reconfigure

meson compile -C "$build_dir"

leak_detection="${ZAP_DETECT_LEAKS:-1}"
export ASAN_OPTIONS="detect_leaks=$leak_detection:halt_on_error=1:abort_on_error=1"
export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"

meson test -C "$build_dir" --print-errorlogs

cd "$repo_dir"
python3 run_tests.py --zapc "$build_dir/zapc" -j 1 \
  tests/string_ownership_runtime_test.zp \
  tests/string_view_owned_semantics_test.zp \
  tests/class_arc_test.zp \
  tests/class_arc_strong_test.zp \
  tests/class_cycle_weak_tombstone_test.zp \
  tests/class_record_cycle_detect_test.zp \
  tests/class_weak_lock_test.zp \
  tests/failable_class_return_test.zp \
  tests/std_network_test.zp