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
    XS-GEM5 uses MaxVecLenInBytes = 64 (512-bit MaxVLEN).

    Checkpoint format stores regs.vector as whitespace-separated bytes for
    40 vector registers (32 arch + 8 internal). When converting an oversized
    upstream blob, each register must be resized independently: taking the
    first 40*64 bytes of a flat 40*8192 stream would keep only the first
    register's head plus garbage from later registers' leading bytes.
    """

    import re

    num_vec_regs = 40
    xs_bytes_per_reg = 64
    xs_max_vec_bytes = num_vec_regs * xs_bytes_per_reg

    def resize_regs_vector(mr, sec):
        """Return a 40*64-byte token list, resizing each register in place."""
        if len(mr) == xs_max_vec_bytes:
            return mr

        # Why reject: a length not divisible by 40 has no register boundaries.
        # Flat pad/truncate would silently corrupt restored vector state.
        if len(mr) % num_vec_regs != 0:
            raise ValueError(
                f"{sec}: regs.vector length {len(mr)} is not divisible by "
                f"{num_vec_regs}; refuse to upgrade an unsupported layout"
            )

        old_bpr = len(mr) // num_vec_regs
        out = []
        for r in range(num_vec_regs):
            chunk = mr[r * old_bpr:(r + 1) * old_bpr]
            if len(chunk) >= xs_bytes_per_reg:
                out.extend(chunk[:xs_bytes_per_reg])
            else:
                out.extend(chunk + ["0"] * (xs_bytes_per_reg - len(chunk)))
        return out

    def parent_isa_section(sec):
        # Prefer upstream-style "...processor...core....xc..." naming, then
        # XiangShan-style "system.cpu.xc.thread_context".
        m = re.search(r"(.*processor.*\.core.*)\.xc", sec)
        if m:
            return m.group(1) + ".isa"
        m = re.search(r"^(.*)\.xc(?:\.|$)", sec)
        if m:
            return m.group(1) + ".isa"
        return None

    for sec in cpt.sections():
        if not cpt.has_option(sec, "regs.vector"):
            continue

        # Only touch XC / thread contexts that look like CPU thread state.
        if not re.search(r"\.xc", sec) and not re.search(r"\.thread$", sec):
            continue

        # Require a verified RISC-V ISA sibling. Do not upgrade bare `.thread`
        # (or other) sections when isaName cannot be confirmed.
        isa_sec = parent_isa_section(sec)
        if not isa_sec or not cpt.has_section(isa_sec):
            continue
        if cpt.get(isa_sec, "isaName", fallback="") != "riscv":
            continue

        mr = cpt.get(sec, "regs.vector").split()
        new_mr = resize_regs_vector(mr, sec)
        if new_mr != mr:
            cpt.set(sec, "regs.vector", " ".join(new_mr))
