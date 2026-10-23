#!/usr/bin/env bash
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${BUILD_DIR:-$root/build}"

if [[ ! -f "$build_dir/CMakeCache.txt" ]]; then
    cmake -S "$root" -B "$build_dir" -G Ninja \
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Release}"
fi

if ! command -v bear >/dev/null 2>&1; then
    echo "error: bear is required to generate compile_commands.json" >&2
    exit 1
fi

bear --append --output "$root/compile_commands.json" -- \
    cmake --build "$build_dir" "$@"

if [[ ! -s "$root/compile_commands.json" ]] || \
   [[ "$(tr -d "[] \n" < "$root/compile_commands.json")" == "" ]]; then
    cp "$build_dir/compile_commands.json" "$root/compile_commands.json"
fi
