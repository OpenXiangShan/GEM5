#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

# Example: ./run-unitTest.sh ./build/RISCV/unittests.debug --gtest_filter=Foo.Bar
exec "$script_dir/run-with-valgrind.sh" "$@"
