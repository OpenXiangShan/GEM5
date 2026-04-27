#!/usr/bin/env python3
import argparse
import math
import sqlite3
from collections import Counter
from pathlib import Path


def parse_pc(text: str) -> int:
    text = text.strip().lower()
    if text.startswith("0x"):
        return int(text, 16)
    return int(text, 16)


def entropy(counter: Counter) -> float:
    total = sum(counter.values())
    if total == 0:
        return 0.0
    return -sum((c / total) * math.log2(c / total) for c in counter.values())


def top_share(counter: Counter, topn: int) -> float:
    total = sum(counter.values())
    if total == 0:
        return 0.0
    return sum(c for _, c in counter.most_common(topn)) / total


def suffix_bucket(bitstr: str, width: int) -> str:
    if width <= 0:
        return ""
    if len(bitstr) <= width:
        return bitstr
    return bitstr[-width:]


def main() -> None:
    parser = argparse.ArgumentParser(description="Analyze low-bit PHR buckets from TAGEMISSTRACE.")
    parser.add_argument("--bpdb", required=True)
    parser.add_argument("--branch-pc", required=True)
    parser.add_argument("--start-pc", required=True)
    parser.add_argument("--taken", type=int, required=True)
    parser.add_argument("--width", type=int, default=32, help="Use low WIDTH bits from phistory text")
    parser.add_argument("--alloc-only", action="store_true")
    args = parser.parse_args()

    branch_pc = parse_pc(args.branch_pc)
    start_pc = parse_pc(args.start_pc)

    conn = sqlite3.connect(Path(args.bpdb))
    where = "branchPC=? and startPC=? and actualTaken=? and mainFound!=0"
    if args.alloc_only:
        where += " and allocSuccess!=0 and allocTable>=2"
    rows = conn.execute(
        f"select phistory, mainTable, mainIndex, mainTag from TAGEMISSTRACE where {where}",
        (branch_pc, start_pc, args.taken),
    ).fetchall()

    phr_ctr = Counter()
    key_ctr = Counter()
    for phistory, table, index, tag in rows:
        bucket = suffix_bucket(phistory or "", args.width)
        phr_ctr[bucket] += 1
        key_ctr[(bucket, table, index, tag)] += 1

    total = sum(phr_ctr.values())
    print(
        f"[summary] rows={total} width={args.width} alloc_only={args.alloc_only} "
        f"uniq_phr={len(phr_ctr)} top1={top_share(phr_ctr,1)*100:.2f}% "
        f"top3={top_share(phr_ctr,3)*100:.2f}% top5={top_share(phr_ctr,5)*100:.2f}% "
        f"entropy={entropy(phr_ctr):.2f}"
    )
    print("[top phr buckets]")
    for bucket, count in phr_ctr.most_common(12):
        display = bucket if len(bucket) <= 64 else ("..." + bucket[-64:])
        print(f"{display}\t{count}\t{count*100/total:.2f}%")
    print("[top phr+key buckets]")
    for (bucket, table, index, tag), count in key_ctr.most_common(12):
        display = bucket if len(bucket) <= 48 else ("..." + bucket[-48:])
        print(f"{display}\tt{table}\t{index}\t{tag}\t{count}\t{count*100/total:.2f}%")


if __name__ == "__main__":
    main()
