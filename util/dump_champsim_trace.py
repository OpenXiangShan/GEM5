#!/usr/bin/env python3
"""
Dump ChampSim binary trace instructions.

This script parses ChampSim's binary instruction trace format (as read by
src/cpu/o3/trace/ChampSimTraceReader.*) and prints selected fields per record.

Record layout (little-endian, 64 bytes total):
  struct input_instr {
      uint64_t ip;                   // instruction pointer (PC)
      uint8_t  is_branch;            // 1 if branch
      uint8_t  branch_taken;         // 1 if taken
      uint8_t  destination_registers[2];
      uint8_t  source_registers[4];
      uint64_t destination_memory[2];
      uint64_t source_memory[4];
  };

Usage examples:
  - Dump first 10 instructions (auto-detect .gz):
      python3 util/dump_champsim_trace.py --trace path/to/trace.gz --limit 10

  - Dump表格模式（默认），包含内存地址：
      python3 util/dump_champsim_trace.py -t trace.bin -n 20 --show-mem

  - 使用 JSON 输出：
      python3 util/dump_champsim_trace.py -t trace.bin -n 20 --show-mem --json

  - Apply simple address mapping (optional):
      python3 util/dump_champsim_trace.py -t trace.gz --map-mode linear \
        --addr-base 0x80000000 --addr-size 0x40000000 --page-align
"""

import argparse
import gzip
import io
import json
import os
import struct
import subprocess
import sys
from typing import Iterator, Tuple

try:
    import lzma  # type: ignore
    _HAS_LZMA = True
except Exception:
    lzma = None  # type: ignore
    _HAS_LZMA = False


# Little-endian struct with exact layout (no padding needed given field order)
REC_STRUCT = struct.Struct("<Q B B 2B 4B 2Q 4Q")

# ChampSim special registers (from inc/trace_instruction.h)
REG_STACK_POINTER = 6
REG_FLAGS = 25
REG_INSTRUCTION_POINTER = 26


class _XZPipeReader:
    """Context-managed reader for .xz via external `xz -dc` (fallback).

    Provides a file-like object supporting read(), and ensures proper
    process cleanup when leaving the context manager.
    """

    def __init__(self, path: str):
        # Use `--` to stop option parsing in case path looks like an option
        self._p = subprocess.Popen([
            "xz", "-dc", "--", path
        ], stdout=subprocess.PIPE)
        if self._p.stdout is None:
            raise OSError("failed to open xz pipe: no stdout")
        self._r = self._p.stdout

    def read(self, n: int) -> bytes:
        return self._r.read(n)

    # Fallback skip for streams without seek
    def _consume(self, n: int) -> None:
        to_read = n
        chunk = 1 << 20
        while to_read > 0:
            data = self._r.read(min(chunk, to_read))
            if not data:
                break
            to_read -= len(data)

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        try:
            try:
                self._r.close()
            finally:
                # Terminate if still running to avoid zombies
                if self._p.poll() is None:
                    self._p.terminate()
                # Wait to reap process
                self._p.wait(timeout=5)
        except Exception:
            pass


def open_trace(path: str):
    if path.endswith(".gz"):
        return gzip.open(path, "rb")  # type: ignore[return-value]
    if path.endswith(".xz"):
        if _HAS_LZMA:
            return lzma.open(path, "rb")  # type: ignore[return-value]
        # Fallback to external xz
        return _XZPipeReader(path)
    return open(path, "rb")  # type: ignore[return-value]


def map_addr_raw(addr: int) -> int:
    return addr


def map_addr_linear(addr: int, base: int, size: int, page_align: bool) -> int:
    if size == 0:
        return addr
    mapped = base + (addr % size)
    if page_align:
        mapped &= ~0x3  # 4-byte align
    return mapped


def map_addr_hash(addr: int, base: int, size: int, page_align: bool) -> int:
    if size == 0:
        return addr
    h = (addr ^ (addr >> 16)) & (size - 1)
    mapped = (base + h)
    if page_align:
        mapped &= ~0x3
    return mapped


def iter_records(f, skip: int = 0) -> Iterator[Tuple[int, ...]]:
    if skip:
        offset = skip * REC_STRUCT.size
        # Try to seek if supported, otherwise consume
        try:
            f.seek(offset, io.SEEK_CUR)
        except Exception:
            # Stream without seeking (e.g., xz pipe)
            to_read = offset
            chunk = 1 << 20
            while to_read > 0:
                data = f.read(min(chunk, to_read))
                if not data:
                    return
                to_read -= len(data)
    while True:
        buf = f.read(REC_STRUCT.size)
        if not buf or len(buf) < REC_STRUCT.size:
            return
        yield REC_STRUCT.unpack(buf)


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(description="Dump ChampSim trace instructions")
    ap.add_argument("--trace", "-t", required=True, help="Path to ChampSim trace (.bin, .gz or .xz)")
    ap.add_argument("--limit", "-n", type=int, default=0, help="Max records to print (0=all)")
    ap.add_argument("--skip", type=int, default=0, help="Skip initial N records")
    ap.add_argument("--json", action="store_true", help="Output JSON lines instead of text")
    ap.add_argument("--show-mem", action="store_true", help="Include memory addresses in output")
    ap.add_argument("--arch", choices=["riscv", "generic"], default="riscv",
                    help="Heuristics for branch type classification (default: riscv)")
    ap.add_argument("--ra-reg", type=int, default=1,
                    help="Return-address register id for call/ret detection (default: 1 for RISC-V ra)")
    ap.add_argument("--reg-abi", choices=["arm64", "raw"], default="arm64",
                    help="Register naming in output (arm64 ABI names or raw numbers; default: arm64)")
    ap.add_argument("--map-mode", choices=["raw", "linear", "hash"], default="raw",
                    help="Address mapping mode for PC/memory")
    ap.add_argument("--addr-base", type=lambda s: int(s, 0), default=0,
                    help="Base for mapped addresses (default: 0)")
    ap.add_argument("--addr-size", type=lambda s: int(s, 0), default=0,
                    help="Size window for mapped addresses (power-of-two recommended)")
    ap.add_argument("--page-align", action="store_true", help="4-byte align mapped addresses")
    return ap.parse_args()


def get_mapper(mode: str):
    if mode == "raw":
        return lambda a, b, c, d: map_addr_raw(a)
    if mode == "linear":
        return map_addr_linear
    if mode == "hash":
        return map_addr_hash
    raise ValueError(mode)


def main() -> int:
    args = parse_args()

    mapper = get_mapper(args.map_mode)

    try:
        f = open_trace(args.trace)
    except OSError as e:
        print(f"error: cannot open {args.trace}: {e}", file=sys.stderr)
        return 2

    printed = 0
    total = 0
    with f:
        for rec in iter_records(f, skip=args.skip):
            total += 1
            (
                ip,
                is_branch,
                branch_taken,
                dst_reg0,
                dst_reg1,
                src_reg0,
                src_reg1,
                src_reg2,
                src_reg3,
                dst_mem0,
                dst_mem1,
                src_mem0,
                src_mem1,
                src_mem2,
                src_mem3,
            ) = rec

            mip = mapper(ip, args.addr_base, args.addr_size, args.page_align)
            dmem = [dst_mem0, dst_mem1]
            smem = [src_mem0, src_mem1, src_mem2, src_mem3]
            dmem_m = [mapper(x, args.addr_base, args.addr_size, args.page_align) for x in dmem]
            smem_m = [mapper(x, args.addr_base, args.addr_size, args.page_align) for x in smem]

            # Utility: compact hex without leading zeros
            def fmt_hex(val: int) -> str:
                return f"0x{val:x}"

            # Heuristic instruction classification covering branch and non-branch
            def classify_type() -> str:
                dst_regs = [dst_reg0, dst_reg1]
                src_regs = [src_reg0, src_reg1, src_reg2, src_reg3]
                if is_branch:
                    writes_sp = REG_STACK_POINTER in dst_regs
                    writes_ip = REG_INSTRUCTION_POINTER in dst_regs
                    reads_sp = REG_STACK_POINTER in src_regs
                    reads_flags = REG_FLAGS in src_regs
                    reads_ip = REG_INSTRUCTION_POINTER in src_regs
                    reads_other = any(
                        r not in (0, REG_STACK_POINTER, REG_FLAGS, REG_INSTRUCTION_POINTER) for r in src_regs
                    )
                    # Mirror ChampSim's classification logic
                    if (not reads_sp) and (not reads_flags) and writes_ip and (not reads_other):
                        return "jump"  # direct jump
                    elif (not reads_sp) and (not reads_flags) and writes_ip and reads_other:
                        return "indirect"
                    elif (
                        (not reads_sp)
                        and reads_ip
                        and (not writes_sp)
                        and writes_ip
                        and reads_flags
                        and (not reads_other)
                    ):
                        return "cond"
                    elif reads_sp and reads_ip and writes_sp and writes_ip and (not reads_flags) and (not reads_other):
                        return "call"  # direct call
                    elif reads_sp and reads_ip and writes_sp and writes_ip and (not reads_flags) and reads_other:
                        return "call"  # indirect call
                    elif reads_sp and (not reads_ip) and writes_sp and writes_ip:
                        return "return"
                    elif writes_ip:
                        return "cond"  # other branch types
                    else:
                        return "cond"
                # Non-branch: use memory fields
                any_smem = any(x != 0 for x in smem)
                any_dmem = any(x != 0 for x in dmem)
                if any_dmem:
                    return "store"
                if any_smem:
                    return "load"
                return "alu"

            itype = classify_type()

            # Register naming
            def reg_name(r: int) -> str:
                if args.reg_abi == "raw":
                    return str(r)
                # ARM64 ABI-style naming (heuristic)
                if r in (REG_STACK_POINTER, 31):
                    return "sp"
                if r == 30:
                    return "lr"
                if r == REG_INSTRUCTION_POINTER:
                    return "pc"
                if 0 <= r <= 29:
                    return f"x{r}"
                if r == REG_FLAGS:
                    return "pstate"
                return f"r{r}"

            if args.json:
                obj = {
                    "ip": mip,
                    "is_branch": bool(is_branch),
                    "branch_taken": bool(branch_taken),
                    "type": itype,
                    "dst_regs": [dst_reg0, dst_reg1],
                    "src_regs": [src_reg0, src_reg1, src_reg2, src_reg3],
                    "dst_regs_names": [reg_name(dst_reg0), reg_name(dst_reg1)],
                    "src_regs_names": [reg_name(src_reg0), reg_name(src_reg1), reg_name(src_reg2), reg_name(src_reg3)],
                }
                if args.show_mem:
                    obj["dst_mem"] = dmem_m
                    obj["src_mem"] = smem_m
                print(json.dumps(obj))
            else:
                # Table mode with aligned columns
                if printed == 0:
                    headers = [
                        ("idx", 8),
                        ("pc", 18),
                        ("br", 3),
                        ("type", 9),
                        ("taken", 6),
                        ("dst_regs", 20),
                        ("src_regs", 30),
                    ]
                    if args.show_mem:
                        # Narrower columns for memory lists (compact hex, non-zero only)
                        headers += [("dst_mem", 28), ("src_mem", 48)]
                    def fmt_hdr(cols):
                        return " ".join(name.ljust(w) for name, w in cols)
                    print(fmt_hdr(headers))
                    print("-" * (sum(w for _, w in headers) + len(headers) - 1))

                idx_str = f"{args.skip + total - 1:>6d}"
                # Compact hex for PC (no leading zeros)
                pc_str = fmt_hex(mip)
                br_str = f"{int(is_branch)}"
                type_str = itype
                taken_str = f"{int(bool(branch_taken))}" if is_branch else ""
                dst_regs_str = "[" + ",".join(
                    reg_name(r) for r in [dst_reg0, dst_reg1] if r != 0
                ) + "]"
                src_regs_str = "[" + ",".join(
                    reg_name(r)
                    for r in [src_reg0, src_reg1, src_reg2, src_reg3]
                    if r != 0
                ) + "]"
                row = [
                    (idx_str, 8),
                    (pc_str, 18),
                    (br_str, 3),
                    (type_str, 9),
                    (taken_str, 6),
                    (dst_regs_str, 20),
                    (src_regs_str, 30),
                ]
                if args.show_mem:
                    nz_dmem = [x for x in dmem_m if x != 0]
                    nz_smem = [x for x in smem_m if x != 0]
                    dst_mem_str = "[" + ",".join(fmt_hex(x) for x in nz_dmem) + "]" if nz_dmem else ""
                    src_mem_str = "[" + ",".join(fmt_hex(x) for x in nz_smem) + "]" if nz_smem else ""
                    row += [(dst_mem_str, 28), (src_mem_str, 48)]

                def fmt_row(cols):
                    return " ".join(str(val)[:w].ljust(w) for val, w in cols)
                print(fmt_row(row))

            printed += 1
            if args.limit and printed >= args.limit:
                break

    return 0


if __name__ == "__main__":
    sys.exit(main())
