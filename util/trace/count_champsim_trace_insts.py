#!/usr/bin/env python3

"""Count total instructions in a trace file (ChampSim or CBP2025).

本脚本复用 ``util/dump_champsim_trace.py`` 中的解析逻辑，
统计给定 trace 文件中的记录条数（即指令数）。

支持的输入格式与 ``dump_champsim_trace.py`` 一致：
  - 原始二进制：  *.bin
  - gzip 压缩：   *.gz
  - xz 压缩：     *.xz

用法示例：

  # 打印单个 trace 的指令总数
  python3 util/count_champsim_trace_insts.py \
      --trace /path/to/trace.champsimtrace.xz \
      --format champsim

  # CBP2025
  python3 util/count_champsim_trace_insts.py \
      --trace /path/to/cbp_trace.gz \
      --format cbp2025

输出：
  仅打印一个整数（十进制），代表该 trace 内的指令总数，
  便于在 shell 中做进一步脚本处理。
"""

from __future__ import annotations

import argparse
import sys
from typing import Tuple

import dump_champsim_trace as dump


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(
        description="Count total instructions in a trace (ChampSim/CBP2025)",
    )
    ap.add_argument(
        "--trace",
        "-t",
        required=True,
        help="Path to trace (.bin/.gz/.xz)",
    )
    ap.add_argument(
        "--skip",
        type=int,
        default=0,
        help="Skip first N records before counting (default: 0)",
    )
    ap.add_argument(
        "--format",
        choices=["champsim", "cbp2025"],
        default="champsim",
        help="Trace format (default: champsim)",
    )
    return ap.parse_args()


def main() -> int:
    args = parse_args()

    # Each record corresponds to one instruction for both formats; CBP2025 does
    # not currently encode slice pieces, so we simply count records.
    iter_fn = (
        dump.iter_records_cbp2025
        if args.format == "cbp2025"
        else dump.iter_records
    )

    try:
        f = dump.open_trace(args.trace)
    except OSError as e:
        print(f"error: cannot open {args.trace}: {e}", file=sys.stderr)
        return 2

    total = 0
    with f:
        for _rec in iter_fn(f, skip=args.skip):
            total += 1

    # 只打印一个整数，方便被其它脚本消费
    print(total)
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
