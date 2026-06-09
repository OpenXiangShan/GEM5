#!/usr/bin/env python3
"""
Consistency checker / bug finder for PerfCCT stall attribution.

Reads a XiangShan gem5 lifetime.db (LifeTimeCommitTrace, optionally
LoadLifeTimeCommitTrace) and checks per-instruction invariants on the recorded
StallReason / SecondaryReason / StallCycles / StallSpans fields. Any instruction
that violates an invariant is flagged with enough context to investigate.

Invariants checked (per committed instruction):

  I1  stall fits in lifetime (independent cross-check):
        total recorded stall cycles (sum of ALL span cycles, incl. HoLBlocked)
        must not exceed the instruction's fetch->commit lifetime,
        (AtCommit - AtFetch) / clock_period.
      This uses the stage timestamps (an independent data source) rather than
      the stall counters, so it can actually catch over-counting bugs.
      (We do NOT check sum(non-HoL spans) == StallCycles: both are derived from
      the same recordStall() counter, so that equality is tautological.)

  I2  secondary consistency:
        if dominant StallReason != 'HoLBlocked' and the inst has any non-HoL
        stall, then SecondaryReason must equal StallReason.
        (Excluding HoL cannot change the arg-max unless HoL was the arg-max.)

  I3  dominant is a recorded reason:
        StallReason must be one of the reasons present in StallSpans (or NoStall
        when there are no spans). NOTE: we deliberately do NOT require it to be
        the cycle-wise arg-max, because dominantStallReason() weights spans by
        tick-extent (which fuses bridged idle gaps), not by the recorded cycle
        count, so the two can legitimately disagree on ties / gap-fused spans.

  I4  replay-ordering (the interesting one):
        if an execution-phase reason (InstNotReady / MemNotReady / Load*Bound /
        Store*Bound / ScalarLongExecute / VectorLongExecute / OtherMemStall)
        appears in StallSpans *after* a HoLBlocked span, the inst must have been
        re-executed. For loads we confirm via LoadLifeTimeCommitTrace.ReplayStr;
        if it is a load with a non-empty replay string -> OK (expected), else the
        ordering is unexplained -> FLAG (candidate bug).

Usage:
    python3 util/perfcct_validate.py m5out/<tag>/lifetime.db
    python3 util/perfcct_validate.py DB --limit 50          # show <=50 flags/inv
    python3 util/perfcct_validate.py DB --pc 0x8000004e      # focus one static PC
"""

import argparse
import os
import sqlite3
import sys

# Execution-phase reasons: an instruction can only enter these while it is still
# computing / waiting on memory, i.e. before it becomes readyToCommit. Seeing one
# AFTER a HoLBlocked span implies the inst lost its readyToCommit state -> replay.
EXEC_REASONS = {
    "InstNotReady", "MemNotReady",
    "LoadL1Bound", "LoadL2Bound", "LoadL3Bound", "LoadMemBound",
    "StoreL1Bound", "StoreL2Bound", "StoreL3Bound", "StoreMemBound",
    "ScalarLongExecute", "VectorLongExecute", "OtherMemStall",
}


def parse_spans(s):
    """'reason:cyc[:firstTick:lastTick];...' -> [(reason,cyc),...]; tolerant."""
    out = []
    if not s:
        return out
    for tok in s.split(";"):
        tok = tok.strip()
        if not tok:
            continue
        f = tok.split(":")
        reason = f[0]
        try:
            cyc = int(f[1]) if len(f) > 1 else 0
        except ValueError:
            cyc = 0
        out.append((reason, cyc))
    return out


def table_exists(cur, name):
    return cur.execute(
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", (name,)
    ).fetchone() is not None


def load_replay_ids(cur):
    """ID -> ReplayStr for loads that were replayed (non-empty)."""
    if not table_exists(cur, "LoadLifeTimeCommitTrace"):
        return {}
    rep = {}
    cols = [r[1] for r in cur.execute("PRAGMA table_info(LoadLifeTimeCommitTrace)")]
    if "ReplayStr" not in cols:
        return {}
    for rid, rstr in cur.execute(
            "SELECT ID, ReplayStr FROM LoadLifeTimeCommitTrace"):
        if rstr:
            rep[rid] = rstr
    return rep


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("db", help="path to lifetime.db")
    ap.add_argument("--limit", type=int, default=20,
                    help="max flagged rows to print per invariant (default 20)")
    ap.add_argument("--pc", default=None,
                    help="only check this static PC (hex, e.g. 0x8000004e)")
    ap.add_argument("--period", type=int, default=0,
                    help="clock period in ticks/cycle for I1 (0 = auto-detect "
                         "from stage timestamps, usually 333)")
    args = ap.parse_args()

    if not os.path.isfile(args.db):
        sys.exit(f"no such file: {args.db}")
    con = sqlite3.connect(args.db)
    cur = con.cursor()
    if not table_exists(cur, "LifeTimeCommitTrace"):
        sys.exit("LifeTimeCommitTrace table not found in this DB.")
    cols = [r[1] for r in cur.execute("PRAGMA table_info(LifeTimeCommitTrace)")]
    for need in ("StallReason", "SecondaryReason", "StallCycles", "StallSpans"):
        if need not in cols:
            sys.exit(f"column {need} missing; rebuild gem5 with the stall-span patch "
                     "and re-run the workload.")

    pc_filter = int(args.pc, 16) if args.pc else None
    replays = load_replay_ids(cur)

    # Clock period in ticks: every pipeline event lands on a cycle boundary, so
    # the gcd of the stage timestamps is the tick-per-cycle period (e.g. 333).
    if args.period:
        period = args.period
    else:
        import math
        period = 0
        for (a, d, c) in cur.execute(
                "SELECT AtFetch, AtDecode, AtCommit FROM LifeTimeCommitTrace "
                "LIMIT 5000"):
            for v in (a, d, c):
                if v:
                    period = math.gcd(period, int(v))
        period = period or 1

    q = ("SELECT ID, PC, DisAsm, StallReason, SecondaryReason, StallCycles, "
         "StallSpans, AtFetch, AtCommit FROM LifeTimeCommitTrace")
    if pc_filter is not None:
        q += f" WHERE PC = {pc_filter}"
    q += " ORDER BY ID"
    rows = cur.execute(q).fetchall()

    n = len(rows)
    flags = {"I1": [], "I2": [], "I3": [], "I4": []}

    for (rid, pc, dis, dom, sec, scyc, spans_s, atf, atc) in rows:
        spans = parse_spans(spans_s)
        per = {}
        for r, c in spans:
            per[r] = per.get(r, 0) + c
        hol = per.get("HoLBlocked", 0)
        nonhol = sum(c for r, c in per.items() if r != "HoLBlocked")
        total_all = sum(c for _, c in spans)

        # I1: total recorded stall cycles must fit in the fetch->commit lifetime
        # (independent cross-check against the stage timestamps). +1 slack for
        # cycle-boundary rounding.
        if atf and atc and atc > atf:
            life_cyc = (atc - atf) // period
            if total_all > life_cyc + 1:
                flags["I1"].append((rid, pc, dis,
                                    f"stall={total_all} > lifetime={life_cyc}cyc",
                                    spans_s))

        # I2: dominant!=HoL & has non-HoL stall => secondary==dominant
        if dom != "HoLBlocked" and nonhol > 0 and sec != dom:
            flags["I2"].append((rid, pc, dis, f"dom={dom} sec={sec}", spans_s))

        # I3: dominant reason must actually appear among the recorded spans
        if per and dom not in per:
            flags["I3"].append((rid, pc, dis, f"dom={dom} not in spans", spans_s))

        # I4: exec-phase reason after a HoLBlocked span -> requires replay
        seen_hol = False
        bad_after = None
        for r, c in spans:
            if r == "HoLBlocked":
                seen_hol = True
            elif seen_hol and r in EXEC_REASONS:
                bad_after = r
                break
        if bad_after is not None and rid not in replays:
            flags["I4"].append((rid, pc, dis,
                                f"'{bad_after}' after HoLBlocked, no replay record",
                                spans_s))

    print(f"=== PerfCCT stall-attribution validation: {args.db} ===")
    print(f"committed instructions checked: {n}"
          + (f"  (PC filter {args.pc})" if pc_filter is not None else ""))
    print(f"loads with replay records:      {len(replays)}")
    print()

    titles = {
        "I1": f"I1 stall <= fetch->commit lifetime (period={period} ticks/cyc)",
        "I2": "I2 secondary consistency (dom!=HoL => secondary==dom)",
        "I3": "I3 dominant reason is present in StallSpans",
        "I4": "I4 replay ordering (exec reason after HoL must be a replayed inst)",
    }
    total_flags = 0
    for key in ("I1", "I2", "I3", "I4"):
        f = flags[key]
        total_flags += len(f)
        status = "OK" if not f else f"{len(f)} FLAGGED"
        print(f"[{status:>11}] {titles[key]}")
        for (rid, pc, dis, why, spans_s) in f[:args.limit]:
            print(f"    #{rid:<7} {pc:#011x} {dis:24.24} {why}")
            print(f"             spans: {spans_s}")
        if len(f) > args.limit:
            print(f"    ... and {len(f) - args.limit} more")
    print()
    print(f"TOTAL FLAGGED: {total_flags}")
    sys.exit(1 if total_flags else 0)


if __name__ == "__main__":
    main()
