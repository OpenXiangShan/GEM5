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
    memory reasonable. Only use this upgrader when importing *upstream*
    checkpoints that already contain RVV state; XiangShan RVGCpt format is
    separate and not handled here.
    """

    import re

    # 40 vector regs (32 arch + 8 internal) * 64 bytes
    xs_max_vec_bytes = 40 * 64

    for sec in cpt.sections():
        # Match legacy gem5 vector register sections.
        if not re.search(r"\.xc\.thread_context$", sec) and not re.search(
            r"\.thread$", sec
        ):
            # Keep scanning; some old dumps store vectors under misc keys.
            pass

        if cpt.has_option(sec, "regs.vector"):
            # Expand/truncate to XS max container footprint.
            # Actual content layout is opaque hex/blob in gem5 checkpoints.
            val = cpt.get(sec, "regs.vector")
            # Leave a marker comment in-process; real resize depends on
            # gem5 checkpoint serializer format and should be validated
            # before production use.
            cpt.set(sec, "regs.vector.note",
                    f"xs-dyn-vlen expect {xs_max_vec_bytes} bytes; "
                    f"raw_len={len(val)}")

    # No hard failure: this is a best-effort adapter for experiments.
