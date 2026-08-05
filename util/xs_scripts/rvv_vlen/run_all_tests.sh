#!/usr/bin/env bash
# Copyright (c) 2026 OpenXiangShan
# SPDX-License-Identifier: BSD-3-Clause
#
# Run the full XS-GEM5 dynamic-VLEN test suite that does not require wrapping
# upstream SE ELFs into XiangShan FS/AM checkpoints.
#
# Prerequisites:
#   - Container with GCC>=11 and a built build/RISCV/gem5.opt (e.g. xs-gem5-build)
#   - Optional: upstream gem5 + xs-env image for SE matrix / binary download
#
# Usage (inside xs-env / xs-gem5-build):
#   ./util/xs_scripts/rvv_vlen/run_all_tests.sh
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$ROOT"

GEM5="${GEM5:-./build/RISCV/gem5.opt}"
GTEST="${GTEST:-./build/RISCV/arch/riscv/vlen.test.opt}"

echo "===== 1) Python VLMAX / config / matrix / negative proofs ====="
python3 util/xs_scripts/rvv_vlen/test_rvv_vlen.py
python3 util/xs_scripts/rvv_vlen/test_rvv_vlen_config.py
python3 util/xs_scripts/rvv_vlen/test_elem_gen_idx_negative.py
python3 util/xs_scripts/rvv_vlen/test_matrix.py \
  ${BIN_DIR:+--bin-dir "$BIN_DIR"}
python3 configs/example/xiangshan_rvv_vlen_smoke.py --standalone --rvv-vlen=256

echo "===== 2) gem5 RiscvISA smoke (128/256/512) ====="
test -x "$GEM5"
for v in 128 256 512; do
  "$GEM5" configs/example/xiangshan_rvv_vlen_smoke.py --rvv-vlen="$v" --rvv-elen=64
done

echo "===== 3) GTest vlen.test ====="
if [[ ! -x "$GTEST" ]]; then
  echo "Building $GTEST ..."
  scons -j"$(nproc)" --ignore-style "$GTEST"
fi
"$GTEST"

echo "===== 4) kmhv3 short sim matrix (needs CPT) ====="
if [[ -n "${CPT:-}" && -f "$CPT" ]]; then
  for v in 128 256 512; do
    out="m5out-vlen-smoke-$v"
    rm -rf "$out"
    "$GEM5" -d "$out" configs/example/kmhv3.py \
      --generic-rv-cpt="$CPT" --maxinsts="${MAXINSTS:-50000}" \
      --rvv-vlen="$v" --disable-difftest
    grep -E "^vlen=$v$" "$out/config.ini"
    echo "PASS kmhv3 vlen=$v"
  done
else
  echo "SKIP kmhv3 (set CPT=/path/to/checkpoint.gz to enable)"
fi

echo "===== 5) AM rvv-vlen-check (functional VLEN on XS) ====="
if [[ -x util/xs_scripts/rvv_vlen/run_am_vlen_check.sh ]]; then
  if [[ -n "${AM_HOME:-}" ]] || [[ -f util/xs_scripts/rvv_vlen/am/rvv-vlen-check/build/rvv-vlen-check-riscv64-xs.bin ]]; then
    util/xs_scripts/rvv_vlen/run_am_vlen_check.sh
  else
    echo "SKIP AM check (set AM_HOME or prebuild rvv-vlen-check .bin)"
  fi
fi

echo "ALL_XS_VLEN_TESTS_OK"
