#!/usr/bin/env python3
# Copyright (c) 2024 Barcelona Supercomputing Center
# Copyright (c) 2026 OpenXiangShan
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
# 1. Redistributions of source code must retain the above copyright notice,
#    this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright notice,
#    this list of conditions and the following disclaimer in the documentation
#    and/or other materials provided with the distribution.
# 3. Neither the name of the copyright holder nor the names of its contributors
#    may be used to endorse or promote products derived from this software
#    without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
# LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
# CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
# ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.

"""
XS-GEM5 adaptation of upstream gem5's RVV VLEN test matrix.

Upstream source:
  - configs/example/gem5_library/riscv-rvv-example.py
  - tests/gem5/se_mode/rvv_intrinsic_tests/test.py

Upstream runs these binaries in SE mode across VLEN=128..16384.
XS-GEM5 does not support SE / obtain_resource workloads, so this file:
  1. Records the upstream resource list and expected pass regex.
  2. Restricts VLEN to XS-supported values {128, 256, 512}.
  3. Provides a dry-run matrix printer and an optional hook for local binaries.

Usage
-----
  # Print the adapted matrix (no gem5 required)
  python3 util/xs_scripts/rvv_vlen/test_matrix.py

  # When local AM/Linux binaries are available, point --bin-dir at them and
  # optionally --gem5 / --config to emit runnable command lines.
  python3 util/xs_scripts/rvv_vlen/test_matrix.py \
      --bin-dir /path/to/rvv-bins \
      --gem5 ./build/RISCV/gem5.opt \
      --config configs/example/kmhv3.py
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path


# Keep in sync with upstream tests/gem5/se_mode/rvv_intrinsic_tests/test.py
UPSTREAM_RESOURCES = [
    "rvv-branch",
    "rvv-index",
    "rvv-matmul",
    "rvv-memcpy",
    "rvv-reduce",
    "rvv-saxpy",
    "rvv-sgemm",
    "rvv-strcmp",
    "rvv-strcpy",
    "rvv-strlen",
    "rvv-strlen-fault",
    "rvv-strncpy",
]

# XS-GEM5 MaxVecLenInBits = 512; Kunminghu default = 128.
XS_VLENS = [128, 256, 512]

PASS_REGEX = re.compile(r"^.*{resource}-\d+\.\d+\.\d+: pass$")


def expected_pass_pattern(resource: str) -> str:
    return PASS_REGEX.pattern.format(resource=re.escape(resource))


def build_matrix():
    rows = []
    for resource in UPSTREAM_RESOURCES:
        for vlen in XS_VLENS:
            rows.append(
                {
                    "name": f"test-riscv-{resource}-vlen_{vlen}-xs",
                    "resource": resource,
                    "vlen": vlen,
                    "pass_regex": expected_pass_pattern(resource),
                }
            )
    return rows


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="XS-GEM5 port of upstream RVV VLEN intrinsic test matrix"
    )
    parser.add_argument(
        "--bin-dir",
        type=Path,
        default=None,
        help="Optional directory containing locally built/copied rvv-* binaries",
    )
    parser.add_argument(
        "--gem5",
        type=Path,
        default=None,
        help="Optional path to gem5.opt for emitting run commands",
    )
    parser.add_argument(
        "--config",
        type=Path,
        default=Path("configs/example/kmhv3.py"),
        help="XS config script used when emitting run commands",
    )
    parser.add_argument(
        "--emit-commands",
        action="store_true",
        help="Print suggested gem5 command lines for available local binaries",
    )
    args = parser.parse_args(argv)

    matrix = build_matrix()
    print(f"# XS-GEM5 RVV VLEN matrix: {len(matrix)} cases "
          f"({len(UPSTREAM_RESOURCES)} resources x {len(XS_VLENS)} VLENs)")
    print("# Adapted from upstream gem5 SE rvv_intrinsic_tests "
          "(VLEN capped to 128/256/512).")
    print()

    missing = []
    for row in matrix:
        bin_path = None
        if args.bin_dir is not None:
            candidate = args.bin_dir / row["resource"]
            if candidate.exists():
                bin_path = candidate
            else:
                missing.append(row["resource"])

        status = "ready" if bin_path else "pending-binary"
        print(
            f"{row['name']}: resource={row['resource']} "
            f"vlen={row['vlen']} status={status}"
        )

        if args.emit_commands and args.gem5 and bin_path:
            # XS FS configs take checkpoints/raw-cpt rather than SE ELFs.
            # Emit a placeholder that still wires --rvv-vlen for smoke.
            print(
                f"  # TODO: wrap {bin_path} into XiangShan FS/AM workload; "
                f"then run with matching VLEN"
            )
            print(
                f"  {args.gem5} {args.config} --rvv-vlen={row['vlen']} "
                f"# expect: {row['pass_regex']}"
            )

    unique_missing = sorted(set(missing))
    if args.bin_dir is not None and unique_missing:
        print()
        print(
            f"# Missing local binaries under {args.bin_dir}: "
            + ", ".join(unique_missing)
        )
        print(
            "# Obtain upstream resources via gem5 Resources "
            "(names above), or rebuild equivalent AM/Linux tests."
        )
        return 1

    print()
    print("# Dry-run OK: matrix generated successfully.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
