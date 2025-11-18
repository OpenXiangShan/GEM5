#!/usr/bin/env python3

"""Check ChampSim trace for non-branch control-flow changes.

给定一个 ChampSim 指令 trace，本脚本扫描所有指令对，
当「前一条指令是非分支指令」且「下一条指令的 PC 不是其
fallthrough（默认 +4 字节）」时，输出一行摘要：

    <idx> <prev_type> <prev_pc_hex> -> <next_type> <next_pc_hex>

其中 idx 是前一条指令在 trace 中的序号（从 0 开始，包含
--skip 的偏移），便于和其它工具对齐。

其中指令类型与 `util/dump_champsim_trace.py` 中的分类一致：
  - 非分支："alu" / "load" / "store"
  - 分支：  "jump" / "indirect" / "cond" / "call" / "return"

用法示例：

  python3 util/check_champsim_nonbranch_cf.py \
      --trace /path/to/trace.champsimtrace.xz \
      --fallthrough-bytes 4

可选参数：
  --limit/-n   最多输出多少条异常（0 表示全部）
  --skip       跳过前 N 条记录再开始检查
"""

from __future__ import annotations

import argparse
from typing import Iterable, Tuple

import dump_champsim_trace as dump


def classify_type(rec: Tuple[int, ...]) -> str:
    """Classify instruction type, mirroring dump_champsim_trace.

    输入为一条解包后的 ChampSim 记录：

      (ip, is_branch, branch_taken,
       dst_reg0, dst_reg1,
       src_reg0, src_reg1, src_reg2, src_reg3,
       dst_mem0, dst_mem1,
       src_mem0, src_mem1, src_mem2, src_mem3)

    这里只使用寄存器/branch 标记做一个粗略分类即可。
    """

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

    dmem = [dst_mem0, dst_mem1]
    smem = [src_mem0, src_mem1, src_mem2, src_mem3]
    dst_regs = [dst_reg0, dst_reg1]
    src_regs = [src_reg0, src_reg1, src_reg2, src_reg3]

    if is_branch:
        writes_sp = dump.REG_STACK_POINTER in dst_regs
        writes_ip = dump.REG_INSTRUCTION_POINTER in dst_regs
        reads_sp = dump.REG_STACK_POINTER in src_regs
        reads_flags = dump.REG_FLAGS in src_regs
        reads_ip = dump.REG_INSTRUCTION_POINTER in src_regs
        reads_other = any(
            r not in (0, dump.REG_STACK_POINTER, dump.REG_FLAGS, dump.REG_INSTRUCTION_POINTER)
            for r in src_regs
        )
        # 与 util/dump_champsim_trace.py 中逻辑保持一致
        if (not reads_sp) and (not reads_flags) and writes_ip and (not reads_other):
            return "jump"  # direct jump
        if (not reads_sp) and (not reads_flags) and writes_ip and reads_other:
            return "indirect"
        if (
            (not reads_sp)
            and reads_ip
            and (not writes_sp)
            and writes_ip
            and reads_flags
            and (not reads_other)
        ):
            return "cond"
        if reads_sp and reads_ip and writes_sp and writes_ip and (not reads_flags) and (not reads_other):
            return "call"  # direct call
        if reads_sp and reads_ip and writes_sp and writes_ip and (not reads_flags) and reads_other:
            return "call"  # indirect call
        if reads_sp and (not reads_ip) and writes_sp and writes_ip:
            return "return"
        if writes_ip:
            return "cond"  # other branch types
        return "cond"

    # 非分支：根据是否访问内存粗略区分 load/store/alu
    any_smem = any(x != 0 for x in smem)
    any_dmem = any(x != 0 for x in dmem)
    if any_dmem:
        return "store"
    if any_smem:
        return "load"
    return "alu"


def find_nonbranch_cf_events(
    recs: Iterable[Tuple[int, ...]],
    allowed_fallthrough_deltas: Iterable[int],
    start_index: int = 0,
) -> Iterable[Tuple[int, str, int, str, int]]:
    """遍历记录，找到"非分支导致控制流改变"的事件。

    返回迭代器，元素为：(idx, prev_type, prev_ip, next_type, next_ip)，
    其中 idx 为前一条指令在 trace 中的序号（从 start_index 开始计数）。
    """

    prev: Tuple[int, ...] | None = None
    prev_idx: int | None = None
    idx = start_index
    allowed_set = {d & ((1 << 64) - 1) for d in allowed_fallthrough_deltas if d > 0}

    for rec in recs:
        if prev is not None and prev_idx is not None:
            prev_ip = prev[0]
            prev_is_branch = prev[1]
            curr_ip = rec[0]

            if not prev_is_branch:
                delta = (curr_ip - prev_ip) & ((1 << 64) - 1)
                # 仅在“PC 差值不属于允许的 fallthrough 步长集合”时认为是
                # 控制流改变。这样可以排除 2/4 字节混布导致的正常顺序执行。
                if delta not in allowed_set:
                    prev_type = classify_type(prev)
                    next_type = classify_type(rec)
                    yield prev_idx, prev_type, prev_ip, next_type, curr_ip

        prev = rec
        prev_idx = idx
        idx += 1


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(
        description="Check ChampSim trace for non-branch control-flow changes",
    )
    ap.add_argument("--trace", "-t", required=True,
                    help="ChampSim trace path (.bin/.gz/.xz)")
    ap.add_argument("--fallthrough-bytes", "-b", type=int, default=4,
                    help="Fallthrough PC increment in bytes (default: 4)")
    ap.add_argument("--skip", type=int, default=0,
                    help="Skip first N records before checking")
    ap.add_argument("--limit", "-n", type=int, default=0,
                    help="Limit number of reported events (0 = no limit)")
    return ap.parse_args()


def main() -> int:
    args = parse_args()

    try:
        f = dump.open_trace(args.trace)
    except OSError as e:
        print(f"error: cannot open {args.trace}: {e}")
        return 2

    # 典型 RISC-V+C trace 中，连续指令的 PC 差值通常为 2 或 4 字节。
    # 默认允许 {fallthrough_bytes, 2, 4} 作为“正常顺序执行”的步长，
    # 从而排除 2/4 字节指令混布导致的大量伪异常。
    allowed_deltas = {args.fallthrough_bytes, 2, 4}

    count = 0
    with f:
        recs = dump.iter_records(f, skip=args.skip)
        for idx, prev_type, prev_ip, next_type, next_ip in find_nonbranch_cf_events(
            recs,
            allowed_fallthrough_deltas=allowed_deltas,
            start_index=args.skip,
        ):
            print(f"{idx} {prev_type} 0x{prev_ip:x} -> {next_type} 0x{next_ip:x}")
            count += 1
            if args.limit and count >= args.limit:
                break

    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
