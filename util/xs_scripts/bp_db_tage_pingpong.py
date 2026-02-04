#!/usr/bin/env python3
"""
Analyze TAGEMISSTRACE in bp.db for Exit-Slot TAGE ping-pong / multi-pattern blocks.

Typical usage:
  python3 util/xs_scripts/bp_db_tage_pingpong.py --db /tmp/debug/.../bp.db --top 20
  python3 util/xs_scripts/bp_db_tage_pingpong.py --db .../bp.db --startpc 0x80000160 --top 50
"""

from __future__ import annotations

import argparse
import collections
import sqlite3
import sys
from dataclasses import dataclass
from typing import Dict, Iterable, List, Optional, Set, Tuple


def parse_u64(x: str) -> int:
    x = x.strip().lower()
    if x.startswith("0x"):
        return int(x, 16)
    return int(x, 10)


def hex0(x: int) -> str:
    return "0x%x" % x


def get_cols(con: sqlite3.Connection, table: str) -> Set[str]:
    cur = con.cursor()
    cur.execute(f"pragma table_info({table});")
    return {r[1] for r in cur.fetchall()}


def require_table(con: sqlite3.Connection, table: str) -> None:
    cur = con.cursor()
    cur.execute(
        "select name from sqlite_master where type='table' and name=?;",
        (table,),
    )
    if cur.fetchone() is None:
        raise SystemExit(f"ERROR: table {table} not found in db")


@dataclass(frozen=True)
class EntryKey:
    main_table: int
    main_index: int
    way: int
    main_tag: int  # 0 if not present


@dataclass
class EntryAgg:
    n: int = 0
    real_encs: Set[int] = None  # type: ignore[assignment]
    payload_pairs: Set[Tuple[int, int]] = None  # type: ignore[assignment]
    pred_encs: Set[int] = None  # type: ignore[assignment]
    startpcs: Set[int] = None  # type: ignore[assignment]
    correct: int = 0
    sels: Set[int] = None  # type: ignore[assignment]

    def __post_init__(self) -> None:
        if self.real_encs is None:
            self.real_encs = set()
        if self.payload_pairs is None:
            self.payload_pairs = set()
        if self.pred_encs is None:
            self.pred_encs = set()
        if self.startpcs is None:
            self.startpcs = set()
        if self.sels is None:
            self.sels = set()


def iter_rows(
    con: sqlite3.Connection,
    cols: Set[str],
    startpc: Optional[int],
    limit: Optional[int],
) -> Iterable[sqlite3.Row]:
    con.row_factory = sqlite3.Row
    cur = con.cursor()

    want = [
        "TICK",
        "startPC",
        "branchPC",
        "actualTaken",
        "mainFound",
        "mainTable",
        "mainIndex",
        "wayIdx",
        # Optional new fields
        "mainTag",
        "mainPayload",
        "mainPayload1",
        "mainSel",
        "predEnc",
        "realEnc",
    ]

    select = [c for c in want if c in cols]
    if "TICK" not in select:
        # Old schema: no explicit tick column in the trace table, but Record adds it.
        # If missing, still proceed.
        pass

    q = "select %s from TAGEMISSTRACE" % (", ".join(select) if select else "*")
    args: List[object] = []
    if startpc is not None and "startPC" in cols:
        q += " where startPC = ?"
        args.append(startpc)
    if "TICK" in cols:
        q += " order by TICK asc"
    if limit is not None:
        q += " limit ?"
        args.append(limit)

    cur.execute(q, args)
    for row in cur.fetchall():
        yield row


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", required=True, help="path to bp.db")
    ap.add_argument("--startpc", default=None, help="filter by startPC (hex or dec)")
    ap.add_argument("--top", type=int, default=30, help="top N entries by entropy")
    ap.add_argument("--min-samples", type=int, default=50, help="min samples per entry")
    ap.add_argument("--limit", type=int, default=None, help="limit number of rows scanned")
    args = ap.parse_args()

    startpc = parse_u64(args.startpc) if args.startpc is not None else None

    con = sqlite3.connect(args.db)
    require_table(con, "TAGEMISSTRACE")
    cols = get_cols(con, "TAGEMISSTRACE")

    # mainPayload1/mainSel are optional (Exit-Slot v2 dual-candidate debug fields).
    missing = [c for c in ("mainPayload", "realEnc", "mainTag", "predEnc") if c not in cols]
    if missing:
        print(
            "WARNING: TAGEMISSTRACE missing columns %s. "
            "This db cannot fully prove ping-pong at entry level. "
            "Re-run with updated gem5 to log payload/tag/realEnc."
            % (missing,),
            file=sys.stderr,
        )

    aggs: Dict[EntryKey, EntryAgg] = {}
    realenc_missing = "realEnc" not in cols
    predenc_missing = "predEnc" not in cols

    for row in iter_rows(con, cols, startpc, args.limit):
        if "mainFound" in cols and int(row["mainFound"]) == 0:
            continue
        if "mainTable" not in row.keys() or "mainIndex" not in row.keys() or "wayIdx" not in row.keys():
            continue
        k = EntryKey(
            main_table=int(row["mainTable"]),
            main_index=int(row["mainIndex"]),
            way=int(row["wayIdx"]),
            main_tag=int(row["mainTag"]) if "mainTag" in row.keys() else 0,
        )
        a = aggs.get(k)
        if a is None:
            a = EntryAgg()
            aggs[k] = a
        a.n += 1
        if "startPC" in row.keys():
            a.startpcs.add(int(row["startPC"]))
        if "mainPayload" in row.keys():
            p0 = int(row["mainPayload"])
            p1 = int(row["mainPayload1"]) if "mainPayload1" in row.keys() else -1
            a.payload_pairs.add((p0, p1))
        if "mainSel" in row.keys():
            a.sels.add(int(row["mainSel"]))
        if not realenc_missing and "realEnc" in row.keys():
            real = int(row["realEnc"])
            a.real_encs.add(real)
            if not predenc_missing and "predEnc" in row.keys():
                pred = int(row["predEnc"])
                a.pred_encs.add(pred)
                if pred == real:
                    a.correct += 1
        elif not predenc_missing and "predEnc" in row.keys():
            a.pred_encs.add(int(row["predEnc"]))

    # Histogram by distinct realEnc count (a proxy of multi-pattern pressure on one entry).
    hist = collections.Counter()
    for a in aggs.values():
        if a.n < args.min_samples:
            continue
        hist[len(a.real_encs)] += 1

    print("# TAGEMISSTRACE Entry Entropy (min_samples=%d)" % args.min_samples)
    if startpc is not None:
        print("- startPC filter: %s" % hex0(startpc))
    print("- total provider-hit records scanned: %d" % sum(a.n for a in aggs.values()))
    print("- unique entry keys: %d" % len(aggs))
    if "realEnc" in cols:
        print("\n## Distinct realEnc per (table,index,way,tag) histogram")
        for k in sorted(hist.keys()):
            print("- %d distinct realEnc: %d entries" % (k, hist[k]))
    else:
        print("\n## NOTE")
        print("- realEnc not available in this db; histogram is skipped.")

    # Top entries by entropy
    items = []
    if "realEnc" in cols:
        for k, a in aggs.items():
            if a.n < args.min_samples:
                continue
            items.append((len(a.real_encs), a.n, k, a))
        # EntryKey is not orderable; provide an explicit key for deterministic sorting.
        items.sort(
            key=lambda x: (
                x[0],  # distinct realEnc
                x[1],  # samples
                x[2].main_table,
                x[2].main_index,
                x[2].way,
                x[2].main_tag,
            ),
            reverse=True,
        )

    print("\n## Top %d entries by distinct realEnc" % args.top)
    if not items:
        print(
            "WARNING: TAGEMISSTRACE missing required columns (need at least realEnc/predEnc/mainTag/mainPayload). "
            "This db cannot prove ping-pong at entry level; please re-run with an instrumented gem5.opt."
        )
        return 0
    for ent_cnt, n, k, a in items[: args.top]:
        acc = (a.correct / a.n) if ("realEnc" in cols and "predEnc" in cols and a.n) else None
        print(
            "- ent=%d n=%d table=%d index=%d way=%d tag=%s startPCs=%d acc=%s realEnc=%s predEnc=%s payloadPairs=%s sel=%s"
            % (
                ent_cnt,
                n,
                k.main_table,
                k.main_index,
                k.way,
                hex0(k.main_tag) if k.main_tag else "0",
                len(a.startpcs),
                ("%.3f" % acc) if acc is not None else "NA",
                sorted(a.real_encs),
                sorted(a.pred_encs),
                sorted(a.payload_pairs),
                sorted(a.sels),
            )
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
