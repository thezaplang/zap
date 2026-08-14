#!/usr/bin/env bash
set -euo pipefail

ZAPC="${1:-}"
INPUT="${2:-}"
C_CALLER="${3:-}"
LIBRARY="${4:-}"
OUTPUT="${5:-}"

if [[ -z "$ZAPC" || -z "$INPUT" || -z "$C_CALLER" || -z "$LIBRARY" || -z "$OUTPUT" ]]; then
    echo "Usage: $0 <zapc> <input.zp> <caller.c> <library.zp> <output>" >&2
    exit 1
fi

ZIR_OUTPUT="${OUTPUT}.zir"
OBJECT_OUTPUT="${OUTPUT}.o"

"$ZAPC" "$INPUT" -emit-zir -o "$ZIR_OUTPUT"

for required_symbol in 'library$used_24' used_5Fdependency retained_c_api retained_5Fentry 'extern @exit'; do
    if ! grep -Fq "$required_symbol" "$ZIR_OUTPUT"; then
        echo "Expected reachable symbol '$required_symbol' is absent from ZIR." >&2
        exit 1
    fi
done

for removed_symbol in unused_imported unused_external; do
    if grep -Fq "$removed_symbol" "$ZIR_OUTPUT"; then
        echo "Unreachable symbol '$removed_symbol' is present in ZIR." >&2
        exit 1
    fi
done

"$ZAPC" "$LIBRARY" -nostdlib -c -o "$OBJECT_OUTPUT"
cc "$C_CALLER" "$OBJECT_OUTPUT" -o "$OUTPUT"
"$OUTPUT"
