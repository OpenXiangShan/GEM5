#!/usr/bin/env python3
"""Micro synthetic trace corpus generator (L4).

Generates tiny, human-readable ChampSim-format trace files that replay the
trigger sequences of the historical trace-mode bugs. Each corpus trace is
<100 records so a full run finishes in seconds; the driver script asserts
exit 0 + EOF marker + committed==instrRead reconciliation.

ChampSimInstr binary layout (must mirror ChampSimTraceReader.hh exactly):
    uint64 ip
    uint8  is_branch
    uint8  branch_taken
    uint8  destination_registers[2]
    uint8  source_registers[4]
    uint64 destination_memory[2]
    uint64 source_memory[4]

Records are written little-endian, packed (no padding; the C++ struct has
no padding by construction: 8 + 1 + 1 + 2 + 4 + 16 + 32 = 64 bytes).

Usage:
    python3 gen_micro_corpus.py --out-dir <dir> [--seed 42]
"""

import argparse
import os
import struct

# (ip, is_branch, branch_taken, dst_regs, src_regs, dst_mem, src_mem)
RECORD_FMT = "<QBB2B4B2Q4Q"
RECORD_SIZE = struct.calcsize(RECORD_FMT)
assert RECORD_SIZE == 64, f"layout drift: {RECORD_SIZE} != 64"

REG_IP = 26
REG_FLAGS = 25


def rec(ip, is_branch=0, branch_taken=0, dst_regs=(), src_regs=(),
        dst_mem=(), src_mem=()):
    dst_regs = list(dst_regs) + [0] * (2 - len(dst_regs))
    src_regs = list(src_regs) + [0] * (4 - len(src_regs))
    dst_mem = list(dst_mem) + [0] * (2 - len(dst_mem))
    src_mem = list(src_mem) + [0] * (4 - len(src_mem))
    return (ip, is_branch, branch_taken, dst_regs, src_regs, dst_mem, src_mem)


def cond_branch(ip, taken):
    """Conditional branch (mirrors markAsCondBranch in the reader tests:
    writes IP, reads IP and FLAGS)."""
    return rec(ip, 1, 1 if taken else 0,
               dst_regs=[REG_IP], src_regs=[REG_IP, REG_FLAGS])


def write_trace(path, records):
    with open(path, "wb") as f:
        for r in records:
            ip, is_branch, branch_taken, dst_regs, src_regs, dst_mem, src_mem = r
            f.write(struct.pack(
                RECORD_FMT, ip, is_branch, branch_taken,
                *dst_regs, *src_regs, *dst_mem, *src_mem))
    return path


def linear(n, base=0x10000, step=0x40):
    """Straight-line code: no branches, sequential PCs."""
    return [rec(base + i * step) for i in range(n)]


def build_corpora(out_dir, seed):
    os.makedirs(out_dir, exist_ok=True)
    corpora = {}

    # 1. straight-line-64: plain sequential execution to EOF.
    corpora["straight-line-64"] = linear(64)

    # 2. cond-taken-loop: taken conditional branches + fallthrough mix.
    #    Guards branch-target-from-next-PC (e0a8ea668e) end to end.
    recs = []
    for i in range(24):
        ip = 0x20000 + i * 0x80
        recs.append(cond_branch(ip, taken=(i % 2 == 0)))
        recs.append(rec(ip + 0x40))
    corpora["cond-taken-loop"] = recs

    # 3. non-branch-discontinuity: non-branch record whose successor PC is
    #    not fallthrough (ctrl-flow change on a non-control instruction).
    #    Guards the ctrl-flow trap routing chain (86adc8434e...efed8e63b6).
    corpora["non-branch-discontinuity"] = [
        rec(0x30000),
        rec(0x38000),   # jump over 0x40/0x80-sized gaps: not fallthrough
        rec(0x30040),
        rec(0x30080),
        rec(0x40000),   # second discontinuity later in the trace
        rec(0x40040),
    ] + linear(32, base=0x40080)

    # 4. dense-2b-spacing: PCs 2 bytes apart (compressed-inst spacing).
    #    Guards the address-mapping alignment domain (a7da33710e).
    corpora["dense-2b-spacing"] = [rec(0x50000 + 2 * i) for i in range(48)]

    # 5. branch-at-eof: taken branch as the very last record (no lookahead
    #    target available). Guards EOF boundary behavior (cfe18c2483 /
    #    e0a8ea668e no-lookahead EOF contract).
    corpora["branch-at-eof"] = linear(48, base=0x60000) + [
        cond_branch(0x6C000, taken=True)
    ]

    # 6. mem-ops-mixed: loads/stores with memory operands mixed into
    #    straight-line code (exercise mem-op metadata + counting).
    recs = []
    for i in range(32):
        ip = 0x70000 + i * 0x40
        if i % 3 == 0:
            recs.append(rec(ip, dst_mem=[0x80000 + i * 8]))
        elif i % 3 == 1:
            recs.append(rec(ip, src_mem=[0x90000 + i * 8]))
        else:
            recs.append(rec(ip))
    corpora["mem-ops-mixed"] = recs

    # 7. regdep-heavy: many register dependencies (CALL_IND-style chains
    #    use x5-dummy-source shapes; here plain GPR deps stress the
    #    dependency plumbing end to end).
    recs = []
    for i in range(32):
        ip = 0xA0000 + i * 0x40
        recs.append(rec(ip, dst_regs=[1 + (i % 30)],
                        src_regs=[1 + (i % 30), 2 + (i % 29), 3 + (i % 28)]))
    corpora["regdep-heavy"] = recs

    # 8. single-record: minimal valid trace (init size gate is
    #    sizeof(ChampSimInstr); one record exactly meets it... actually one
    #    record IS 64 bytes and the gate is >= one record).
    corpora["single-record"] = [rec(0xB0000)]

    # 9. fetch-stall-2000: KNOWN-FAILING on pr/trace-runtime-split.
    #    2000 sequential records at 0x40 stride wedge the frontend after
    #    repeated wrong-path cycles (40k-cycle CommitStuck panic); 400
    #    records at the same stride run clean, and the baseline binary
    #    reproduces the stall. ROOT CAUSE (diagnosed 2026-10-09): the 0x40
    #    stride falls outside TraceReader::isApproxFallthrough's pc+2/pc+4
    #    tolerance, so EVERY record is marked ctrlFlowChange and enters the
    #    non-branch-trap wrong-path cycle (enter -> supply 2B NOPs ->
    #    squash -> exit at correct PC); after a few hundred consecutive
    #    cycles the backend-redirect-pending state wedges and fetch skips
    #    FTQ heads forever. Real traces keep consecutive instructions
    #    within the fallthrough tolerance (see corpus #10), so this is
    #    unreachable for realistic workloads — the corpus pins the wedge
    #    mode for a future fix (defensive cycle bound or redirect-flag
    #    root cause).
    corpora["fetch-stall-2000"] = linear(2000, base=0xC0000, step=0x40)

    # 10. straight-line-4b-2000: the correctly-strided long counterpart of
    #     #9. A 4-byte stride stays inside isApproxFallthrough's tolerance,
    #     so no per-record wrong-path cycle occurs and the full 2000-record
    #     trace must run to completion. Guards the realistic long-trace
    #     path that #9's artificial stride bypasses.
    corpora["straight-line-4b-2000"] = linear(2000, base=0xD0000, step=4)

    paths = {}
    for name, records in sorted(corpora.items()):
        p = write_trace(os.path.join(out_dir, f"{name}.champsimtrace"),
                        records)
        paths[name] = p
    return paths


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--seed", type=int, default=42,
                    help="kept for interface symmetry; corpora are "
                         "deterministic")
    args = ap.parse_args()

    paths = build_corpora(args.out_dir, args.seed)
    for name, path in paths.items():
        size = os.path.getsize(path)
        print(f"{name:28s} {size // RECORD_SIZE:4d} records  {path}")


if __name__ == "__main__":
    main()
