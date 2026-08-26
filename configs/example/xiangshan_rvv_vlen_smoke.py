# Copyright (c) 2024 Barcelona Supercomputing Center
# Copyright (c) 2026 OpenXiangShan
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Adapted from upstream gem5:
#   configs/example/gem5_library/riscv-rvv-example.py
#
# Upstream runs SE + obtain_resource(). XS-GEM5 is FS/XiangShan-checkpoint
# oriented, so this script only validates that RiscvISA(vlen/elen) can be
# constructed with the supported VLEN set and prints the configured values.
# It exits before simulation if --dry-run is set (default).

"""
Validate XS-GEM5 configurable VLEN wiring (no workload required).

Usage
-----
  ./build/RISCV/gem5.opt configs/example/xiangshan_rvv_vlen_smoke.py \\
      --rvv-vlen=256 --rvv-elen=64

  # Or exercise the Python SimObject layer only (if PYTHONPATH includes build):
  python3 configs/example/xiangshan_rvv_vlen_smoke.py --standalone \\
      --rvv-vlen=512
"""

import argparse
import sys


SUPPORTED_VLEN = (128, 256, 512)
SUPPORTED_ELEN = (8, 16, 32, 64)


def validate_params(vlen: int, elen: int) -> None:
    if vlen not in SUPPORTED_VLEN:
        raise SystemExit(
            f"Unsupported VLEN {vlen}; XS-GEM5 supports {SUPPORTED_VLEN}"
        )
    if elen not in SUPPORTED_ELEN:
        raise SystemExit(
            f"Unsupported ELEN {elen}; XS-GEM5 supports {SUPPORTED_ELEN}"
        )
    if vlen < elen:
        raise SystemExit(f"Invalid config: VLEN ({vlen}) < ELEN ({elen})")
    if vlen & (vlen - 1):
        raise SystemExit(f"VLEN must be a power of 2: {vlen}")
    if elen & (elen - 1):
        raise SystemExit(f"ELEN must be a power of 2: {elen}")


def standalone_check(vlen: int, elen: int) -> int:
    """Pure-Python contract check (mirrors RiscvISA.py validators)."""
    validate_params(vlen, elen)
    print(f"xiangshan_rvv_vlen_smoke: OK vlen={vlen} elen={elen} "
          f"vlenb={vlen // 8}")
    # Expected VLMAX samples used by tests/test_rvv_vlen.py
    samples = [
        (0, 0, vlen // 8),          # SEW=8, LMUL=1
        (3, 0, vlen // 64),         # SEW=64, LMUL=1
    ]
    for vsew, vlmul, expected in samples:
        sew = 8 << vsew
        vlmax = (vlen // sew) * (1 << vlmul)
        assert vlmax == expected, (vlen, vsew, vlmul, vlmax, expected)
        print(f"  VLMAX(sew={sew},lmul=1) = {vlmax}")
    return 0


def gem5_check(vlen: int, elen: int) -> int:
    """Construct RiscvISA inside gem5 and read back parameters."""
    import m5
    from m5.objects import RiscvISA, System, SrcClockDomain, VoltageDomain

    validate_params(vlen, elen)

    system = System()
    system.clk_domain = SrcClockDomain(
        clock="1GHz", voltage_domain=VoltageDomain()
    )
    system.mem_mode = "timing"
    system.mem_ranges = []

    isa = RiscvISA(vlen=vlen, elen=elen)
    print(
        f"xiangshan_rvv_vlen_smoke(gem5): constructed RiscvISA "
        f"vlen={int(isa.vlen)} elen={int(isa.elen)}"
    )
    assert int(isa.vlen) == vlen
    assert int(isa.elen) == elen

    # Do not start a full simulation; parameter construction is the smoke.
    print("xiangshan_rvv_vlen_smoke(gem5): PASS (dry-run, no workload)")
    return 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--rvv-vlen", type=int, default=128)
    parser.add_argument("--rvv-elen", type=int, default=64)
    parser.add_argument(
        "--standalone",
        action="store_true",
        help="Run pure-Python checks without importing m5",
    )
    args, _unknown = parser.parse_known_args(argv)

    if args.standalone or "m5" not in sys.modules:
        # When launched via gem5.opt, m5 is already present; prefer gem5 path.
        try:
            import m5  # noqa: F401
            in_gem5 = True
        except ImportError:
            in_gem5 = False
        if args.standalone or not in_gem5:
            return standalone_check(args.rvv_vlen, args.rvv_elen)
        return gem5_check(args.rvv_vlen, args.rvv_elen)

    return gem5_check(args.rvv_vlen, args.rvv_elen)


# gem5 execs configs with __name__ == "__m5_main__" (see m5/main.py).
# Keep importable for util/xs_scripts/rvv_vlen/test_rvv_vlen_config.py.
if __name__ in ("__main__", "__m5_main__"):
    sys.exit(main())
