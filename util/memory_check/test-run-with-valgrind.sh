#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
wrapper="$script_dir/run-with-valgrind.sh"
build_dir="$(mktemp -d)"
output_root="${MEMORY_CHECK_OUTPUT_DIR:-$PWD/valgrind-test}/self-test"
trap 'rm -rf "$build_dir"' EXIT
mkdir -p "$output_root"

cc -O0 -g -Wall -Wextra -Wno-use-after-free \
  "$script_dir/tests/memcheck_target.c" \
  -o "$build_dir/memcheck-target"

run_success_test() {
  local output_dir="$output_root/success"

  MEMORY_CHECK_OUTPUT_DIR="$output_dir" \
    "$wrapper" "$build_dir/memcheck-target" success "argument with spaces"

  test -s "$output_dir/valgrind.log"
  grep -q 'ERROR SUMMARY: 0 errors' "$output_dir/valgrind.log"
}

run_invalid_read_test() {
  local output_dir="$output_root/invalid-read"
  local status=0

  MEMORY_CHECK_OUTPUT_DIR="$output_dir" \
    "$wrapper" "$build_dir/memcheck-target" invalid-read || status=$?

  if [[ "$status" -ne 99 ]]; then
    echo "FAIL: invalid read returned $status instead of 99" >&2
    return 1
  fi

  grep -q 'Invalid read' "$output_dir/valgrind.log"
}

run_invalid_write_test() {
  local output_dir="$output_root/invalid-write"
  local status=0

  MEMORY_CHECK_OUTPUT_DIR="$output_dir" \
    "$wrapper" "$build_dir/memcheck-target" invalid-write || status=$?

  if [[ "$status" -ne 99 ]]; then
    echo "FAIL: invalid write returned $status instead of 99" >&2
    return 1
  fi

  grep -q 'Invalid write' "$output_dir/valgrind.log"
}

run_definite_leak_test() {
  local output_dir="$output_root/definite-leak"
  local status=0

  MEMORY_CHECK_OUTPUT_DIR="$output_dir" \
    "$wrapper" "$build_dir/memcheck-target" definite-leak || status=$?

  if [[ "$status" -ne 99 ]]; then
    echo "FAIL: definite leak returned $status instead of 99" >&2
    return 1
  fi

  grep -Eq 'definitely lost: 32 bytes in 1 blocks' \
    "$output_dir/valgrind.log"
}

run_abnormal_exit_test() {
  local output_dir="$output_root/abnormal-exit"
  local status=0

  MEMORY_CHECK_OUTPUT_DIR="$output_dir" \
    "$wrapper" "$build_dir/memcheck-target" abnormal-exit || status=$?

  if [[ "$status" -ne 42 ]]; then
    echo "FAIL: expected target exit 42, got $status" >&2
    return 1
  fi

  grep -q 'ERROR SUMMARY: 0 errors' "$output_dir/valgrind.log"
}

run_reachable_leak_test() {
  local output_dir="$output_root/reachable-leak"

  MEMORY_CHECK_OUTPUT_DIR="$output_dir" \
    "$wrapper" "$build_dir/memcheck-target" reachable-leak

  grep -Eq 'still reachable: 16 bytes in 1 blocks' \
    "$output_dir/valgrind.log"
}

run_unit_test_adapter_test() {
  local output_dir="$output_root/unit-test-adapter"
  local status=0

  MEMORY_CHECK_OUTPUT_DIR="$output_dir" \
    "$script_dir/run-unitTest.sh" \
    "$build_dir/memcheck-target" abnormal-exit || status=$?

  if [[ "$status" -ne 42 ]]; then
    echo "FAIL: run-unitTest.sh returned $status instead of 42" >&2
    return 1
  fi
}

run_success_test
echo "PASS: successful target and argv preservation"
run_invalid_read_test
echo "PASS: invalid read rejected"
run_invalid_write_test
echo "PASS: invalid write rejected"
run_definite_leak_test
echo "PASS: definite leak rejected"
run_abnormal_exit_test
echo "PASS: abnormal target exit propagated"
run_reachable_leak_test
echo "PASS: non-definite leak reported without failing"
run_unit_test_adapter_test
echo "PASS: unit-test adapter delegates to the wrapper"
