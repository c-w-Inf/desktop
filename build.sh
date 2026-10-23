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

bear_db="$build_dir/compile_commands.bear.json"
cp "$build_dir/compile_commands.json" "$bear_db"
bear --append --output "$bear_db" -- \
    cmake --build "$build_dir" "$@"
cp "$bear_db" "$root/compile_commands.json"

# The agent sandbox bind-mounts the repository at /home/c_w, while the real
# checkout may live elsewhere. Keep CDB paths usable by the host clangd.
host_root="$root"
if [[ -r /proc/self/mountinfo ]]; then
    mounted_root="$(awk -v target="$root" '$5 == target { value = $4 } END { print value }' /proc/self/mountinfo)"
    if [[ -n "$mounted_root" && "$mounted_root" != "$root" ]]; then
        host_root="$mounted_root"
    fi
fi
python3 "$root/tools/compile_commands_for_host.py" "$root" "$host_root"
