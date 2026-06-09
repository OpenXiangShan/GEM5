#!/usr/bin/env python3
"""Tabular viewer for PerfCCT (arch_db) LifeTimeCommitTrace.

Unlike util/perfcct.py (which draws the per-cycle ASCII timeline), this script
prints structured *tables*: one row per committed instruction with the cycle
each pipeline stage was reached, end-to-end latency, and the stall attribution
columns (StallReason / StallCycles) added to PerfCCT.

Examples
--------
# Per-inst table, first 50 committed insts
python3 util/perfcct_table.py m5out/coremark_perfcct/lifetime.db -n 50

# Window by *cycle* (AtCommit), only a given PC, sorted by latency
python3 util/perfcct_table.py DB -s 100000 -e 100500 --pc 0x80000a7a --sort lat

# Export the whole per-inst table to CSV (open in Excel / VSCode)
python3 util/perfcct_table.py DB --csv perinst.csv

# Aggregate tables: stage residency, stall histogram, top blocker PCs
python3 util/perfcct_table.py DB --agg
"""
import argparse
import csv
import os
import sqlite3
import sys

STAGES = ['AtFetch', 'AtDecode', 'AtRename', 'AtDispQue', 'AtIssueQue',
          'AtIssueArb', 'AtIssueReadReg', 'AtFU', 'AtBypassVal',
          'AtWriteVal', 'AtCommit']
# short header label per stage
SHORT = ['f', 'dec', 'ren', 'dpq', 'isq', 'arb', 'rrf', 'fu', 'byp', 'wb', 'cmt']

# Module residency intervals reconstructed from the lifecycle timestamps.
# Each inst occupies a module during [enter_cycle, exit_cycle).
#   (label, enter_stage, exit_stage)
MODULES = [
    ('Fetch',      'AtFetch',    'AtDecode'),     # in fetch / ibuffer
    ('Decode',     'AtDecode',   'AtRename'),     # in decode
    ('Rename',     'AtRename',   'AtIssueQue'),   # in rename/dispatch
    ('IssueQ',     'AtIssueQue', 'AtFU'),         # waiting in issue queue
    ('Execute',    'AtFU',       'AtWriteVal'),   # in FU (execute+bypass)
    ('CommitWait', 'AtWriteVal', 'AtCommit'),     # done, waiting in-order commit
    ('InFlight',   'AtFetch',    'AtCommit'),     # anywhere in the machine
]


def cyc(tick, period):
    return tick // period if tick else 0


def fmt_pc(pc):
    return "0x%012x" % (pc & 0xffffffffffff)


def load_rows(cur, args):
    where = []
    if args.start_cycle is not None:
        where.append(f"AtCommit >= {args.start_cycle * args.period}")
    if args.end_cycle is not None:
        where.append(f"AtCommit <= {args.end_cycle * args.period}")
    if args.pc is not None:
        pcval = int(args.pc, 16)
        # PC stored masked to low 60 bits; match on low 48 bits to be safe
        where.append(f"(PC & 0xffffffffffff) = {pcval & 0xffffffffffff}")
    if args.no_holblocked:
        where.append("StallReason != 'HoLBlocked'")
    wsql = ("WHERE " + " AND ".join(where)) if where else ""
    order = "ASC" if args.sort == "commit" else None
    if args.sort == "lat":
        order_sql = "ORDER BY (AtCommit-AtFetch) DESC"
    elif args.sort == "stall":
        order_sql = "ORDER BY StallCycles DESC"
    else:
        order_sql = "ORDER BY AtCommit ASC"
    limit = f"LIMIT {args.limit}" if args.limit and args.limit > 0 else ""
    cols = ("ID," + ",".join(STAGES) +
            ",StallReason,StallCycles,SecondaryReason,DisAsm,PC")
    cur.execute(f"SELECT {cols} FROM LifeTimeCommitTrace {wsql} {order_sql} {limit}")
    return cur.fetchall()


def per_inst(cur, args):
    rows = load_rows(cur, args)
    if not rows:
        print("(no rows match the filter)")
        return
    P = args.period
    # detect which stages are non-zero anywhere in the selection (drop dead cols)
    nstage = len(STAGES)
    used = [False] * nstage
    for r in rows:
        for i in range(nstage):
            if r[1 + i]:
                used[i] = True
    keep = [i for i in range(nstage) if used[i]]

    if args.csv:
        with open(args.csv, "w", newline="") as f:
            w = csv.writer(f)
            head = (["ID", "PC", "abs_fetch_cyc"] +
                    [SHORT[i] + "_cyc" for i in keep] +
                    ["total_cyc", "StallReason", "StallCycles",
                     "SecondaryReason", "DisAsm"])
            w.writerow(head)
            for r in rows:
                rid = r[0]
                stagev = r[1:1 + nstage]
                reason, scyc, secondary, disasm, pc = r[1 + nstage:]
                fetch = stagev[0]
                line = [rid, fmt_pc(pc), cyc(fetch, P)]
                for i in keep:
                    line.append(cyc(stagev[i] - fetch, P) if stagev[i] else "")
                line += [cyc(stagev[-1] - fetch, P), reason, scyc,
                         secondary, disasm]
                w.writerow(line)
        print(f"wrote {len(rows)} rows -> {args.csv}")
        return

    # terminal table; stage columns are cycle offset from fetch (f=0),
    # absF = absolute fetch cycle (arrival time on the global timeline)
    hdr = (f"{'ID':>8} {'PC':>14} {'absF':>11} " +
           " ".join(f"{SHORT[i]:>4}" for i in keep) +
           f" {'tot':>5} {'StallReason':>22} {'sCyc':>6} "
           f"{'SecondaryReason':>22}  disasm")
    print(hdr)
    print("-" * len(hdr))
    for r in rows:
        rid = r[0]
        stagev = r[1:1 + nstage]
        reason, scyc, secondary, disasm, pc = r[1 + nstage:]
        fetch = stagev[0]
        cells = []
        for i in keep:
            cells.append(f"{cyc(stagev[i]-fetch, P):>4}" if stagev[i] else f"{'-':>4}")
        tot = cyc(stagev[-1] - fetch, P)
        # when blocked behind ROB head, surface the inst's own real problem
        sec = secondary if reason == 'HoLBlocked' else ''
        print(f"{rid:>8} {fmt_pc(pc):>14} {cyc(fetch, P):>11} " + " ".join(cells) +
              f" {tot:>5} {reason:>22} {scyc:>6} {sec:>22}  {disasm}")
    print(f"\n({len(rows)} rows; absF=absolute fetch cycle, "
          f"other stage columns = cycle offset from fetch)")


def agg(cur, args):
    P = args.period
    print("===== 1) Stage residency (avg cycles between consecutive stages) =====")
    for i in range(1, len(STAGES)):
        a, b = STAGES[i - 1], STAGES[i]
        row = cur.execute(
            f"SELECT AVG(({b}-{a})*1.0/{P}), COUNT(*) FROM LifeTimeCommitTrace "
            f"WHERE {a}>0 AND {b}>={a}").fetchone()
        d, n = row
        val = "(unused)" if d is None else f"{d:8.3f} cyc  (n={n})"
        print(f"  {a:14} -> {b:14}: {val}")
    tot = cur.execute(
        f"SELECT AVG((AtCommit-AtFetch)*1.0/{P}) FROM LifeTimeCommitTrace "
        f"WHERE AtFetch>0").fetchone()[0]
    print(f"  end-to-end fetch -> commit: {tot:8.3f} cyc")

    print("\n===== 2) StallReason histogram (by total stall cycles) =====")
    print(f"  {'StallReason':24}{'insts':>10}{'stall_cyc':>14}")
    for r, n, s in cur.execute(
            "SELECT StallReason,COUNT(*),SUM(StallCycles) FROM LifeTimeCommitTrace "
            "GROUP BY StallReason ORDER BY 3 DESC"):
        print(f"  {r:24}{n:>10}{(s or 0):>14}")

    print("\n===== 2b) Real problem behind HoLBlocked (SecondaryReason) =====")
    print("  (for insts whose dominant reason is HoLBlocked, what was their own"
          " stall)")
    print(f"  {'SecondaryReason':24}{'insts':>10}")
    for r, n in cur.execute(
            "SELECT SecondaryReason,COUNT(*) FROM LifeTimeCommitTrace "
            "WHERE StallReason='HoLBlocked' GROUP BY SecondaryReason "
            "ORDER BY 2 DESC"):
        tag = "  (pure victim, no own stall)" if r == 'NoStall' else ""
        print(f"  {r:24}{n:>10}{tag}")

    print("\n===== 3) Top blocker PCs (excluding HoLBlocked victims) =====")
    print(f"  {'PC':16}{'StallReason':22}{'insts':>8}{'stall_cyc':>12}  disasm")
    for pc, rs, n, s in cur.execute(
            "SELECT PC,StallReason,COUNT(*),SUM(StallCycles) FROM LifeTimeCommitTrace "
            "WHERE StallReason!='HoLBlocked' AND StallReason!='NoStall' "
            "GROUP BY PC,StallReason ORDER BY 4 DESC LIMIT 15"):
        asm = cur.execute("SELECT DisAsm FROM LifeTimeCommitTrace WHERE PC=? LIMIT 1",
                          (pc,)).fetchone()[0]
        print(f"  {fmt_pc(pc):16}{rs:22}{n:>8}{s:>12}  {asm}")


def occupancy(cur, args):
    """Reconstruct per-cycle module occupancy from lifecycle timestamps.

    For every committed inst and every module, the inst is 'resident' during
    [enter_cycle, exit_cycle). We accumulate counts per cycle with a difference
    array, then prefix-sum to get the occupancy time series.

    NOTE: PerfCCT only records *committed* insts (no wrong-path / squashed),
    so occupancy is a lower bound, especially during misprediction recovery.
    """
    P = args.period
    idx = {name: i for i, name in enumerate(STAGES)}
    cur.execute(f"SELECT {','.join(STAGES)} FROM LifeTimeCommitTrace WHERE AtFetch>0")
    rows = cur.fetchall()
    if not rows:
        print("(no rows)")
        return

    # cycle window
    lo = args.start_cycle if args.start_cycle is not None \
        else min(r[idx['AtFetch']] for r in rows) // P
    hi = args.end_cycle if args.end_cycle is not None \
        else max(r[idx['AtCommit']] for r in rows) // P
    if hi < lo:
        print("error: end-cycle < start-cycle")
        return
    span = hi - lo + 1

    labels = [m[0] for m in MODULES]
    diff = {lab: [0] * (span + 1) for lab in labels}
    for r in rows:
        for lab, ec, xc in MODULES:
            a, b = r[idx[ec]], r[idx[xc]]
            if not a or not b or b <= a:
                continue
            ca, cb = a // P, b // P                  # resident [ca, cb)
            ca = max(ca, lo)
            cb = min(cb, hi + 1)
            if cb <= ca:
                continue
            diff[lab][ca - lo] += 1
            diff[lab][cb - lo] -= 1

    series = {}
    stat = {}
    for lab in labels:
        d = diff[lab]
        run = 0
        ssum = 0
        smax = 0
        ser = []
        for i in range(span):
            run += d[i]
            ser.append(run)
            ssum += run
            smax += 0
            if run > smax:
                smax = run
        series[lab] = ser
        stat[lab] = (ssum / span, smax)

    print(f"===== Per-module occupancy over cycles [{lo}, {hi}]  "
          f"({span} cycles, {len(rows)} committed insts) =====")
    print(f"  {'module':12}{'avg_occ':>10}{'max_occ':>10}")
    for lab in labels:
        avg, mx = stat[lab]
        print(f"  {lab:12}{avg:>10.2f}{mx:>10}")
    print("  (avg_occ = mean #insts resident per cycle; only committed insts)")

    if args.csv:
        with open(args.csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["cycle"] + labels)
            for i in range(span):
                w.writerow([lo + i] + [series[lab][i] for lab in labels])
        print(f"\nwrote {span} cycle rows -> {args.csv}")
    elif args.start_cycle is not None and args.end_cycle is not None:
        if span > 400:
            print(f"\n(window {span} cycles too wide for terminal; "
                  f"add --csv FILE to dump the per-cycle table)")
        else:
            print(f"\n  {'cycle':>10} " + " ".join(f"{lab[:6]:>6}" for lab in labels))
            for i in range(span):
                print(f"  {lo+i:>10} " +
                      " ".join(f"{series[lab][i]:>6}" for lab in labels))
    else:
        print("\n(add -s START -e END for a per-cycle table, "
              "or --csv FILE to dump every cycle)")


def inst_occupancy(cur, args):
    """Long/tidy table: one row per (cycle, module, instruction) that is
    resident in that module at that cycle. Reconstructs *which* insts sit in
    each pipeline module every cycle (not just the count).

    Output columns: cycle, module, ID, PC, DisAsm

    Rows explode quickly, so a cycle window (-s/-e) and/or --csv is required.
    InFlight is excluded by default (it is the union of all modules); add it
    back with --modules.
    """
    P = args.period
    idx = {name: i for i, name in enumerate(STAGES)}

    # which modules to list
    if args.modules:
        want = [m.strip() for m in args.modules.split(",")]
        mods = [m for m in MODULES if m[0] in want]
        if not mods:
            print(f"error: --modules matched nothing; valid: "
                  f"{[m[0] for m in MODULES]}")
            return
    else:
        mods = [m for m in MODULES if m[0] != 'InFlight']

    cols = "ID," + ",".join(STAGES) + ",PC,DisAsm"
    where = []
    if args.start_cycle is not None:
        where.append(f"AtCommit >= {args.start_cycle * P}")
    if args.end_cycle is not None:
        where.append(f"AtFetch <= {args.end_cycle * P}")
    wsql = ("WHERE " + " AND ".join(where)) if where else ""
    cur.execute(f"SELECT {cols} FROM LifeTimeCommitTrace {wsql}")
    rows = cur.fetchall()
    if not rows:
        print("(no rows match the filter)")
        return

    lo = args.start_cycle if args.start_cycle is not None \
        else min(r[1 + idx['AtFetch']] for r in rows) // P
    hi = args.end_cycle if args.end_cycle is not None \
        else max(r[1 + idx['AtCommit']] for r in rows) // P

    def gen():
        for r in rows:
            rid = r[0]
            stagev = r[1:1 + len(STAGES)]
            pc = r[-2]
            disasm = r[-1]
            for lab, ec, xc in mods:
                a, b = stagev[idx[ec]], stagev[idx[xc]]
                if not a or not b or b <= a:
                    continue
                ca = max(a // P, lo)
                cb = min(b // P, hi + 1)
                for c in range(ca, cb):
                    yield (c, lab, rid, fmt_pc(pc), disasm)

    if args.csv:
        n = 0
        with open(args.csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["cycle", "module", "ID", "PC", "DisAsm"])
            for row in gen():
                w.writerow(row)
                n += 1
        print(f"wrote {n} rows -> {args.csv}  (cycles [{lo},{hi}], "
              f"modules={[m[0] for m in mods]})")
        return

    if args.start_cycle is None or args.end_cycle is None:
        print("error: detailed inst-occupancy needs a window (-s START -e END) "
              "or --csv FILE (it produces one row per cycle*module*inst).")
        return
    if (hi - lo + 1) > 80:
        print(f"window {hi-lo+1} cycles too wide for terminal; add --csv FILE.")
        return
    out = sorted(gen())
    print(f"{'cycle':>8} {'module':12} {'ID':>8} {'PC':>14}  disasm")
    for c, lab, rid, pc, disasm in out:
        print(f"{c:>8} {lab:12} {rid:>8} {pc:>14}  {disasm}")
    print(f"\n({len(out)} rows; cycles [{lo},{hi}], modules={[m[0] for m in mods]})")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sqldb")
    ap.add_argument("-p", "--period", type=int, default=333,
                    help="ticks per cycle (3GHz=333, 2GHz=500)")
    ap.add_argument("-n", "--limit", type=int, default=50,
                    help="max rows for per-inst table (0=all)")
    ap.add_argument("-s", "--start-cycle", type=int, default=None)
    ap.add_argument("-e", "--end-cycle", type=int, default=None)
    ap.add_argument("--pc", type=str, default=None, help="filter by PC (hex)")
    ap.add_argument("--sort", choices=["commit", "lat", "stall"], default="commit",
                    help="commit order / fetch->commit latency / stall cycles")
    ap.add_argument("--no-holblocked", action="store_true",
                    help="drop HoLBlocked rows (show real blockers)")
    ap.add_argument("--csv", type=str, default=None, help="export per-inst table to CSV")
    ap.add_argument("--agg", action="store_true", help="show aggregate tables instead")
    ap.add_argument("--occupancy", action="store_true",
                    help="per-cycle occupancy COUNT of each pipeline module")
    ap.add_argument("--inst-occupancy", dest="inst_occupancy", action="store_true",
                    help="long table: which insts are resident in each module each cycle")
    ap.add_argument("--modules", type=str, default=None,
                    help="comma list of modules to include (default: all except InFlight)")
    args = ap.parse_args()

    if not os.path.isfile(args.sqldb):
        sys.exit(f"error: db file not found: {args.sqldb}\n"
                 f"       (pass the real PerfCCT db path, e.g. "
                 f"m5out/<tag>/lifetime.db)")
    con = sqlite3.connect(args.sqldb)
    cur = con.cursor()
    if not cur.execute("SELECT name FROM sqlite_master WHERE type='table' "
                       "AND name='LifeTimeCommitTrace'").fetchone():
        tbls = [r[0] for r in cur.execute(
            "SELECT name FROM sqlite_master WHERE type='table'")]
        sys.exit(f"error: table LifeTimeCommitTrace not found in {args.sqldb}\n"
                 f"       tables present: {tbls or '(none)'}\n"
                 f"       was this run done with --enable-arch-db and "
                 f"dump_lifetime=True?")
    if args.inst_occupancy:
        inst_occupancy(cur, args)
    elif args.occupancy:
        occupancy(cur, args)
    elif args.agg:
        agg(cur, args)
    else:
        per_inst(cur, args)


if __name__ == "__main__":
    main()
