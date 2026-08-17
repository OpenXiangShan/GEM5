#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/../.." && pwd)"
gem5_bin="${GEM5_DEBUG_BIN:-$repo_root/build/RISCV/gem5.debug}"
coremark_bin="${COREMARK_BIN:-/nfs/home/share/gem5_ci/checkpoints/coremark-riscv64-xs.bin}"
output_dir="${MEMORY_CHECK_OUTPUT_DIR:-$repo_root/valgrind-test}"
maxinsts="${MEMORY_CHECK_MAXINSTS:-100000}"

if [[ ! -x "$gem5_bin" ]]; then
  echo "missing executable gem5 debug binary: $gem5_bin" >&2
  exit 2
fi
if [[ ! -r "$coremark_bin" ]]; then
  echo "missing CoreMark image: $coremark_bin" >&2
  exit 2
fi
if [[ ! "$maxinsts" =~ ^[1-9][0-9]*$ ]]; then
  echo "MEMORY_CHECK_MAXINSTS must be a positive integer: $maxinsts" >&2
  exit 2
fi

export MEMORY_CHECK_OUTPUT_DIR="$output_dir"
mkdir -p "$output_dir/m5out"
cd "$repo_root"

exec "$script_dir/run-with-valgrind.sh" \
  "$gem5_bin" \
  --listener-mode=off \
  -d "$output_dir/m5out" \
  "$repo_root/configs/example/kmhv3.py" \
  --raw-cpt \
  --generic-rv-cpt="$coremark_bin" \
  --disable-difftest \
  --maxinsts="$maxinsts"
