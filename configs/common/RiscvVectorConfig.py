# Copyright (c) 2026 OpenXiangShan
# SPDX-License-Identifier: BSD-3-Clause
#
# Shared RVV VLEN/ELEN ISA wiring for FS (xiangshan/kmhv3) and SE (se.py).
# gem5 forbids mutating VectorParamValue after construction, so RiscvISA must
# be built with the requested vlen/elen before createThreads().

from m5.objects import RiscvISA
from m5.util import fatal


def configure_riscv_vector_isa(args, cpus):
    """Attach RiscvISA(vlen/elen) to each CPU before createThreads.

    Why shared: both kmhv3 FS and SE now expose --rvv-vlen/--rvv-elen via
    Options.addCommonOptions; duplicating the attach logic in se.py would
    drift from the FS path that decode / difftest already rely on.
    """
    vlen = int(getattr(args, 'rvv_vlen', 128))
    elen = int(getattr(args, 'rvv_elen', 64))
    if vlen < elen:
        fatal(f"Invalid RVV config: VLEN ({vlen}) < ELEN ({elen})")
    if getattr(args, 'enable_difftest', False) and vlen != 128:
        fatal(
            f"--enable-difftest currently requires --rvv-vlen=128 "
            f"(stock NEMU ABI); got {vlen}"
        )
    for cpu in cpus:
        nthreads = int(cpu.numThreads) if hasattr(cpu, 'numThreads') else 1
        if getattr(args, 'smt', False):
            nthreads = max(nthreads, 2)
        cpu.isa = [
            RiscvISA(vlen=vlen, elen=elen) for _ in range(nthreads)
        ]
        print(f"Configured RiscvISA VLEN={vlen} ELEN={elen} "
              f"for {nthreads} thread(s) on {type(cpu)}")
