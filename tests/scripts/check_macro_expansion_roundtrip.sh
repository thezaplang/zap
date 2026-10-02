#!/usr/bin/env bash
set -euo pipefail

zapc=$1
source_file=$2
task_directory=$(mktemp -d)
trap 'rm -r "$task_directory"' EXIT

"$zapc" -noprelude "$source_file" -o "$task_directory/original"
"$task_directory/original"
"$zapc" --emit-expanded -noprelude "$source_file" -o "$task_directory/expanded.zp"
"$zapc" -noprelude "$task_directory/expanded.zp" -o "$task_directory/expanded"
"$task_directory/expanded"
