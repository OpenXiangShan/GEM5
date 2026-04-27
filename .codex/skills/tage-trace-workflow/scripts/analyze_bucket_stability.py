#!/usr/bin/env python3
import argparse
import math
import sqlite3
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List, Sequence, Tuple


def parse_pc(text: str) -> int:
    text = text.strip().lower()
    if text.startswith("0x"):
        return int(text, 16)
    return int(text, 16)


def fmt_pc(value: int) -> str:
    return f"0x{value:x}"


def entropy(counter: Counter) -> float:
    total = sum(counter.values())
    if total == 0:
        return 0.0
    ent = 0.0
    for count in counter.values():
        p = count / total
        ent -= p * math.log2(p)
    return ent


def top_share(counter: Counter, topn: int) -> float:
    total = sum(counter.values())
    if total == 0:
        return 0.0
    return sum(count for _, count in counter.most_common(topn)) / total


def stability(counter: Counter) -> float:
    total = sum(counter.values())
    if total <= 1:
        return 0.0
    same = sum(count - 1 for _, count in counter.items() if count > 1)
    return same / (total - 1)


@dataclass(frozen=True)
class Gem5Row:
    tick: int
    idx_fh: int
    main_table: int
    main_index: int
    main_tag: int


@dataclass(frozen=True)
class RtlRow:
    stamp: int
    provider_table: int
    provider_set: int
    provider_way: int
    alloc_table: int
    alloc_set: int
    alloc_way: int


def load_gem5_rows(
    bpdb: Path, branch_pc: int, start_pc: int, taken: int
) -> List[Gem5Row]:
    conn = sqlite3.connect(bpdb)
    rows = conn.execute(
        """
        select
          TICK,
          indexFoldedHist,
          mainTable,
          mainIndex,
          mainTag
        from TAGEMISSTRACE
        where branchPC = ? and startPC = ? and actualTaken = ? and mainFound != 0
        order by TICK, ID
        """,
        (branch_pc, start_pc, taken),
    ).fetchall()
    return [Gem5Row(*row) for row in rows]


def load_rtl_rows(
    db: Path, branch_pc: int, start_pc: int, taken: int
) -> List[RtlRow]:
    conn = sqlite3.connect(db)
    union = " union all ".join(
        [
            f"""select STAMP, PROVIDERTABLEIDX, PROVIDERSETIDX, PROVIDERWAYIDX,
                       ALLOCATETABLEIDX, ALLOCATESETIDX, ALLOCATEWAYIDX
                from CondTrace_{i}
                where CFIPC = ? and STARTPC_ADDR = ? and ACTUALTAKEN = ? and HASPROVIDER != 0"""
            for i in range(8)
        ]
    )
    params: List[int] = []
    for _ in range(8):
        params.extend([branch_pc, start_pc, taken])
    rows = conn.execute(f"{union} order by STAMP, PROVIDERTABLEIDX, PROVIDERSETIDX, PROVIDERWAYIDX", params).fetchall()
    return [RtlRow(*row) for row in rows]


def transition_counter(seq: Sequence[Tuple[int, ...]]) -> Counter:
    ctr: Counter = Counter()
    for prev, cur in zip(seq, seq[1:]):
        ctr[(prev, cur)] += 1
    return ctr


def print_counter_summary(label: str, counter: Counter, topn: int = 5) -> None:
    total = sum(counter.values())
    uniq = len(counter)
    print(
        f"  {label}: uniq={uniq} top1={top_share(counter,1)*100:.2f}% "
        f"top3={top_share(counter,3)*100:.2f}% top5={top_share(counter,5)*100:.2f}% "
        f"entropy={entropy(counter):.2f}"
    )
    for item, count in counter.most_common(topn):
        print(f"    {item} -> {count} ({count*100/total:.2f}%)")


def print_transition_summary(label: str, counter: Counter, topn: int = 8) -> None:
    total = sum(counter.values())
    uniq = len(counter)
    print(
        f"  {label}: uniq={uniq} top1={top_share(counter,1)*100:.2f}% "
        f"top3={top_share(counter,3)*100:.2f}% entropy={entropy(counter):.2f}"
    )
    for item, count in counter.most_common(topn):
        print(f"    {item} -> {count} ({count*100/total:.2f}%)")


def analyze_gem5(rows: List[Gem5Row]) -> None:
    idx_ctr = Counter(row.idx_fh for row in rows)
    key_ctr = Counter((row.main_table, row.main_index, row.main_tag) for row in rows)
    tbl_ctr = Counter(row.main_table for row in rows)
    idx_trans = transition_counter([row.idx_fh for row in rows])
    key_trans = transition_counter([(row.main_table, row.main_index, row.main_tag) for row in rows])
    print(f"  rows={len(rows)} idx_stability={stability(idx_ctr):.2f} key_stability={stability(key_ctr):.2f}")
    print_counter_summary("indexFoldedHist", idx_ctr)
    print_counter_summary("mainKey(table,index,tag)", key_ctr)
    print_counter_summary("mainTable", tbl_ctr, topn=8)
    print_transition_summary("indexFoldedHist transitions", idx_trans)
    print_transition_summary("mainKey transitions", key_trans)


def analyze_rtl(rows: List[RtlRow]) -> None:
    set_ctr = Counter((row.provider_table, row.provider_set) for row in rows)
    slot_ctr = Counter((row.provider_table, row.provider_set, row.provider_way) for row in rows)
    tbl_ctr = Counter(row.provider_table for row in rows)
    set_trans = transition_counter([(row.provider_table, row.provider_set) for row in rows])
    slot_trans = transition_counter(
        [(row.provider_table, row.provider_set, row.provider_way) for row in rows]
    )
    print(f"  rows={len(rows)} set_stability={stability(set_ctr):.2f} slot_stability={stability(slot_ctr):.2f}")
    print_counter_summary("providerSet(table,set)", set_ctr)
    print_counter_summary("providerSlot(table,set,way)", slot_ctr)
    print_counter_summary("providerTable", tbl_ctr, topn=8)
    print_transition_summary("providerSet transitions", set_trans)
    print_transition_summary("providerSlot transitions", slot_trans)


def main() -> None:
    parser = argparse.ArgumentParser(description="Analyze gem5/RTL bucket stability for a branch context.")
    parser.add_argument("--gem5-bpdb", required=True)
    parser.add_argument("--rtl-db", required=True)
    parser.add_argument("--branch-pc", required=True)
    parser.add_argument("--gem5-start-pc", required=True)
    parser.add_argument("--rtl-start-pc", required=True)
    parser.add_argument("--taken", type=int, required=True)
    args = parser.parse_args()

    branch_pc = parse_pc(args.branch_pc)
    gem5_start_pc = parse_pc(args.gem5_start_pc)
    rtl_start_pc = parse_pc(args.rtl_start_pc)

    print(
        f"[context] branch={fmt_pc(branch_pc)} gem5_start={fmt_pc(gem5_start_pc)} "
        f"rtl_start={fmt_pc(rtl_start_pc)} taken={args.taken}"
    )

    gem5_rows = load_gem5_rows(Path(args.gem5_bpdb), branch_pc, gem5_start_pc, args.taken)
    rtl_rows = load_rtl_rows(Path(args.rtl_db), branch_pc, rtl_start_pc, args.taken)

    print("[gem5]")
    analyze_gem5(gem5_rows)
    print("[rtl]")
    analyze_rtl(rtl_rows)


if __name__ == "__main__":
    main()
