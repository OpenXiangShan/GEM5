#!/usr/bin/env bash

set -euo pipefail

if (($# == 0)); then
  echo "usage: $0 COMMAND [ARG ...]" >&2
  exit 2
fi

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
output_dir="${MEMORY_CHECK_OUTPUT_DIR:-$PWD/valgrind-test}"
suppression_file="${MEMORY_CHECK_SUPPRESSIONS:-$script_dir/../valgrind-suppressions}"
mkdir -p "$output_dir"

valgrind \
  --tool=memcheck \
  --track-origins=yes \
  --leak-check=full \
  --show-leak-kinds=all \
  --errors-for-leak-kinds=definite \
  --error-limit=no \
  --error-exitcode=99 \
  --suppressions="$suppression_file" \
  --log-file="$output_dir/valgrind.log" \
  -s \
  -- \
  "$@"
