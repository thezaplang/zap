#!/usr/bin/env bash
set -euo pipefail

zapc=$(realpath "$1")
source_file=$(realpath "$2")
task_directory=$(mktemp -d)
trap 'rm -r "$task_directory"' EXIT
cd "$(dirname "$source_file")"

flags=(-noprelude)
if [[ ${3:-} == with-prelude ]]; then
    flags=()
fi

"$zapc" "${flags[@]}" "$source_file" -o "$task_directory/original"
"$task_directory/original"
"$zapc" --emit-expanded "${flags[@]}" "$source_file" -o "$task_directory/expanded.zp"
"$zapc" "${flags[@]}" "$task_directory/expanded.zp" -o "$task_directory/expanded"
"$task_directory/expanded"
