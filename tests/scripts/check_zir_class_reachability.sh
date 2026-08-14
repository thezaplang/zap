#!/usr/bin/env bash
set -euo pipefail

ZAPC="${1:-}"
INPUT="${2:-}"
OUTPUT="${3:-}"

if [[ -z "$ZAPC" || -z "$INPUT" || -z "$OUTPUT" ]]; then
    echo "Usage: $0 <zapc> <input.zp> <output>" >&2
    exit 1
fi

"$ZAPC" "$INPUT" -emit-zir -o "$OUTPUT"

for required_symbol in Base_24run Derived_24run Base_24deinit Derived_24deinit; do
    if ! grep -Fq "$required_symbol" "$OUTPUT"; then
        echo "Expected live class symbol '$required_symbol' is absent from ZIR." >&2
        exit 1
    fi
done

for removed_symbol in unused_5Fbase_5Fmethod unused_5Fderived_5Fmethod; do
    if grep -Fq "$removed_symbol" "$OUTPUT"; then
        echo "Unreachable class symbol '$removed_symbol' is present in ZIR." >&2
        exit 1
    fi
done
