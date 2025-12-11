#!/usr/bin/env python3

"""Align BUILD and BIND trace events per sn/tracesn.

本脚本用于解析由 ``util/trace/extract_trace_events.py`` 生成的 events.txt，
对齐指令构建事件（BUILD）与 trace 元数据绑定事件（BIND），
生成一张按 sn 索引的表，便于分析 ChampSim trace 对应的 gem5
执行行为。

输入行格式（来自 extract_trace_events.py）：

    <tick>\t[<LABEL>] <原始 gem5 日志行>

其中我们关心的两类行：

  - BUILD:
      15922856871:\ system.cpu.fetch: [tid:0]\
        Instruction PC (0xb3f7a3ac=>0xb3f7a3b0).(0=>1) created [sn:145113397].

  - BIND:
      15922861533:\ system.cpu.fetch: [tid:0]\
        Bind trace metadata to [sn:145113477]->[tracesn:83372305]:\
          pc=0xb3fc9640, taken=0, traceInstrConsumed=84209692

输出：

  sn  build_tick  build_pc  bind_tick  trace_pc  taken  tracesn

若某个 sn 只在 BUILD/BIND 中出现，会在另一侧填 "-"。

用法示例：

  # 对某个 debug 目录下的 events.txt 对齐
  python3 util/trace/align_trace_bind_events.py \
      /path/to/debug/events.txt > /path/to/debug/bind_align.tsv
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from typing import Dict, Optional


@dataclass
class BuildInfo:
    tick: int
    pc: str  # hex string, e.g. 0x804983e8


@dataclass
class BindInfo:
    tick: int
    trace_pc: str  # hex string
    taken: int
    tracesn: int


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(
        description="Align BUILD and BIND events per sn from extract_trace_events output.",
    )
    ap.add_argument("events", help="Path to events.txt (output of extract_trace_events.py)")
    return ap.parse_args()


def main() -> int:
    args = parse_args()

    # 行头模式: <tick>\t[<LABEL>] <raw>
    pat_line = re.compile(r"^(\d+)\s+\[([A-Z_]+)\]\s+(.*)$")

    # BUILD 行模式: Instruction PC (0xPC=>0xPC2)... [sn:NNN]
    pat_build = re.compile(
        r"Instruction PC \((0x[0-9a-fA-F]+)=>(0x[0-9a-fA-F]+)\).*?\[sn:(\d+)\]"
    )

    # BIND 行模式: Bind trace metadata to [sn:...]->[tracesn:...]: pc=0x..., taken=0/1
    pat_bind = re.compile(
        r"Bind trace metadata to \[sn:(\d+)\]->\[tracesn:(\d+)\]: "
        r"pc=(0x[0-9a-fA-F]+), taken=([01])"
    )

    build_by_sn: Dict[int, BuildInfo] = {}
    bind_by_sn: Dict[int, BindInfo] = {}

    with open(args.events, "r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue
            m = pat_line.match(line)
            if not m:
                continue
            tick = int(m.group(1))
            label = m.group(2)
            raw = m.group(3)

            if label == "BUILD":
                mb = pat_build.search(raw)
                if not mb:
                    continue
                pc = mb.group(1)  # 使用 before-PC 作为该指令 PC
                sn = int(mb.group(3))
                # 若同一 sn 出现多次，保留 tick 最小的一条（首次 build）
                info = build_by_sn.get(sn)
                if info is None or tick < info.tick:
                    build_by_sn[sn] = BuildInfo(tick=tick, pc=pc)

            elif label == "BIND":
                mb = pat_bind.search(raw)
                if not mb:
                    continue
                sn = int(mb.group(1))
                tracesn = int(mb.group(2))
                trace_pc = mb.group(3)
                taken = int(mb.group(4))
                info = bind_by_sn.get(sn)
                # 若多次绑定，同样保留 tick 最小的一次
                if info is None or tick < info.tick:
                    bind_by_sn[sn] = BindInfo(
                        tick=tick,
                        trace_pc=trace_pc,
                        taken=taken,
                        tracesn=tracesn,
                    )

    # 输出表头
    print("sn\tbuild_tick\tbuild_pc\tbind_tick\ttrace_pc\ttaken\ttracesn")

    all_sns = sorted(set(build_by_sn.keys()) | set(bind_by_sn.keys()))
    for sn in all_sns:
        b: Optional[BuildInfo] = build_by_sn.get(sn)
        t: Optional[BindInfo] = bind_by_sn.get(sn)

        build_tick = str(b.tick) if b is not None else "-"
        build_pc = b.pc if b is not None else "-"
        bind_tick = str(t.tick) if t is not None else "-"
        trace_pc = t.trace_pc if t is not None else "-"
        taken = str(t.taken) if t is not None else "-"
        tracesn = str(t.tracesn) if t is not None else "-"

        print("\t".join([
            str(sn), build_tick, build_pc, bind_tick, trace_pc, taken, tracesn,
        ]))

    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
