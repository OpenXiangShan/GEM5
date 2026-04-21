#!/usr/bin/env python3
import argparse
import sqlite3
from collections import Counter
from pathlib import Path


def parse_pc(text: str) -> int:
    text = text.strip().lower()
    if text.startswith("0x"):
        return int(text, 16)
    return int(text, 16)


def fmt_pc(value: int) -> str:
    return f"0x{value:x}"


def main() -> None:
    ap = argparse.ArgumentParser(description="Summarize prediction-time PHR contributor for a TAGEMISSTRACE context.")
    ap.add_argument("--bpdb", required=True)
    ap.add_argument("--branch-pc", required=True)
    ap.add_argument("--start-pc", required=True)
    ap.add_argument("--taken", type=int, required=True)
    ap.add_argument("--top", type=int, default=12)
    args = ap.parse_args()

    branch_pc = parse_pc(args.branch_pc)
    start_pc = parse_pc(args.start_pc)

    conn = sqlite3.connect(Path(args.bpdb))
    conn.row_factory = sqlite3.Row
    rows = list(
        conn.execute(
            """
            select predTaken, phistPC, phistTarget, phistTaken, count(*) as cnt
            from TAGEMISSTRACE
            where branchPC=? and startPC=? and actualTaken=?
            group by predTaken, phistPC, phistTarget, phistTaken
            order by cnt desc
            """,
            (branch_pc, start_pc, args.taken),
        )
    )
    if not rows:
        print("no rows matched")
        return

    total = sum(r["cnt"] for r in rows)
    cat = Counter()
    pred = Counter()
    for r in rows:
        cnt = r["cnt"]
        pred["predTaken=1" if r["predTaken"] else "predTaken=0"] += cnt
        if not r["phistTaken"]:
            cat["none"] += cnt
        elif r["phistPC"] == branch_pc:
            cat["self"] += cnt
        elif start_pc <= r["phistPC"] < start_pc + 0x20:
            cat["same_block_other"] += cnt
        else:
            cat["other_block"] += cnt

    print(
        f"[context] branch={fmt_pc(branch_pc)} start={fmt_pc(start_pc)} "
        f"actualTaken={args.taken} rows={total}"
    )
    print("[pred_taken_share]")
    for k, v in pred.items():
        print(f"{k}\trows={v}\tshare={v*100/total:.2f}%")

    print("[contributor_category]")
    for k, v in cat.items():
        print(f"{k}\trows={v}\tshare={v*100/total:.2f}%")

    print("[top_contributors]")
    for r in rows[: args.top]:
        print(
            f"predTaken={r['predTaken']}\t"
            f"phistPC={fmt_pc(r['phistPC'])}\t"
            f"phistTarget={fmt_pc(r['phistTarget'])}\t"
            f"phistTaken={r['phistTaken']}\t"
            f"rows={r['cnt']}\tshare={r['cnt']*100/total:.2f}%"
        )


if __name__ == "__main__":
    main()
