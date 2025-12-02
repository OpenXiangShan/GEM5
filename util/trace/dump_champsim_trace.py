#!/usr/bin/env python3
"""
Dump ChampSim or CBP2025 binary trace instructions.

This script parses ChampSim's binary trace format (as read by
src/cpu/o3/trace/ChampSimTraceReader.*) or CBP2025 trace format
(src/cpu/o3/trace/CBP2025TraceReader.*) and prints selected fields per record.

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

Usage examples (ChampSim):
  - Dump first 10 instructions (auto-detect .gz):
      python3 util/dump_champsim_trace.py --trace path/to/trace.gz --limit 10

  - Dump表格模式（默认），包含内存地址：
      python3 util/dump_champsim_trace.py -t trace.bin -n 20 --show-mem

  - 使用 JSON 输出：
      python3 util/dump_champsim_trace.py -t trace.bin -n 20 --show-mem --json

Usage examples (CBP2025):
  - Dump first 5 CBP instructions:
      python3 util/dump_champsim_trace.py --format cbp2025 -t compress_0_trace.gz -n 5

Address mapping (optional):
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
from typing import Dict, Iterator, Tuple

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


# ---- CBP2025 parsing ----
CBP_TYPE_NAMES = {
    0: "alu",
    1: "load",
    2: "store",
    3: "cond_br",
    4: "uncond_dir",
    5: "uncond_ind",
    6: "fp",
    7: "slow_alu",
    8: "undef",
    9: "call_dir",
    10: "call_ind",
    11: "ret",
}


def cbp_reg_is_int(reg: int) -> bool:
    # mirror cbp2025: 0-31 int, 31=SP, 64 flags, 65 zero; >=32 & <64 are SIMD
    return reg < 32 or reg == 64 or reg == 65


def _read_exact(f, n: int) -> bytes:
    buf = f.read(n)
    if not buf or len(buf) < n:
        return b""
    return buf


def _read_uint(f, fmt: str):
    size = struct.calcsize(fmt)
    data = _read_exact(f, size)
    if not data:
        return None
    return struct.unpack(fmt, data)[0]


def _read_cbp_record(f):
    pc = _read_uint(f, "<Q")
    if pc is None:
        return None
    typ = _read_uint(f, "<B")
    if typ is None:
        return None
    typ_u8 = int(typ)

    eff_addr = 0
    mem_size = 0
    base_upd = 0
    has_reg_offset = 0
    taken = False
    next_pc = pc + 4

    def is_load(t): return t == 1
    def is_store(t): return t == 2
    def is_mem(t): return is_load(t) or is_store(t)
    def is_branch(t): return t in (3, 4, 5, 9, 10, 11)
    def is_cond_branch(t): return t == 3

    if is_mem(typ_u8):
        val = _read_uint(f, "<Q")
        if val is None:
            return None
        eff_addr = val
        val = _read_uint(f, "<B")
        if val is None:
            return None
        mem_size = int(val)
        val = _read_uint(f, "<B")
        if val is None:
            return None
        base_upd = int(val)
        if is_store(typ_u8):
            val = _read_uint(f, "<B")
            if val is None:
                return None
            has_reg_offset = int(val)

    if is_branch(typ_u8):
        val = _read_uint(f, "<B")
        if val is None:
            return None
        taken = bool(val)
        if not is_cond_branch(typ_u8):
            taken = True
        if taken:
            val = _read_uint(f, "<Q")
            if val is None:
                return None
            next_pc = val

    num_in = _read_uint(f, "<B")
    if num_in is None:
        return None
    in_regs = []
    for _ in range(num_in):
        r = _read_uint(f, "<B")
        if r is None:
            return None
        in_regs.append(int(r))

    num_out = _read_uint(f, "<B")
    if num_out is None:
        return None
    out_regs = []
    for _ in range(num_out):
        r = _read_uint(f, "<B")
        if r is None:
            return None
        out_regs.append(int(r))

    # consume output values (int:8B, fp:16B)
    for r in out_regs:
        v = _read_uint(f, "<Q")
        if v is None:
            return None
        if not cbp_reg_is_int(r):
            v_hi = _read_uint(f, "<Q")
            if v_hi is None:
                return None

    return {
        "pc": pc,
        "next_pc": next_pc,
        "type": CBP_TYPE_NAMES.get(typ_u8, f"t{typ_u8}"),
        "type_id": typ_u8,
        "taken": taken,
        "is_branch": bool(is_branch(typ_u8)),
        "is_load": bool(is_load(typ_u8)),
        "is_store": bool(is_store(typ_u8)),
        "eff_addr": eff_addr,
        "mem_size": mem_size,
        "base_upd": base_upd,
        "has_reg_offset": has_reg_offset,
        "in_regs": in_regs,
        "out_regs": out_regs,
    }


def iter_records_cbp2025(f, skip: int = 0):
    # Consume skip instructions
    while skip > 0:
        rec = _read_cbp_record(f)
        if rec is None:
            return
        skip -= 1
    while True:
        rec = _read_cbp_record(f)
        if rec is None:
            return
        yield rec


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(description="Dump ChampSim / CBP2025 trace instructions")
    ap.add_argument("--trace", "-t", required=True, help="Path to trace (.bin, .gz or .xz)")
    ap.add_argument("--format", choices=["champsim", "cbp2025"], default="champsim",
                    help="Trace format (default: champsim)")
    ap.add_argument("--limit", "-n", type=int, default=0, help="Max records to print (0=all)")
    ap.add_argument("--skip", type=int, default=0, help="Skip initial N records")
    ap.add_argument("--start-index", type=int, default=None,
                    help="Start at record index (0-based, overrides --skip if set)")
    ap.add_argument("--count", type=int, default=None,
                    help="Number of records to print (overrides --limit if set)")
    ap.add_argument("--json", action="store_true", help="Output JSON lines instead of text")
    ap.add_argument("--show-mem", action="store_true", help="Include memory addresses in output")
    ap.add_argument("--arch", choices=["riscv", "generic"], default="riscv",
                    help="Heuristics for branch type classification (ChampSim only, default: riscv)")
    ap.add_argument("--ra-reg", type=int, default=1,
                    help="Return-address register id for call/ret detection (ChampSim only, default: 1 for RISC-V ra)")
    ap.add_argument("--reg-abi", choices=["arm64", "raw"], default="arm64",
                    help="Register naming in output (arm64 ABI names or raw numbers; default: arm64)")
    ap.add_argument("--map-mode", choices=["raw", "linear", "hash"], default="raw",
                    help="Address mapping mode for PC/memory")
    ap.add_argument("--addr-base", type=lambda s: int(s, 0), default=0,
                    help="Base for mapped addresses (default: 0)")
    ap.add_argument("--addr-size", type=lambda s: int(s, 0), default=0,
                    help="Size window for mapped addresses (power-of-two recommended)")
    ap.add_argument("--page-align", action="store_true", help="4-byte align mapped addresses")
    ap.add_argument("--dump-raw-struct", action="store_true",
                    help="Print raw record tuple fields for each entry")
    ap.add_argument("--analyze-len-reg", action="store_true",
                    help="Analyze length vs src/dst reg count (ChampSim/CBP); "
                         "reports anomalies and exits")
    return ap.parse_args()


def get_mapper(mode: str):
    if mode == "raw":
        return lambda a, b, c, d: map_addr_raw(a)
    if mode == "linear":
        return map_addr_linear
    if mode == "hash":
        return map_addr_hash
    raise ValueError(mode)


def classify_champsim_branch(dst_regs, src_regs, is_branch_flag, branch_taken_raw):
    """Mimic ChampSim's instruction.h branch classification for ChampSim traces."""
    REG_SP = 6
    REG_FL = 25
    REG_IP = 26
    REG_RA = 1

    dst_set = set(r for r in dst_regs if r != 0)
    src_set = set(r for r in src_regs if r != 0)

    writes_sp = REG_SP in dst_set
    writes_ip = REG_IP in dst_set
    reads_sp = REG_SP in src_set
    reads_flags = REG_FL in src_set
    reads_ip = REG_IP in src_set
    reads_ra = REG_RA in src_set
    reads_other = any(r not in (REG_SP, REG_FL, REG_IP) for r in src_set)

    # direct jump
    if (not reads_sp) and (not reads_flags) and writes_ip and (not reads_other):
        return "jump", True, True
    # indirect branch
    if (not reads_sp) and (not reads_flags) and writes_ip and reads_other:
        return "indirect", True, True
    # conditional branch
    if (not reads_sp) and reads_ip and (not writes_sp) and writes_ip and reads_flags and (not reads_other):
        return "cond", True, bool(branch_taken_raw)
    # direct call
    if reads_sp and reads_ip and writes_sp and writes_ip and (not reads_flags) and (not reads_other):
        return "call", True, True
    # indirect call
    if reads_sp and reads_ip and writes_sp and writes_ip and (not reads_flags) and reads_other:
        return "call_ind", True, True
    # return (ChampSim uses SP/IP, but we also accept RA-only pattern)
    if reads_sp and (not reads_ip) and writes_sp and writes_ip:
        return "return", True, True
    if (not reads_sp) and (not reads_flags) and writes_ip and reads_ra and (not reads_other):
        return "return", True, True
    # branch_other fallback
    if writes_ip:
        return "branch_other", True, bool(branch_taken_raw)
    return "alu", bool(is_branch_flag), False


def analyze_len_reg_champsim(f, skip: int, limit: int) -> int:
    """
    逐条扫描 ChampSim trace，检查：非 taken 分支/非分支指令的 npc-this_pc 作为长度，
    - 若长度不在 {2,4}，输出异常条目；
    - 若长度=2 且源寄存器数量>1，输出异常条目。
    """
    rec_iter = iter_records(f, skip=skip)
    idx = skip
    try:
        prev = next(rec_iter)
    except StopIteration:
        print("no records", file=sys.stderr)
        return 0

    seen_len = set()       # PCs already reported for delta not in {2,4}
    seen_len2 = set()      # PCs already reported for delta=2 & src>1
    total_checked = 0

    def to_int(val):
        if isinstance(val, int):
            return val
        try:
            return int(val)
        except Exception:
            if isinstance(val, (list, tuple)) and val:
                try:
                    return int(val[0])
                except Exception:
                    return 0
            return 0

    def print_header_if_needed(state):
        if state.get("printed_header"):
            return
        print("Anomaly: length not in {2,4} or len=2 with src_cnt>1 (unique per PC)")
        print(" idx      pc              next_pc         delta  type   taken  src_regs           dst_regs")
        print("-------------------------------------------------------------------------------------------")
        state["printed_header"] = True

    header_state = {"printed_header": False}

    while True:
        try:
            curr = next(rec_iter)
        except StopIteration:
            break
        idx += 1
        if limit and (idx - skip) > limit:
            break

        # prev fields
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
        ) = prev

        branch_type, is_branch_eff, branch_taken_eff = classify_champsim_branch(
            [dst_reg0, dst_reg1],
            [src_reg0, src_reg1, src_reg2, src_reg3],
            is_branch,
            branch_taken,
        )

        # 跳过 taken 分支
        if is_branch_eff and branch_taken_eff:
            prev = curr
            continue

        next_ip = curr[0]
        delta = next_ip - ip
        total_checked += 1

        src_regs = [r for r in (src_reg0, src_reg1, src_reg2, src_reg3) if r != 0]
        src_cnt = len(src_regs)

        if delta not in (2, 4):
            if ip not in seen_len:
                print_header_if_needed(header_state)
                msg = (
                    f" {idx-1:>6d} 0x{to_int(ip):016x} 0x{to_int(next_ip):016x} "
                    f"{to_int(delta):>6d} "
                    f"{'br' if is_branch_eff else 'nb':<5s} {str(branch_taken_eff):<5s} "
                    f"{str(src_regs):<18} {[r for r in (dst_reg0, dst_reg1) if r != 0]}"
                )
                print(msg)
                seen_len.add(ip)
        elif delta == 2 and src_cnt > 1:
            if ip not in seen_len2:
                print_header_if_needed(header_state)
                msg = (
                    f" {idx-1:>6d} 0x{to_int(ip):016x} 0x{to_int(next_ip):016x} "
                    f"{to_int(delta):>6d} "
                    f"{'nb':<5s} {str(branch_taken_eff):<5s} "
                    f"{str(src_regs):<18} {[r for r in (dst_reg0, dst_reg1) if r != 0]}"
                )
                print(msg)
                seen_len2.add(ip)

        prev = curr

    print(f"Checked {total_checked} records (skip={skip}, limit={'all' if not limit else limit}); "
          f"anomaly_len={len(seen_len)}, anomaly_len2_src={len(seen_len2)}")
    return 0


def analyze_len_reg_cbp(f, skip: int, limit: int) -> int:
    rec_iter = iter_records_cbp2025(f, skip=skip)
    prev = None
    idx = skip
    seen_len = set()
    seen_len2 = set()
    total_checked = 0

    def print_header_if_needed(state):
        if state.get("printed_header"):
            return
        print("Anomaly: length not in {2,4} or len=2 with src_cnt>1 (unique per PC)")
        print(" idx      pc              next_pc         delta  type   taken  src_regs           dst_regs")
        print("-------------------------------------------------------------------------------------------")
        state["printed_header"] = True

    header_state = {"printed_header": False}

    for rec in rec_iter:
        if prev is None:
            prev = rec
            idx += 1
            continue
        if limit and (idx - skip) >= limit:
            break

        # prev rec fields
        pc = prev["pc"]
        next_pc = prev["next_pc"]
        is_branch = prev["is_branch"]
        taken = prev["taken"]
        src_regs = [r for r in prev["in_regs"] if r != 0]
        dst_regs = [r for r in prev["out_regs"] if r != 0]

        # 跳过 taken 分支
        if is_branch and taken:
            prev = rec
            idx += 1
            continue

        # 对于 CBP trace，next_pc 已在记录中；若缺失则退化为 pc+4
        if next_pc is None:
            next_pc = pc + 4
        delta = next_pc - pc
        total_checked += 1
        src_cnt = len(src_regs)

        if delta not in (2, 4):
            if pc not in seen_len:
                print_header_if_needed(header_state)
                print(f" {idx:>6d} 0x{pc:016x} 0x{next_pc:016x} {delta:>6d} "
                      f"{'br' if is_branch else 'nb':<5s} {str(taken):<5s} {str(src_regs):<18} {dst_regs}")
                seen_len.add(pc)
        elif delta == 2 and src_cnt > 1:
            if pc not in seen_len2:
                print_header_if_needed(header_state)
                print(f" {idx:>6d} 0x{pc:016x} 0x{next_pc:016x} {delta:>6d} "
                      f"{'nb':<5s} {str(taken):<5s} {str(src_regs):<18} {dst_regs}")
                seen_len2.add(pc)

        prev = rec
        idx += 1

    print(f"Checked {total_checked} records (skip={skip}, limit={'all' if not limit else limit}); "
          f"anomaly_len={len(seen_len)}, anomaly_len2_src={len(seen_len2)}")
    return 0
def main() -> int:
    args = parse_args()

    mapper = get_mapper(args.map_mode)

    # 计算有效的 skip / limit：如果指定了 --start-index/--count，则优先使用
    effective_skip = args.skip
    if args.start_index is not None and args.start_index > 0:
        effective_skip = args.start_index

    effective_limit = args.limit
    if args.count is not None and args.count > 0:
        effective_limit = args.count

    try:
        f = open_trace(args.trace)
    except OSError as e:
        print(f"error: cannot open {args.trace}: {e}", file=sys.stderr)
        return 2

    printed = 0
    total = 0
    with f:
        if args.analyze_len_reg:
            if args.format == "champsim":
                return analyze_len_reg_champsim(f, skip=effective_skip, limit=effective_limit)
            elif args.format == "cbp2025":
                return analyze_len_reg_cbp(f, skip=effective_skip, limit=effective_limit)
            else:
                print("--analyze-len-reg unsupported format", file=sys.stderr)
                return 1

        rec_iter = iter_records(f, skip=effective_skip) if args.format == "champsim" \
            else iter_records_cbp2025(f, skip=effective_skip)

        for rec in rec_iter:
            total += 1
            rec_dict: Dict[str, object] = {}
            if args.format == "champsim":
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
                mem_size = 0
                branch_taken_raw = branch_taken
                # Reclassify branch type per ChampSim logic
                branch_type, is_branch_eff, branch_taken_eff = classify_champsim_branch(
                    [dst_reg0, dst_reg1],
                    [src_reg0, src_reg1, src_reg2, src_reg3],
                    is_branch,
                    branch_taken_raw,
                )
                is_branch = 1 if is_branch_eff else 0
            else:
                rec_dict = rec  # type: ignore[assignment]
                ip = rec_dict["pc"]
                is_branch = 1 if rec_dict["is_branch"] else 0
                branch_taken_raw = 1 if rec_dict["taken"] else 0
                branch_taken_eff = branch_taken_raw
                branch_type = rec_dict["type"]  # type: ignore[index]
                dst_regs = rec_dict["out_regs"]
                src_regs = rec_dict["in_regs"]
                dst_reg0, dst_reg1 = (dst_regs + [0, 0])[:2]
                src_reg0, src_reg1, src_reg2, src_reg3 = (src_regs + [0, 0, 0, 0])[:4]
                dst_mem0 = rec_dict["eff_addr"] if rec_dict["is_store"] else 0
                dst_mem1 = 0
                smem_val = rec_dict["eff_addr"] if rec_dict["is_load"] else 0
                src_mem0, src_mem1, src_mem2, src_mem3 = (smem_val, 0, 0, 0)
                mem_size = rec_dict["mem_size"]

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
                if args.format == "cbp2025":
                    return rec_dict["type"]  # type: ignore[index]
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
                    "branch_type": branch_type,
                    "branch_taken_raw": bool(branch_taken_raw),
                    "branch_taken": bool(branch_taken_eff),
                    "type": itype,
                    "dst_regs": [dst_reg0, dst_reg1],
                    "src_regs": [src_reg0, src_reg1, src_reg2, src_reg3],
                    "dst_regs_names": [reg_name(dst_reg0), reg_name(dst_reg1)],
                    "src_regs_names": [reg_name(src_reg0), reg_name(src_reg1), reg_name(src_reg2), reg_name(src_reg3)],
                }
                if args.format == "cbp2025":
                    obj["mem_size"] = mem_size
                if args.show_mem:
                    obj["dst_mem"] = dmem_m
                    obj["src_mem"] = smem_m
                if args.dump_raw_struct:
                    if args.format == "cbp2025":
                        obj["raw"] = rec_dict
                    else:
                        obj["raw_struct"] = {
                            "ip": ip,
                            "is_branch": is_branch,
                            "branch_taken_raw": branch_taken_raw,
                            "branch_taken": branch_taken_eff,
                            "dst_reg0": dst_reg0,
                            "dst_reg1": dst_reg1,
                            "src_reg0": src_reg0,
                            "src_reg1": src_reg1,
                            "src_reg2": src_reg2,
                            "src_reg3": src_reg3,
                            "dst_mem0": dst_mem0,
                            "dst_mem1": dst_mem1,
                            "src_mem0": src_mem0,
                            "src_mem1": src_mem1,
                            "src_mem2": src_mem2,
                            "src_mem3": src_mem3,
                        }
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
                        headers += [("dst_mem", 28), ("src_mem", 48)]
                    if args.format == "cbp2025":
                        headers += [("mem_sz", 8)]

                    def fmt_hdr(cols):
                        return " ".join(name.ljust(w) for name, w in cols)
                    print(fmt_hdr(headers))
                    print("-" * (sum(w for _, w in headers) + len(headers) - 1))

                idx_str = f"{effective_skip + total - 1:>6d}"
                # Compact hex for PC (no leading zeros)
                pc_str = fmt_hex(mip)
                br_str = f"{int(is_branch)}"
                type_str = itype
                taken_str = f"{int(bool(branch_taken_eff))}" if is_branch else ""
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
                if args.format == "cbp2025":
                    row.append((str(mem_size), 8))
                if args.dump_raw_struct:
                    if args.format == "cbp2025":
                        raw_str = str(rec_dict)
                    else:
                        raw_fields = {
                            "ip": ip,
                            "is_branch": is_branch,
                            "branch_type": branch_type,
                            "branch_taken_raw": branch_taken_raw,
                            "branch_taken": branch_taken_eff,
                            "dst_reg0": dst_reg0,
                            "dst_reg1": dst_reg1,
                            "src_reg0": src_reg0,
                            "src_reg1": src_reg1,
                            "src_reg2": src_reg2,
                            "src_reg3": src_reg3,
                            "dst_mem0": dst_mem0,
                            "dst_mem1": dst_mem1,
                            "src_mem0": src_mem0,
                            "src_mem1": src_mem1,
                            "src_mem2": src_mem2,
                            "src_mem3": src_mem3,
                        }
                        raw_str = str(raw_fields)
                    row.append((raw_str, -1))

                def fmt_row(cols):
                    out_parts = []
                    for val, w in cols:
                        sval = str(val)
                        if w is None or w <= 0:
                            out_parts.append(sval)
                        else:
                            out_parts.append(sval[:w].ljust(w))
                    return " ".join(out_parts)
                print(fmt_row(row))

            printed += 1
            if effective_limit and printed >= effective_limit:
                break

    return 0


if __name__ == "__main__":
    sys.exit(main())
