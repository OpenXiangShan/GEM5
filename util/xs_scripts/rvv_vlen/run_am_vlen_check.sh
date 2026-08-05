#!/usr/bin/env bash
# Copyright (c) 2026 OpenXiangShan
# SPDX-License-Identifier: BSD-3-Clause
#
# Build the AM rvv-vlen-check image and run it on XS-GEM5 for VLEN 128/256/512.
#
# Env:
#   AM_HOME   path to nexus-am (required for build)
#   GEM5      path to gem5.opt (default: ./build/RISCV/gem5.opt)
#   LINUX_GNU_TOOLCHAIN=1  use riscv64-linux-gnu-* (Ubuntu packages)
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$ROOT"

APP_DIR="$ROOT/util/xs_scripts/rvv_vlen/am/rvv-vlen-check"
GEM5="${GEM5:-$ROOT/build/RISCV/gem5.opt}"
BIN="$APP_DIR/build/rvv-vlen-check-riscv64-xs.bin"

if [[ ! -f "$BIN" ]]; then
  if [[ -z "${AM_HOME:-}" ]]; then
    echo "AM_HOME not set and $BIN missing; cannot build." >&2
    exit 1
  fi
  echo "Building AM image with AM_HOME=$AM_HOME ..."
  make -C "$APP_DIR" ARCH=riscv64-xs LINUX_GNU_TOOLCHAIN="${LINUX_GNU_TOOLCHAIN:-1}" -j"$(nproc)"
fi

test -x "$GEM5"
test -f "$BIN"

for v in 128 256 512; do
  out="$ROOT/util/xs_scripts/rvv_vlen/m5out-am-vlen-$v"
  rm -rf "$out"
  mkdir -p "$out"
  echo "===== kmhv3 --rvv-vlen=$v + rvv-vlen-check ====="
  "$GEM5" -d "$out" configs/example/kmhv3.py \
    --raw-cpt --generic-rv-cpt="$BIN" \
    --rvv-vlen="$v" --disable-difftest \
    --maxinsts="${MAXINSTS:-200000}" \
    > "$out/run.log" 2>&1
  grep -E "^vlen=$v$" "$out/config.ini"
  expect_vlenb=$((v / 8))
  expect_e64=$((v / 64))
  grep -E "rvv-vlen-check: vlenb=${expect_vlenb} vlen=${v} vlmax_e8_m1=${expect_vlenb} vlmax_e64_m1=${expect_e64}" \
    "$out/run.log"
  # Functional mem path (vlseg) must also pass; CSR-only checks are insufficient.
  grep -F "rvv-vlen-check: vlseg PASS" "$out/run.log"
  grep -F "rvv-vlen-check: PASS" "$out/run.log"
  echo "PASS_vlen_$v"
done

echo "AM_VLEN_CHECK_ALL_PASS"
