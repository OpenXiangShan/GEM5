#!/usr/bin/env python3
"""
Compute simple *offline* upper bounds for Exit-Slot (block-based) TAGE using bp.db.

Why this exists:
  - We want a quick way to answer: "Is per-block exit-slot fundamentally limited, or is our
    current implementation/training leaving accuracy on the table?"
  - We estimate an upper bound under a *fixed feature set* by doing majority-vote per key.

Upper bounds reported (all computed from TAGEMISSTRACE rows):
  UB(startPC):
    For each startPC, always predict the most frequent realEnc under that startPC.
  UB(startPC, indexFoldedHist):
    For each (startPC, indexFoldedHist), always predict the most frequent realEnc.

Interpretation:
  - If UB(startPC, hist) is high but actual acc is low -> implementation/training/aliasing issues.
  - If UB(startPC, hist) itself is low -> the current history signature cannot separate modes;
    need better features (history type/length/folding) or accept a lower ceiling.

Typical usage:
  python3 util/xs_scripts/bp_db_upperbound.py --root /tmp/debug/tage-new6
  python3 util/xs_scripts/bp_db_upperbound.py --db /tmp/debug/tage-new6/xor_dependency_opt/bp.db
"""

from __future__ import annotations

import argparse
import os
import sqlite3
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple


def _connect(path: str) -> sqlite3.Connection:
    con = sqlite3.connect(path)
    # ORDER BY / GROUP BY can spill to temp; keep it in memory to avoid TMPDIR quirks.
    try:
        con.execute("pragma temp_store=memory;")
    except sqlite3.Error:
        pass
    return con


def _has_table(con: sqlite3.Connection, table: str) -> bool:
    cur = con.cursor()
    cur.execute(
        "select 1 from sqlite_master where type='table' and name=?;",
        (table,),
    )
    return cur.fetchone() is not None


def _cols(con: sqlite3.Connection, table: str) -> List[str]:
    return [r[1] for r in con.execute(f"pragma table_info({table});")]


def _mispred_rate(con: sqlite3.Connection) -> Optional[Tuple[int, int, float]]:
    if not _has_table(con, "BPTRACE"):
        return None
    cur = con.cursor()
    n = cur.execute("select count(*) from BPTRACE;").fetchone()[0]
    m = cur.execute("select sum(mispred) from BPTRACE;").fetchone()[0]
    m = int(m or 0)
    return int(n), m, (m / n if n else 0.0)


@dataclass
class UBRes:
    n: int
    actual_acc: Optional[float]
    provider_acc: Optional[float]
    base_acc: Optional[float]
    ub_startpc: Optional[float]
    ub_startpc_hist: Optional[float]
    ub_startpc_fullhist: Optional[float]


def _tage_upperbounds(con: sqlite3.Connection) -> Optional[UBRes]:
    if not _has_table(con, "TAGEMISSTRACE"):
        return None

    cols = set(_cols(con, "TAGEMISSTRACE"))
    if "realEnc" not in cols:
        # Old per-branch schema doesn't carry block label; cannot compute UB.
        n = con.execute("select count(*) from TAGEMISSTRACE;").fetchone()[0]
        return UBRes(
            n=int(n),
            actual_acc=None,
            provider_acc=None,
            base_acc=None,
            ub_startpc=None,
            ub_startpc_hist=None,
            ub_startpc_fullhist=None,
        )

    cur = con.cursor()
    n = int(cur.execute("select count(*) from TAGEMISSTRACE;").fetchone()[0])

    actual_acc = None
    provider_acc = None
    base_acc = None
    if "predEnc" in cols:
        actual_acc = float(
            cur.execute(
                "select 1.0*sum(case when predEnc=realEnc then 1 else 0 end)/count(*) "
                "from TAGEMISSTRACE;"
            ).fetchone()[0]
        )
        if "predSource" in cols:
            v = cur.execute(
                "select case when count(*)=0 then null else "
                "1.0*sum(case when predEnc=realEnc then 1 else 0 end)/count(*) end "
                "from TAGEMISSTRACE where predSource=0;"
            ).fetchone()[0]
            provider_acc = (None if v is None else float(v))
            v = cur.execute(
                "select case when count(*)=0 then null else "
                "1.0*sum(case when predEnc=realEnc then 1 else 0 end)/count(*) end "
                "from TAGEMISSTRACE where predSource=2;"
            ).fetchone()[0]
            base_acc = (None if v is None else float(v))

    # UB(startPC)
    ub_startpc = float(
        cur.execute(
            """
            with per_label as (
              select startPC, realEnc, count(*) as c
              from TAGEMISSTRACE
              group by startPC, realEnc
            ),
            per_startpc as (
              select startPC, max(c) as mx
              from per_label
              group by startPC
            )
            select 1.0*sum(mx)/(select count(*) from TAGEMISSTRACE)
            from per_startpc;
            """
        ).fetchone()[0]
    )

    ub_startpc_hist = None
    if "indexFoldedHist" in cols:
        ub_startpc_hist = float(
            cur.execute(
                """
                with per_label as (
                  select startPC, indexFoldedHist, realEnc, count(*) as c
                  from TAGEMISSTRACE
                  group by startPC, indexFoldedHist, realEnc
                ),
                per_key as (
                  select startPC, indexFoldedHist, max(c) as mx
                  from per_label
                  group by startPC, indexFoldedHist
                )
                select 1.0*sum(mx)/(select count(*) from TAGEMISSTRACE)
                from per_key;
                """
            ).fetchone()[0]
        )

    # UB(startPC, full history bitstring) if available.
    ub_startpc_fullhist = None
    if "history" in cols:
        ub_startpc_fullhist = float(
            cur.execute(
                """
                with per_label as (
                  select startPC, history, realEnc, count(*) as c
                  from TAGEMISSTRACE
                  group by startPC, history, realEnc
                ),
                per_key as (
                  select startPC, history, max(c) as mx
                  from per_label
                  group by startPC, history
                )
                select 1.0*sum(mx)/(select count(*) from TAGEMISSTRACE)
                from per_key;
                """
            ).fetchone()[0]
        )

    return UBRes(
        n=n,
        actual_acc=actual_acc,
        provider_acc=provider_acc,
        base_acc=base_acc,
        ub_startpc=ub_startpc,
        ub_startpc_hist=ub_startpc_hist,
        ub_startpc_fullhist=ub_startpc_fullhist,
    )


@dataclass
class DirUBRes:
    """Offline separability upper bounds for per-branch direction prediction."""

    n: int
    taken_rate: Optional[float]
    actual_acc: Optional[float]  # predTaken vs actualTaken, if predTaken exists
    # Majority-vote UB under different identity/features.
    ub_branchpc: Optional[float]
    ub_branchpc_hist: Optional[float]
    ub_branchpc_fullhist: Optional[float]
    ub_startpc_slot: Optional[float]
    ub_startpc_slot_hist: Optional[float]
    ub_startpc_slot_fullhist: Optional[float]


def _dir_upperbounds(con: sqlite3.Connection) -> Optional[DirUBRes]:
    if not _has_table(con, "TAGEMISSTRACE"):
        return None
    cols = set(_cols(con, "TAGEMISSTRACE"))
    if "actualTaken" not in cols or "branchPC" not in cols:
        return None

    cur = con.cursor()
    n = int(cur.execute("select count(*) from TAGEMISSTRACE;").fetchone()[0])
    if n == 0:
        return DirUBRes(
            n=0,
            taken_rate=None,
            actual_acc=None,
            ub_branchpc=None,
            ub_branchpc_hist=None,
            ub_branchpc_fullhist=None,
            ub_startpc_slot=None,
            ub_startpc_slot_hist=None,
            ub_startpc_slot_fullhist=None,
        )

    taken_rate = float(cur.execute("select 1.0*sum(actualTaken)/count(*) from TAGEMISSTRACE;").fetchone()[0])

    actual_acc = None
    if "predTaken" in cols:
        v = cur.execute(
            "select 1.0*sum(case when predTaken=actualTaken then 1 else 0 end)/count(*) from TAGEMISSTRACE;"
        ).fetchone()[0]
        actual_acc = (None if v is None else float(v))

    # UB(branchPC)
    ub_branchpc = float(
        cur.execute(
            """
            with per_label as (
              select branchPC, actualTaken, count(*) as c
              from TAGEMISSTRACE
              group by branchPC, actualTaken
            ),
            per_key as (
              select branchPC, max(c) as mx
              from per_label
              group by branchPC
            )
            select 1.0*sum(mx)/(select count(*) from TAGEMISSTRACE)
            from per_key;
            """
        ).fetchone()[0]
    )

    ub_branchpc_hist = None
    if "indexFoldedHist" in cols:
        ub_branchpc_hist = float(
            cur.execute(
                """
                with per_label as (
                  select branchPC, indexFoldedHist, actualTaken, count(*) as c
                  from TAGEMISSTRACE
                  group by branchPC, indexFoldedHist, actualTaken
                ),
                per_key as (
                  select branchPC, indexFoldedHist, max(c) as mx
                  from per_label
                  group by branchPC, indexFoldedHist
                )
                select 1.0*sum(mx)/(select count(*) from TAGEMISSTRACE)
                from per_key;
                """
            ).fetchone()[0]
        )

    ub_branchpc_fullhist = None
    if "history" in cols:
        ub_branchpc_fullhist = float(
            cur.execute(
                """
                with per_label as (
                  select branchPC, history, actualTaken, count(*) as c
                  from TAGEMISSTRACE
                  group by branchPC, history, actualTaken
                ),
                per_key as (
                  select branchPC, history, max(c) as mx
                  from per_label
                  group by branchPC, history
                )
                select 1.0*sum(mx)/(select count(*) from TAGEMISSTRACE)
                from per_key;
                """
            ).fetchone()[0]
        )

    # UB(startPC, slot): approximate the benefit of injecting "position" identity.
    # Slot is computed at 2B granularity and masked to 5 bits (0..31) to match the typical
    # in-block slot encoding.
    ub_startpc_slot = None
    ub_startpc_slot_hist = None
    ub_startpc_slot_fullhist = None
    if "startPC" in cols:
        ub_startpc_slot = float(
            cur.execute(
                """
                with per_label as (
                  select startPC, ((branchPC - startPC) >> 1) & 31 as slot, actualTaken, count(*) as c
                  from TAGEMISSTRACE
                  group by startPC, slot, actualTaken
                ),
                per_key as (
                  select startPC, slot, max(c) as mx
                  from per_label
                  group by startPC, slot
                )
                select 1.0*sum(mx)/(select count(*) from TAGEMISSTRACE)
                from per_key;
                """
            ).fetchone()[0]
        )
        if "indexFoldedHist" in cols:
            ub_startpc_slot_hist = float(
                cur.execute(
                    """
                    with per_label as (
                      select startPC, ((branchPC - startPC) >> 1) & 31 as slot,
                             indexFoldedHist, actualTaken, count(*) as c
                      from TAGEMISSTRACE
                      group by startPC, slot, indexFoldedHist, actualTaken
                    ),
                    per_key as (
                      select startPC, slot, indexFoldedHist, max(c) as mx
                      from per_label
                      group by startPC, slot, indexFoldedHist
                    )
                    select 1.0*sum(mx)/(select count(*) from TAGEMISSTRACE)
                    from per_key;
                    """
                ).fetchone()[0]
            )
        if "history" in cols:
            ub_startpc_slot_fullhist = float(
                cur.execute(
                    """
                    with per_label as (
                      select startPC, ((branchPC - startPC) >> 1) & 31 as slot,
                             history, actualTaken, count(*) as c
                      from TAGEMISSTRACE
                      group by startPC, slot, history, actualTaken
                    ),
                    per_key as (
                      select startPC, slot, history, max(c) as mx
                      from per_label
                      group by startPC, slot, history
                    )
                    select 1.0*sum(mx)/(select count(*) from TAGEMISSTRACE)
                    from per_key;
                    """
                ).fetchone()[0]
            )

    return DirUBRes(
        n=n,
        taken_rate=taken_rate,
        actual_acc=actual_acc,
        ub_branchpc=ub_branchpc,
        ub_branchpc_hist=ub_branchpc_hist,
        ub_branchpc_fullhist=ub_branchpc_fullhist,
        ub_startpc_slot=ub_startpc_slot,
        ub_startpc_slot_hist=ub_startpc_slot_hist,
        ub_startpc_slot_fullhist=ub_startpc_slot_fullhist,
    )


def _fmt_pct(x: Optional[float]) -> str:
    if x is None:
        return "n/a"
    return f"{x*100:5.1f}%"


def _fmt_n(x: Optional[int]) -> str:
    if x is None:
        return "n/a"
    # Compact human-readable counts.
    if x >= 1_000_000_000:
        return f"{x/1_000_000_000:.1f}G"
    if x >= 1_000_000:
        return f"{x/1_000_000:.1f}M"
    if x >= 1_000:
        return f"{x/1_000:.1f}k"
    return str(x)


def _analyze_one(db: str) -> Dict[str, object]:
    con = _connect(db)
    ub = _tage_upperbounds(con)
    dir_ub = _dir_upperbounds(con)
    bp = _mispred_rate(con)
    con.close()
    return {"db": db, "ub": ub, "dir_ub": dir_ub, "bp": bp}


def main() -> int:
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--db", help="analyze one bp.db")
    g.add_argument("--root", help="scan a /tmp/debug/tage-newX directory that contains */bp.db")
    args = ap.parse_args()

    if args.db:
        r = _analyze_one(args.db)
        ub: Optional[UBRes] = r["ub"]  # type: ignore[assignment]
        dub: Optional[DirUBRes] = r["dir_ub"]  # type: ignore[assignment]
        bp = r["bp"]
        print(f"# {args.db}")
        if bp is not None:
            n, m, rate = bp
            print(f"- BPTRACE mispred: {rate*100:.2f}% ({m}/{n})")
        if ub is not None and ub.ub_startpc is not None:
            print(f"- TAGEMISSTRACE samples: {ub.n}")
            print(f"- actual acc:            {_fmt_pct(ub.actual_acc)}")
            print(f"- provider acc:          {_fmt_pct(ub.provider_acc)}")
            print(f"- base acc:              {_fmt_pct(ub.base_acc)}")
            print(f"- UB_exit(startPC):      {_fmt_pct(ub.ub_startpc)}")
            print(f"- UB_exit(startPC,hist): {_fmt_pct(ub.ub_startpc_hist)}")
            print(f"- UB_exit(startPC,H):    {_fmt_pct(ub.ub_startpc_fullhist)}")
            if ub.actual_acc is not None and ub.ub_startpc_hist is not None:
                print(f"- headroom (UB2-acc):    {_fmt_pct(ub.ub_startpc_hist - ub.actual_acc)}")
        if dub is not None:
            print(f"- DIR samples:           {dub.n}")
            print(f"- DIR taken rate:        {_fmt_pct(dub.taken_rate)}")
            print(f"- DIR actual acc:        {_fmt_pct(dub.actual_acc)}")
            print(f"- UB_dir(branchPC):      {_fmt_pct(dub.ub_branchpc)}")
            print(f"- UB_dir(branchPC,hist): {_fmt_pct(dub.ub_branchpc_hist)}")
            print(f"- UB_dir(branchPC,H):    {_fmt_pct(dub.ub_branchpc_fullhist)}")
            print(f"- UB_dir(startPC,slot):  {_fmt_pct(dub.ub_startpc_slot)}")
            print(f"- UB_dir(startPC,slot,hist): {_fmt_pct(dub.ub_startpc_slot_hist)}")
            print(f"- UB_dir(startPC,slot,H):    {_fmt_pct(dub.ub_startpc_slot_fullhist)}")
        return 0

    root: str = args.root
    # Pair *_opt with *_ref.
    benches: Dict[str, Dict[str, str]] = {}
    for d in os.listdir(root):
        if not d.endswith(("_opt", "_ref")):
            continue
        kind = "opt" if d.endswith("_opt") else "ref"
        base = d[: -len("_opt")] if kind == "opt" else d[: -len("_ref")]
        db = os.path.join(root, d, "bp.db")
        if os.path.exists(db):
            benches.setdefault(base, {})[kind] = db

    rows = []
    for base, mp in sorted(benches.items()):
        opt = _analyze_one(mp["opt"]) if "opt" in mp else None
        ref = _analyze_one(mp["ref"]) if "ref" in mp else None
        rows.append((base, opt, ref))

    # Print a compact table for quick comparison.
    print(f"# Upperbound Report: {root}")
    print("")
    print("## What This Report Measures")
    print("")
    print("- This is an *offline separability upper bound* computed from `bp.db`.")
    print("- For each chosen feature key (e.g., `(startPC, history)`), we compute the best possible")
    print("  accuracy under 0/1 loss by always predicting the *most frequent label* for that key")
    print("  (majority vote). This is Bayes-optimal given only that key.")
    print("- It is **NOT** an oracle that peeks at the future; it quantifies whether the available")
    print("  features contain enough information to separate patterns.")
    print("")
    print("### Exit-slot (per-block) label")
    print("")
    print("- Uses `TAGEMISSTRACE.realEnc` (0..32) as the true label for Exit-Slot multi-class classification.")
    print("- `UB_exit(startPC,hist)`: key is `(startPC, indexFoldedHist)`.")
    print("- `UB_exit(startPC,H)`: key is `(startPC, history_string)` (low 50 bits in current logging).")
    print("")
    print("### Direction (per-branch) label")
    print("")
    print("- Uses `TAGEMISSTRACE.actualTaken` (0/1) as the true label for direction prediction.")
    print("- `acc_dir(ref)`: measured accuracy `predTaken==actualTaken` in ref trace (if `predTaken` exists).")
    print("- `UB_dir(ref startPC,slot,hist)`: key is `(startPC, slot, indexFoldedHist)`, where")
    print("  `slot = ((branchPC - startPC) >> 1) & 31` approximates in-block position identity.")
    print("- `UB_dir(ref startPC,slot,H)`: key is `(startPC, slot, history_string)`.")
    print("")
    print("### About `n/a`")
    print("")
    print("- `n/a` means the db does not have usable samples for that metric (missing table/columns,")
    print("  or `TAGEMISSTRACE` exists but has 0 rows for that run).")
    print("")
    header = (
        "| bench | BP mispred opt | BP mispred ref | delta | "
        "n_exit(opt) | acc_exit(opt) | UB_exit(startPC,hist) | UB_exit(startPC,H) | "
        "n_dir(ref) | acc_dir(ref) | UB_dir(ref startPC,slot,hist) | UB_dir(ref startPC,slot,H) |"
    )
    sep = "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"
    print(header)
    print(sep)

    reg_items: List[Tuple[float, str]] = []

    for base, opt, ref in rows:
        opt_bp = opt["bp"] if opt else None  # type: ignore[index]
        ref_bp = ref["bp"] if ref else None  # type: ignore[index]

        def _bp_fmt(x: Optional[Tuple[int, int, float]]) -> str:
            if x is None:
                return "n/a"
            return f"{x[2]*100:5.2f}%"

        opt_rate = opt_bp[2] if opt_bp else None
        ref_rate = ref_bp[2] if ref_bp else None
        delta = (opt_rate - ref_rate) if (opt_rate is not None and ref_rate is not None) else None

        opt_ub: Optional[UBRes] = opt["ub"] if opt else None  # type: ignore[index]
        ref_dir: Optional[DirUBRes] = (ref["dir_ub"] if ref else None)  # type: ignore[index]

        n_exit = opt_ub.n if (opt_ub and opt_ub.ub_startpc is not None) else None
        acc_exit = opt_ub.actual_acc if (opt_ub and opt_ub.actual_acc is not None) else None
        ub_exit2 = opt_ub.ub_startpc_hist if opt_ub else None
        ub_exit3 = opt_ub.ub_startpc_fullhist if opt_ub else None

        n_dir = ref_dir.n if (ref_dir and ref_dir.n) else None
        acc_dir = ref_dir.actual_acc if ref_dir else None
        ub_dir2 = ref_dir.ub_startpc_slot_hist if ref_dir else None
        ub_dir3 = ref_dir.ub_startpc_slot_fullhist if ref_dir else None
        if delta is not None:
            reg_items.append((delta, base))

        def _pct(x: Optional[float]) -> str:
            if x is None:
                return "n/a"
            return f"{x*100:5.1f}%"

        print(
            "| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |"
            % (
                base,
                _bp_fmt(opt_bp),
                _bp_fmt(ref_bp),
                ("n/a" if delta is None else f"{delta*100:+.2f}%"),
                _fmt_n(n_exit),
                _pct(acc_exit),
                _pct(ub_exit2),
                _pct(ub_exit3),
                _fmt_n(n_dir),
                _pct(acc_dir),
                _pct(ub_dir2),
                _pct(ub_dir3),
            )
        )

    reg_items.sort(reverse=True)
    print("")
    print("## Biggest BP mispred regressions (opt - ref)")
    for d, b in reg_items[:10]:
        print(f"- {b}: {d*100:+.2f}%")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
