#!/usr/bin/env bash
set -euo pipefail

zapc=$1
source_file=$2
snapshot=$3
semantic_error_file=$4
output_file=$5

diff -u "$snapshot" <("$zapc" --emit-expanded -noprelude "$source_file")
"$zapc" --emit-expanded -noprelude "$source_file" -o "$output_file"
diff -u "$snapshot" "$output_file"

expanded=$("$zapc" --emit-expanded -noprelude "$semantic_error_file")
if [[ "$expanded" != *"caller_local"* ]]; then
    echo "expanded output did not bypass semantic analysis" >&2
    exit 1
fi

if "$zapc" --emit-expanded -emit-zir -noprelude "$source_file" >/dev/null 2>&1; then
    echo "conflicting output modes were accepted" >&2
    exit 1
fi

if "$zapc" --emit-expanded -noprelude "$source_file" -o "$source_file" >/dev/null 2>&1; then
    echo "expanded output overwrote an input module" >&2
    exit 1
fi
