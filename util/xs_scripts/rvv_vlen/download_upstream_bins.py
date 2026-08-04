#!/usr/bin/env python3
# Copyright (c) 2026 OpenXiangShan
# SPDX-License-Identifier: BSD-3-Clause
"""
Download upstream gem5 RVV intrinsic test binaries via obtain_resource.

Must be executed by an upstream (stdlib-capable) gem5 binary, e.g.:

  GEM5_RESOURCE_DIR=./util/xs_scripts/rvv_vlen/gem5-resource-cache \\
    /path/to/upstream/gem5.opt \\
    util/xs_scripts/rvv_vlen/download_upstream_bins.py \\
    --out util/xs_scripts/rvv_vlen/bins

XS-GEM5 itself does not ship obtain_resource for these SE ELFs.
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys

RESOURCES = [
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


def _resource_path(res) -> str:
    if hasattr(res, "get_local_path"):
        path = res.get_local_path()
        if path:
            return str(path)
    for attr in ("path", "local_path", "_local_path"):
        if hasattr(res, attr):
            val = getattr(res, attr)
            if val:
                return str(val)
    if hasattr(res, "get_executable"):
        return str(res.get_executable())
    return str(res)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--out",
        default=os.path.join(os.path.dirname(__file__), "bins"),
        help="Directory to copy rvv-* binaries into",
    )
    args, _unknown = parser.parse_known_args(argv)

    from gem5.resources.resource import obtain_resource

    os.makedirs(args.out, exist_ok=True)
    failed = []
    for name in RESOURCES:
        print(f"obtaining {name}...", flush=True)
        try:
            res = obtain_resource(name)
            path = _resource_path(res)
            print(f"  path={path}", flush=True)
            if not os.path.isfile(path):
                raise FileNotFoundError(path)
            dest = os.path.join(args.out, name)
            shutil.copy2(path, dest)
            os.chmod(dest, 0o755)
            print(f"  copied -> {dest}", flush=True)
        except Exception as exc:  # noqa: BLE001 — report and continue
            print(f"  FAIL {name}: {exc}", flush=True)
            failed.append(name)

    if failed:
        print(f"FAILED ({len(failed)}): {', '.join(failed)}", flush=True)
        return 1
    print(f"OK: {len(RESOURCES)} binaries in {args.out}", flush=True)
    return 0


# gem5 config entry
if __name__ in ("__main__", "__m5_main__"):
    sys.exit(main())
