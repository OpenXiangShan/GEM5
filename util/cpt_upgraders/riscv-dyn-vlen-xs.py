# Copyright (c) 2024 Arm Limited
# Copyright (c) 2023 Barcelona Supercomputing Center (BSC)
# Copyright (c) 2026 OpenXiangShan
# All rights reserved
#
# Adapted from upstream gem5 util/cpt_upgraders/riscv-dyn-vlen.py
# for XS-GEM5's MaxVecLenInBytes = 64 (512-bit) container, not upstream's 8192.

def upgrader(cpt):
    """
    Resize serialized vector-register blobs to XS-GEM5 MaxVecLenInBytes.

    Upstream gem5 uses MaxVecLenInBytes = 8192 (65536-bit MaxVLEN).
    XS-GEM5 uses MaxVecLenInBytes = 64 (512-bit MaxVLEN) to keep O3 PRF
    memory reasonable.

    Checkpoint format stores regs.vector as whitespace-separated bytes.
    We expand/truncate to 40 regs * 64 bytes = 2560 bytes (dummy fill 0),
    matching the upstream upgrader's "always MaxVecLen container" policy.
    """

    import re

    # 40 vector regs (32 arch + 8 internal) * 64 bytes
    xs_max_vec_bytes = 40 * 64

    for sec in cpt.sections():
        res = re.search(r"(.*processor.*\.core.*)\.xc.*", sec)
        if not res:
            # Also accept bare ".thread_context" / ".thread" sections used by
            # some older dumps.
            if not re.search(r"\.xc\.thread_context$", sec) and not re.search(
                r"\.thread$", sec
            ):
                continue

        if not cpt.has_option(sec, "regs.vector"):
            continue

        mr = cpt.get(sec, "regs.vector").split()
        if len(mr) == xs_max_vec_bytes:
            continue

        # Why rewrite: VecRegContainer is always MaxVecLenInBytes wide.
        # Truncate oversized upstream blobs; zero-pad undersized ones.
        if len(mr) > xs_max_vec_bytes:
            mr = mr[:xs_max_vec_bytes]
        else:
            mr = mr + ["0"] * (xs_max_vec_bytes - len(mr))
        cpt.set(sec, "regs.vector", " ".join(mr))
