#!/usr/bin/env python3
"""Generate interactive HTML report(s) from a PerfCCT (arch_db) db.

Reads LifeTimeCommitTrace and emits TWO self-contained HTML files:

  <base>_overview.html  program-wide statistics (Plotly charts):
                        module occupancy over time, real stall-reason
                        histogram, stage residency, latency distribution,
                        top blocker PCs.
  <base>_detail.html    per-cycle module contents (pick a cycle -> which
                        insts sit in each pipeline module) + a searchable /
                        sortable per-instruction top-N stall table.

    # both reports next to the db
    python3 util/perfcct_report.py m5out/coremark_perfcct/lifetime.db

    # only the detail report, top-2000 by stall cycles, custom window
    python3 util/perfcct_report.py DB --part detail --rank stall \
        --max-rows 2000 --detail-start 100000 --detail-end 102000

Overview needs internet for the Plotly CDN; detail is fully offline.
"""
import argparse
import json
import os
import sqlite3
import sys

STAGES = ['AtFetch', 'AtDecode', 'AtRename', 'AtDispQue', 'AtIssueQue',
          'AtIssueArb', 'AtIssueReadReg', 'AtFU', 'AtBypassVal',
          'AtWriteVal', 'AtCommit']
MODULES = [
    ('Fetch',      'AtFetch',    'AtDecode'),
    ('Decode',     'AtDecode',   'AtRename'),
    ('Rename',     'AtRename',   'AtIssueQue'),
    ('IssueQ',     'AtIssueQue', 'AtFU'),
    ('Execute',    'AtFU',       'AtWriteVal'),
    ('CommitWait', 'AtWriteVal', 'AtCommit'),
    ('InFlight',   'AtFetch',    'AtCommit'),
]

# Finer structural view (detail panel 2): front-end queues + IQ sub-stages
# (mutually exclusive positions) plus the overlapping container structures
# ROB / LQ / SQ. (label, enter_stage, exit_stage, memfilter); memfilter limits
# LQ/SQ to loads/stores (atomics 'A' count as both). ROB/LQ/SQ enter at dispatch
# (AtDispQue) and leave at commit (or, for wrong-path insts, at the squash tick
# which gem5 stores in the AtCommit column of SquashedLifeTimeTrace).
# NOTE: AtDispQue is never recorded in this XiangShan config, so we use AtRename
# as the dispatch/allocation proxy: ROB / LQ / SQ entries are allocated around
# rename, so their residency is approximated as AtRename -> AtCommit. The
# rename+dispatch-queue gap is one module (AtRename -> AtIssueQue).
DET2_MODULES = [
    ('FetchQ',     'AtFetch',        'AtDecode',       None),
    ('DecodeQ',    'AtDecode',       'AtRename',       None),
    ('Rename/Disp', 'AtRename',      'AtIssueQue',     None),
    ('IQ-wait',    'AtIssueQue',     'AtIssueArb',     None),
    ('IQ-arb',     'AtIssueArb',     'AtIssueReadReg', None),
    ('IQ-rdReg',   'AtIssueReadReg', 'AtFU',           None),
    ('Execute',    'AtFU',           'AtWriteVal',     None),
    ('ROB',        'AtRename',       'AtCommit',       None),
    ('LQ',         'AtRename',       'AtCommit',       'L'),
    ('SQ',         'AtRename',       'AtCommit',       'S'),
]


def fmt_pc(pc):
    return "0x%012x" % (pc & 0xffffffffffff)


def build(db, period, max_rows, occ_buckets,
          detail_start=None, detail_end=None, detail_window=2000,
          detail_all=False, rank='lat'):
    con = sqlite3.connect(db)
    c = con.cursor()
    if not c.execute("SELECT name FROM sqlite_master WHERE type='table' "
                     "AND name='LifeTimeCommitTrace'").fetchone():
        sys.exit("error: LifeTimeCommitTrace not found in " + db)
    idx = {n: i for i, n in enumerate(STAGES)}
    # MemType present only on newer gem5 builds (for LQ/SQ classification).
    main_cols = [d[1] for d in c.execute("PRAGMA table_info(LifeTimeCommitTrace)")]
    has_memtype = 'MemType' in main_cols
    mt_sel = ",MemType" if has_memtype else ",'' AS MemType"
    c.execute("SELECT " + ",".join(STAGES) +
              ",StallReason,StallCycles,SecondaryReason,StallSpans" + mt_sel +
              ",DisAsm,PC,ID "
              "FROM LifeTimeCommitTrace WHERE AtFetch>0")
    rows = c.fetchall()
    P = period
    ninsts = len(rows)

    # Load replay info (ID -> (replay_count, replay_type_chars, last_replay_tick)).
    # A load that already reported ready-to-commit can be nuked/replayed and sent
    # back to re-execute, which is why some HoLBlocked spans land *before* the
    # final AtFU (AtFU keeps only the last execution pass). Surfacing the replay
    # count in the report explains that apparent inconsistency.
    replay = {}
    if c.execute("SELECT name FROM sqlite_master WHERE type='table' "
                 "AND name='LoadLifeTimeCommitTrace'").fetchone():
        lcols = [d[1] for d in c.execute(
            "PRAGMA table_info(LoadLifeTimeCommitTrace)")]
        has_ticks = 'ReplayTicks' in lcols
        has_exec = 'ExecuteTicks' in lcols
        sel = "SELECT ID,ReplayStr,LastReplay" + \
              (",ReplayTicks" if has_ticks else "") + \
              (",ExecuteTicks" if has_exec else "") + \
              " FROM LoadLifeTimeCommitTrace"
        for row in c.execute(sel):
            rid, rstr, lastr = row[0], row[1], row[2]
            if not rstr:
                continue
            ticks = []
            if has_ticks and row[3]:
                ticks = [int(x) for x in row[3].split() if x]
            exec_t = []
            if has_exec and row[3 + (1 if has_ticks else 0)]:
                exec_t = [int(x) for x in
                          row[3 + (1 if has_ticks else 0)].split() if x]
            replay[rid] = (len(rstr), rstr, lastr, ticks, exec_t)
    # column layout: STAGES..., reason(R), scyc(R+1), secondary(R+2),
    #                spans(R+3), memtype(R+4), disasm(R+5), pc(-2), id(-1)
    R = len(STAGES)
    MEMT = R + 4             # MemType column ('L'/'S'/'A'/'')
    DIS = R + 5              # disasm column index

    def _toint(x):
        return int(x) if x and x.lstrip('-').isdigit() else None

    def parse_spans(s):
        # span record: "reason:cycles[:firstTick:lastTick]"; ticks present only
        # with the newer gem5 build (used to place each span at its real cycle).
        out = []
        if not s:
            return out
        for part in s.split(";"):
            f = part.split(":")
            if not f or not f[0]:
                continue
            cyc = (_toint(f[1]) if len(f) > 1 else 0) or 0
            ft = _toint(f[2]) if len(f) > 2 else None
            lt = _toint(f[3]) if len(f) > 3 else None
            out.append([f[0], cyc, ft, lt])
        return out

    from collections import Counter, defaultdict

    # --- device / MMIO I/O detection -------------------------------------
    # No address/miss tables in this trace, but device (uncacheable, e.g. UART
    # status polling) loads have a fixed, abnormally long access latency
    # (FU->WriteVal ~hundreds of cyc) vs ~1-3 cyc for cacheable loads, so they
    # are cleanly separable. Such a load at the in-order ROB head freezes commit
    # and is *not* real compute, so we exclude the device loads (and the cycles
    # the ROB spends blocked behind them) from every CPU statistic below.
    IO_TH = 200  # cyc: FU->WriteVal >= this => device/MMIO access
    AtF, AtC = idx['AtFetch'], idx['AtCommit']
    AtFUi, AtWVi = idx['AtFU'], idx['AtWriteVal']

    def _is_io(r):
        fu, wv = r[AtFUi], r[AtWVi]
        return (r[MEMT] == 'L' and fu and wv and wv > fu
                and (wv - fu) // P >= IO_TH)

    io_row = [_is_io(r) for r in rows]
    fmin = min(r[AtF] for r in rows) // P
    cmax = max(r[AtC] for r in rows) // P
    span = cmax - fmin + 1
    # per-cycle I/O mask by commit order: the head inst owns the cycles
    # (prevCommit, ownCommit]; those owned by a device load are I/O-wait cycles.
    io_mask = bytearray(span + 2)
    _ord = sorted(range(ninsts), key=lambda i: rows[i][AtC])
    _prev = rows[_ord[0]][AtC] if _ord else 0
    for i in _ord:
        cm = rows[i][AtC]
        if io_row[i]:
            a = max(0, _prev // P - fmin)
            b = min(span, cm // P - fmin)
            for k in range(a, b):
                io_mask[k] = 1
        _prev = cm
    io_pre = [0] * (span + 2)        # prefix sum: io_pre[k] = #I/O cyc in [0,k)
    for k in range(span):
        io_pre[k + 1] = io_pre[k] + io_mask[k]

    def io_between(c0, c1):          # #I/O cycles in [c0,c1) (absolute cycles)
        a = min(max(c0 - fmin, 0), span)
        b = min(max(c1 - fmin, 0), span)
        return io_pre[b] - io_pre[a]

    # --- stall histogram ---
    # HoLBlocked is passive waiting (like NoStall: e.g. a load auto-waits a few
    # cycles before the L1 lookup), so ranking it against real, actionable stall
    # reasons is meaningless. We collapse it into the inst's *effective* reason:
    #   effReason = SecondaryReason when dominant==HoLBlocked, else StallReason
    # and count by #insts (NOT stall cycles: a victim's StallCycles is mostly HoL
    # wait time, which would wrongly inflate the secondary reason). NoStall is
    # dropped from the chart since it is not an actionable stall.
    eff_n = Counter()        # effective (real) reason, by inst count
    sec_n = Counter()        # secondary reason among HoLBlocked-dominant insts

    def eff_reason(r):
        return r[R + 2] if r[R] == 'HoLBlocked' else r[R]

    for i, r in enumerate(rows):
        if io_row[i]:                # exclude device-I/O loads from CPU stats
            continue
        reason = r[R]
        eff = eff_reason(r)
        if eff != 'NoStall':
            eff_n[eff] += 1
        if reason == 'HoLBlocked':
            sec_n[r[R + 2]] += 1

    # --- stage residency (avg cycles between consecutive stages) ---
    stage_lat = []
    for i in range(1, len(STAGES)):
        a, b = idx[STAGES[i - 1]], idx[STAGES[i]]
        tot = n = 0
        for j, r in enumerate(rows):
            if io_row[j]:
                continue
            if r[a] > 0 and r[b] >= r[a]:
                tot += (r[b] - r[a]); n += 1
        stage_lat.append((STAGES[i - 1] + "->" + STAGES[i],
                          (tot / n / P) if n else 0.0))

    # --- latency distribution (fetch->commit cycles) ---
    lats = [(r[idx['AtCommit']] - r[idx['AtFetch']]) // P for r in rows]
    # Distribution uses CPU latency = fetch->commit minus any device-I/O cycles
    # overlapping the inst's lifetime (so commit-stall behind a device load is
    # not counted), and drops the device loads themselves.
    cpu_lats = []
    for i, r in enumerate(rows):
        if io_row[i]:
            continue
        v = lats[i] - io_between(r[AtF] // P, r[AtC] // P)
        cpu_lats.append(v if v > 0 else 0)
    lo, hi = (min(cpu_lats), max(cpu_lats)) if cpu_lats else (0, 0)
    nb = 60
    width = max(1, (hi - lo) // nb + 1)
    hist = Counter()
    for v in cpu_lats:
        hist[(v - lo) // width] += 1
    lat_x = [lo + k * width for k in range(((hi - lo) // width) + 1)]
    lat_y = [hist.get(k, 0) for k in range(len(lat_x))]

    # --- occupancy over time (downsampled) ---
    labels = [m[0] for m in MODULES]
    diff = {lab: [0] * (span + 1) for lab in labels}
    for r in rows:
        for lab, ec, xc in MODULES:
            a, b = r[idx[ec]], r[idx[xc]]
            if not a or not b or b < a:
                continue
            ca, cb = a // P - fmin, b // P - fmin
            if cb <= ca:               # single-cycle stage: count 1 cycle
                cb = ca + 1
            diff[lab][ca] += 1
            diff[lab][cb] -= 1
    bucket = max(1, span // occ_buckets)
    occ_series = {}
    occ_x = [fmin + k * bucket for k in range((span // bucket) + 1)]
    peak_cyc = fmin
    peak_val = -1
    for lab in labels:
        d = diff[lab]
        run = 0
        acc = 0
        cnt = 0
        ser = []
        for i in range(span):
            run += d[i]
            if lab == 'InFlight' and run > peak_val:
                peak_val = run
                peak_cyc = fmin + i
            acc += run
            cnt += 1
            if cnt == bucket:
                ser.append(round(acc / cnt, 2)); acc = 0; cnt = 0
        if cnt:
            ser.append(round(acc / cnt, 2))
        occ_series[lab] = ser

    # default detail window: open on the busiest *CPU* region (most commits per
    # window with little device I/O) instead of the peak InFlight cycle, which
    # sits inside a device-I/O stall (ROB full of insts waiting behind the head).
    commits_cyc = [0] * (span + 1)
    for r in rows:
        cc = r[AtC] // P - fmin
        if 0 <= cc < span:
            commits_cyc[cc] += 1
    cpu_peak_cyc = peak_cyc
    W = max(1, min(detail_window, span))
    if span > W:
        cwin = sum(commits_cyc[0:W])
        best = (-1, 0)
        for s in range(span - W):
            if s:
                cwin += commits_cyc[s + W - 1] - commits_cyc[s - 1]
            iowin = io_pre[s + W] - io_pre[s]
            if iowin < W * 0.2 and cwin > best[0]:
                best = (cwin, s)
        if best[0] > 0:
            cpu_peak_cyc = fmin + best[1] + W // 2

    # --- top blocker PCs (excluding HoLBlocked / NoStall) ---
    blk_cyc = defaultdict(int)
    blk_reason = {}
    blk_asm = {}
    for i, r in enumerate(rows):
        if io_row[i]:
            continue
        reason = r[R]
        if reason in ('HoLBlocked', 'NoStall'):
            continue
        pc = r[-2]
        blk_cyc[pc] += r[R + 1]
        blk_reason[pc] = reason
        blk_asm[pc] = r[DIS]
    top_blk = sorted(blk_cyc.items(), key=lambda x: -x[1])[:20]

    # --- per-inst table (top-N by latency or by stall cycles; <=0 means ALL) ---
    if rank == 'stall':
        order = sorted(range(ninsts), key=lambda i: -rows[i][R + 1])
    else:
        order = sorted(range(ninsts), key=lambda i: -lats[i])
    if max_rows and max_rows > 0:
        order = order[:max_rows]
    table = []
    for i in order:
        r = rows[i]
        fetch = r[idx['AtFetch']]
        rep = replay.get(r[-1])
        table.append({
            "id": r[-1],
            "rep": rep[0] if rep else 0,        # number of load replays
            "reptypes": rep[1] if rep else "",  # replay-type chars (e.g. "OOON")
            "reptick": ((rep[2] - fetch) // P) if (rep and fetch) else None,
            # per-replay cycle (relative to fetch), aligned with reptypes chars
            "repcyc": [((t - fetch) // P) for t in rep[3]] if (rep and fetch) else [],
            # per-pass execution-start cycle (one per AtFU pass); lets the module
            # bar draw the real IssueQ->Execute sawtooth for replayed loads.
            "execcyc": [((t - fetch) // P) for t in rep[4]] if (rep and fetch and len(rep) > 4) else [],
            "pc": fmt_pc(r[-2]),
            "absF": fetch // P,
            "f": 0,
            "dec": (r[idx['AtDecode']] - fetch) // P if r[idx['AtDecode']] else None,
            "ren": (r[idx['AtRename']] - fetch) // P if r[idx['AtRename']] else None,
            "isq": (r[idx['AtIssueQue']] - fetch) // P if r[idx['AtIssueQue']] else None,
            "fu": (r[idx['AtFU']] - fetch) // P if r[idx['AtFU']] else None,
            "wb": (r[idx['AtWriteVal']] - fetch) // P if r[idx['AtWriteVal']] else None,
            "cmt": (r[idx['AtCommit']] - fetch) // P if r[idx['AtCommit']] else None,
            "tot": lats[i],
            "reason": eff_reason(r),          # real reason (HoLBlocked->secondary)
            "raw": r[R],                       # original dominant reason
            "scyc": r[R + 1],
            "asm": r[DIS],
            # spans as [reason, cycles, startCyc, endCyc] relative to fetch, so
            # the timeline can be aligned in time with the module/stage bar.
            "spans": [
                [name, cyc,
                 ((ft - fetch) // P) if (ft is not None and fetch) else None,
                 ((lt - fetch) // P + 1) if (lt is not None and fetch) else None]
                for (name, cyc, ft, lt) in parse_spans(r[R + 3])
            ],
        })

    # --- per-cycle / per-module instruction detail (windowed) ---
    # Embedding every inst's lifecycle for the whole run would be huge, so we
    # embed only insts overlapping a cycle window [ds,de]. The window defaults
    # to detail_window cycles centred on the busiest *CPU* cycle (most commits,
    # device-I/O excluded), or can be set explicitly via --detail-start/-end.
    det_mods = [m for m in MODULES if m[0] != 'InFlight']
    if detail_all:
        ds, de = fmin, cmax
    elif detail_start is not None and detail_end is not None:
        ds, de = detail_start, detail_end
    else:
        half = detail_window // 2
        ds = max(fmin, cpu_peak_cyc - half)
        de = ds + detail_window
    de = min(de, cmax)
    # Compact, interned encoding so the window (or whole run with --detail-all)
    # stays as small as possible. Each inst -> flat array:
    #   [id, pcRef, asmRef, e0,x0, e1,x1, ...]  (enter/exit cycle per module,
    #   0,0 when the inst never occupied that module). pc/asm strings repeat
    #   heavily (loops), so we intern them into side tables.
    asmtab, asmidx = [], {}
    pctab, pcidx = [], {}

    def intern(tab, d, s):
        i = d.get(s)
        if i is None:
            i = len(tab)
            d[s] = i
            tab.append(s)
        return i

    det_insts = []
    for r in rows:
        f_cyc = r[idx['AtFetch']] // P
        c_cyc = r[idx['AtCommit']] // P
        if c_cyc < ds or f_cyc > de:        # inst not alive in the window
            continue
        rec = [r[-1], intern(pctab, pcidx, fmt_pc(r[-2])),
               intern(asmtab, asmidx, r[DIS])]
        for lab, ec, xc in det_mods:
            a, b = r[idx[ec]], r[idx[xc]]
            if a and b and b >= a:
                ea, xb = a // P, b // P
                if xb <= ea:           # single-cycle stage: still show 1 cycle
                    xb = ea + 1        # (e.g. 1-cycle ALU execute, AtFU==AtWriteVal)
                rec += [ea, xb]
            else:
                rec += [0, 0]
        det_insts.append(rec)

    # --- detail panel 2: finer structural occupancy incl. squashed insts ---
    # Pull wrong-path (squashed) insts; their AtCommit column holds the squash
    # tick (when they left the machine), which bounds any structure they still
    # occupied. Same first-11 STAGES layout as the main query, so `idx` works.
    sq_rows = []
    if c.execute("SELECT name FROM sqlite_master WHERE type='table' "
                 "AND name='SquashedLifeTimeTrace'").fetchone():
        scols = [d[1] for d in c.execute(
            "PRAGMA table_info(SquashedLifeTimeTrace)")]
        s_mt = "MemType" if 'MemType' in scols else "'' AS MemType"
        c.execute("SELECT " + ",".join(STAGES) + "," + s_mt +
                  ",DisAsm,PC,ID FROM SquashedLifeTimeTrace WHERE AtFetch>0")
        sq_rows = c.fetchall()
    SMEMT, SDIS, SPC, SID = len(STAGES), len(STAGES) + 1, len(STAGES) + 2, \
        len(STAGES) + 3

    def det2_rec(ts, memtype, pcv, asmv, idv, squashed):
        rec = [idv, intern(pctab, pcidx, pcv),
               intern(asmtab, asmidx, asmv), 1 if squashed else 0]
        ctick = ts[idx['AtCommit']]      # commit tick, or squash tick if squashed
        for lab, ec, xc, mf in DET2_MODULES:
            a, b = ts[idx[ec]], ts[idx[xc]]
            ok = bool(a and a > 0)
            if ok and mf == 'L' and memtype not in ('L', 'A'):
                ok = False
            elif ok and mf == 'S' and memtype not in ('S', 'A'):
                ok = False
            if ok:
                if not b or b < a:        # still resident at squash -> extend
                    b = ctick if squashed else 0
                if b and b >= a:
                    ea, xb = a // P, b // P
                    if xb <= ea:
                        xb = ea + 1
                    rec += [ea, xb]
                else:
                    rec += [0, 0]
            else:
                rec += [0, 0]
        return rec

    det2_insts = []
    for r in rows:
        if (r[idx['AtCommit']] // P) < ds or (r[idx['AtFetch']] // P) > de:
            continue
        det2_insts.append(det2_rec(r, r[MEMT], fmt_pc(r[-2]), r[DIS],
                                   r[-1], False))
    for sr in sq_rows:
        af, sq = sr[idx['AtFetch']], sr[idx['AtCommit']]
        if not af:
            continue
        x_cyc = (sq // P) if sq else (af // P)
        if x_cyc < ds or (af // P) > de:
            continue
        det2_insts.append(det2_rec(sr, sr[SMEMT], fmt_pc(sr[SPC]),
                                   sr[SDIS], sr[SID], True))

    # --- cycle accounting summary (uses the device-I/O mask built above) ---
    io_cyc = io_pre[span]
    cpu_cyc = span - io_cyc
    cycacct = {
        "io_th": IO_TH,
        "total": span,
        "io": io_cyc,
        "cpu": cpu_cyc,
        "io_insts": sum(io_row),
        "ipc": round(ninsts / span, 3) if span else 0,
        "ipc_cpu": round(ninsts / cpu_cyc, 3) if cpu_cyc else 0,
    }

    return {
        "db": os.path.basename(db),
        "period": P,
        "ninsts": ninsts,
        "span": span,
        "cycacct": cycacct,
        "detail": {"start": ds, "end": de,
                   "peak_cyc": cpu_peak_cyc, "peak_val": peak_val,
                   "modules": [m[0] for m in det_mods],
                   "pc": pctab, "asm": asmtab, "insts": det_insts},
        "detail2": {"modules": [m[0] for m in DET2_MODULES],
                    "insts": det2_insts, "nsquash": len(sq_rows)},
        "stall": [{"reason": k, "insts": eff_n[k]}
                  for k in sorted(eff_n, key=lambda x: -eff_n[x])],
        "sec": [{"reason": k, "insts": sec_n[k]}
                for k in sorted(sec_n, key=lambda x: -sec_n[x])],
        "stage_lat": stage_lat,
        "lat_x": lat_x, "lat_y": lat_y,
        "occ_x": occ_x, "occ": occ_series, "occ_labels": labels,
        "top_blk": [{"pc": fmt_pc(p), "reason": blk_reason[p],
                     "cyc": cyc, "asm": blk_asm[p]} for p, cyc in top_blk],
        "table": table,
        "max_rows": ("all" if not max_rows or max_rows <= 0 else max_rows),
        "ntable": len(table),
        "rank": rank,
    }


_STYLE = """
 body{font-family:system-ui,Segoe UI,Arial,sans-serif;margin:18px;background:#0f1115;color:#e6e6e6}
 h1{font-size:20px} h2{font-size:16px;margin-top:28px;border-bottom:1px solid #333;padding-bottom:4px}
 .meta{color:#9aa;font-size:13px;margin-bottom:10px}
 .grid{display:grid;grid-template-columns:1fr 1fr;gap:16px}
 .chart{background:#171a21;border:1px solid #2a2f3a;border-radius:8px;padding:6px}
 input,select{background:#1c2129;color:#e6e6e6;border:1px solid #39414f;
border-radius:6px;padding:6px 8px;font-size:13px}
 table{border-collapse:collapse;width:100%;font-size:12px;margin-top:8px}
 th,td{border:1px solid #2a2f3a;padding:3px 6px;text-align:right;white-space:nowrap}
 th{cursor:pointer;position:sticky;top:0;background:#1c2129}
 td.l,th.l{text-align:left}
 tr:nth-child(even){background:#13161c}
 .wrap{max-height:560px;overflow:auto;border:1px solid #2a2f3a;border-radius:8px}
 .hint{color:#9aa;font-size:12px}
"""

# Shared JS: renders the cycle-accounting banner (CPU work vs device-I/O wait).
_CYCACCT_JS = r"""
function renderCycAcct(a){
  const el=document.getElementById('cycacct');
  if(!el||!a||!a.total){return;}
  const f=n=>n.toLocaleString();
  const pc=v=>(100*v/a.total).toFixed(1);
  el.innerHTML=
    `<div style="background:#171a21;border:1px solid #2a2f3a;border-radius:8px;`+
    `padding:10px 12px;margin-bottom:10px">`+
    `<div style="font-weight:bold;margin-bottom:6px">Cycle accounting `+
    `<span class="hint">(device/MMIO I/O wait excluded from CPU work; `+
    `a load is device I/O when FU&rarr;WriteVal &ge; ${a.io_th} cyc)</span></div>`+
    `<div style="display:flex;height:22px;border-radius:5px;overflow:hidden;`+
    `font-size:11px;line-height:22px;text-align:center">`+
    `<div style="width:${pc(a.cpu)}%;background:#2ecc71;color:#04210f" `+
    `title="effective CPU work">CPU ${pc(a.cpu)}%</div>`+
    `<div style="width:${pc(a.io)}%;background:#e74c3c;color:#2a0a07" `+
    `title="device/MMIO I/O busy-wait (e.g. UART polling)">I/O ${pc(a.io)}%</div></div>`+
    `<div class="hint" style="margin-top:6px">`+
    `total ${f(a.total)} cyc &nbsp;|&nbsp; `+
    `<span style="color:#2ecc71">effective CPU ${f(a.cpu)} cyc (${pc(a.cpu)}%)</span> &nbsp;|&nbsp; `+
    `<span style="color:#e74c3c">device-I/O wait ${f(a.io)} cyc `+
    `(${pc(a.io)}%), ${f(a.io_insts)} device loads</span><br>`+
    `IPC overall = <b>${a.ipc}</b> &nbsp;|&nbsp; `+
    `IPC excluding I/O = <b style="color:#2ecc71">${a.ipc_cpu}</b></div></div>`;
}
"""

# ---------------------------------------------------------------------------
# Part 1: program-wide statistics (charts only).
# ---------------------------------------------------------------------------
HTML_OVERVIEW = r"""<!DOCTYPE html>
<html><head><meta charset="utf-8">
<title>PerfCCT overview - __DB__</title>
<script src="https://cdn.plot.ly/plotly-2.35.2.min.js"></script>
<style>__STYLE__</style></head><body>
<h1>PerfCCT overview &mdash; __DB__</h1>
<div class="meta" id="meta"></div>
<div id="cycacct"></div>
<div class="hint">Program-wide statistics. Per-cycle module contents &amp; the
 per-instruction stall table live in the <b>*_detail.html</b> report.</div>

<h2>Module occupancy over time (avg #insts resident per cycle)</h2>
<div class="chart"><div id="occ" style="height:360px"></div></div>

<div class="grid">
 <div><h2>Real stall reason (HoLBlocked&rarr;secondary, excl. NoStall)</h2>
   <div class="chart"><div id="stall" style="height:340px"></div></div></div>
 <div><h2>Stage residency (avg cycles)</h2>
   <div class="chart"><div id="stage" style="height:340px"></div></div></div>
 <div><h2>Latency distribution (fetch&rarr;commit cycles)</h2>
   <div class="chart"><div id="lat" style="height:340px"></div></div></div>
 <div><h2>Top blocker PCs (excl. HoLBlocked)</h2>
   <div class="chart"><div id="blk" style="height:340px"></div></div></div>
</div>

<script>
const D = __DATA__;
document.getElementById('meta').textContent =
  `db=${D.db}  period=${D.period} ticks/cyc  `+
  `committed insts=${D.ninsts.toLocaleString()}  `+
  `span=${D.span.toLocaleString()} cyc`;
renderCycAcct(D.cycacct);
const dark={paper_bgcolor:'#171a21',plot_bgcolor:'#171a21',font:{color:'#e6e6e6'},margin:{t:10,r:10,b:40,l:50}};

Plotly.newPlot('occ', D.occ_labels.map(l=>({x:D.occ_x,y:D.occ[l],name:l,mode:'lines',type:'scatter'})),
  Object.assign({},dark,{margin:{t:10,r:10,b:90,l:50},xaxis:{title:'cycle'},yaxis:{title:'avg occupancy'},
    legend:{orientation:'h',y:-0.28,yanchor:'top'}}),{responsive:true});
Plotly.newPlot('stall', [{x:D.stall.map(s=>s.reason),y:D.stall.map(s=>s.insts),type:'bar',marker:{color:'#e67e22'}}],
  Object.assign({},dark,{yaxis:{title:'#insts',type:'log'}}),{responsive:true});
Plotly.newPlot('stage', [{x:D.stage_lat.map(s=>s[0].replace(/At/g,'')),
  y:D.stage_lat.map(s=>s[1]),type:'bar',marker:{color:'#3498db'}}],
  Object.assign({},dark,{yaxis:{title:'avg cyc'}}),{responsive:true});
Plotly.newPlot('lat', [{x:D.lat_x,y:D.lat_y,type:'bar',marker:{color:'#2ecc71'}}],
  Object.assign({},dark,{xaxis:{title:'fetch->commit cyc'},yaxis:{title:'#insts',type:'log'}}),{responsive:true});
Plotly.newPlot('blk', [{y:D.top_blk.map(b=>b.pc+' '+b.asm).reverse(),x:D.top_blk.map(b=>b.cyc).reverse(),
  type:'bar',orientation:'h',marker:{color:'#9b59b6'},
  text:D.top_blk.map(b=>b.reason).reverse(),textposition:'auto'}],
  Object.assign({},dark,{margin:{t:10,r:10,b:40,l:230},xaxis:{title:'stall cycles'}}),{responsive:true});
__CYCACCT_JS__
</script>
</body></html>"""


# ---------------------------------------------------------------------------
# Part 2: per-cycle module contents + per-instruction (top-N stall) table.
# ---------------------------------------------------------------------------
HTML_DETAIL = r"""<!DOCTYPE html>
<html><head><meta charset="utf-8">
<title>PerfCCT detail - __DB__</title>
<style>__STYLE__</style></head><body>
<h1>PerfCCT detail &mdash; __DB__</h1>
<div class="meta" id="meta"></div>
<div id="cycacct"></div>
<div class="hint">Per-cycle module contents &amp; the per-instruction stall
 table. Program-wide charts live in the <b>*_overview.html</b> report.
 <br><b>sCyc</b> = the inst's own stall cycles, <b>excluding</b> passive
 HoL-blocked (head-of-line) waiting.</div>

<h2>Instructions in each module at a given cycle
  <span class="hint" id="detmeta"></span></h2>
<div>
 <label class="hint">cycle <input id="detcyc" type="number" style="width:120px"></label>
 <input id="detrange" type="range" style="width:420px;vertical-align:middle">
 <span class="hint" id="detcnt"></span>
</div>
<div id="detgrid" style="display:flex;gap:10px;overflow-x:auto;margin-top:8px"></div>

<h2>Microarchitectural structures at that cycle (finer modules, incl. squashed)
  <span class="hint" id="det2meta"></span></h2>
<div class="hint">Same cycle slider as above. Front-end queues &amp; IQ sub-stages are mutually-exclusive
 positions; <b>ROB / LQ / SQ</b> are container structures that overlap them (an inst in IssueQ is also in the ROB).
 <span style="color:#e74c3c">&#10007;</span> marks wrong-path (squashed) insts.</div>
<div id="det2grid" style="display:flex;gap:6px;overflow-x:auto;margin-top:8px"></div>

<h2>Per-instruction table <span class="hint">(embedded: top __MAXROWS__
 by __RANK__; type to filter, click header to sort, <b>click a row to
 expand its full stall-reason timeline</b>)</span></h2>
<div>
 <input id="q" placeholder="filter: pc / disasm / reason ..." size="40" oninput="render()">
 <select id="ronly" onchange="render()">
   <option value="">all insts</option>
   <option value="real">only real stalls (reason&ne;NoStall)</option>
 </select>
 <span class="hint" id="cnt"></span>
</div>
<div class="wrap"><table id="t"><thead><tr id="th"></tr></thead><tbody id="tb"></tbody></table></div>

<script>
const D = __DATA__;
document.getElementById('meta').textContent =
  `db=${D.db}  period=${D.period} ticks/cyc  `+
  `committed insts=${D.ninsts.toLocaleString()}  `+
  `span=${D.span.toLocaleString()} cyc`;
renderCycAcct(D.cycacct);
__CYCACCT_JS__

// ---- per-cycle module-instruction detail ----
const DET=D.detail;
const MODCOL={Fetch:'#3498db',Decode:'#1abc9c',Rename:'#2ecc71',IssueQ:'#e74c3c',
              ReplayQ:'#c0392b',Execute:'#e67e22',CommitWait:'#9b59b6'};
// inst record layout: [id, pcRef, asmRef, e0,x0, e1,x1, ...] per DET.modules
const NM=DET.modules.length;
document.getElementById('detmeta').textContent=
  `(window ${DET.start.toLocaleString()}\u2013${DET.end.toLocaleString()} cyc, `+
  `${DET.insts.length.toLocaleString()} insts embedded; busiest cycle \u2248${DET.peak_cyc.toLocaleString()}, `+
  `InFlight\u2248${DET.peak_val}. Regen with --detail-start/--detail-end, or --detail-all for the whole run.)`;
const dcyc=document.getElementById('detcyc'),drng=document.getElementById('detrange');
dcyc.min=drng.min=DET.start;dcyc.max=drng.max=DET.end;
const startCyc=Math.min(DET.peak_cyc,DET.end);
dcyc.value=drng.value=startCyc;
function renderDet(){
  const c=+dcyc.value;
  const cols=DET.modules.map(()=>[]);
  for(const it of DET.insts){
    for(let k=0;k<NM;k++){const e=it[3+2*k],x=it[4+2*k];
      if(x>e&&e<=c&&c<x)cols[k].push(it);}
  }
  const g=document.getElementById('detgrid');g.innerHTML='';
  let tot=0;
  DET.modules.forEach((m,k)=>{
    const list=cols[k];tot+=list.length;
    const box=document.createElement('div');
    box.style.cssText='flex:1;min-width:180px;background:#171a21;'+
      'border:1px solid #2a2f3a;border-radius:8px;padding:6px';
    let html=`<div style="font-weight:bold;color:${MODCOL[m]||'#ccc'}">${m} `+
             `<span class="hint">(${list.length})</span></div>`;
    html+='<div style="max-height:340px;overflow:auto;font-size:12px;margin-top:4px">';
    list.sort((a,b)=>a[0]-b[0]);
    for(const it of list)
      html+=`<div style="white-space:nowrap">#${it[0]} `+
        `<span class="hint">${DET.pc[it[1]]}</span> ${DET.asm[it[2]]}</div>`;
    if(!list.length)html+='<div class="hint">(empty)</div>';
    html+='</div>';box.innerHTML=html;g.appendChild(box);
  });
  document.getElementById('detcnt').textContent=`cycle ${c}: ${tot} insts in flight`;
}
// ---- per-cycle finer structural occupancy (incl. squashed) ----
const DET2=D.detail2;
const NM2=DET2?DET2.modules.length:0;
const MOD2COL={FetchQ:'#3498db',DecodeQ:'#1abc9c','Rename/Disp':'#2ecc71',
  'IQ-wait':'#e74c3c','IQ-arb':'#d35400','IQ-rdReg':'#e67e22',Execute:'#f39c12',
  ROB:'#9b59b6',LQ:'#2980b9',SQ:'#8e44ad'};
if(DET2)document.getElementById('det2meta').textContent=
  `(same window; ${DET2.insts.length.toLocaleString()} insts incl. ${DET2.nsquash.toLocaleString()} squashed)`;
function renderDet2(){
  if(!DET2)return;
  const c=+dcyc.value;
  // record layout: [id, pcRef, asmRef, squashed, e0,x0, e1,x1, ...]
  const cols=DET2.modules.map(()=>[]);
  for(const it of DET2.insts){
    for(let k=0;k<NM2;k++){const e=it[4+2*k],x=it[5+2*k];
      if(x>e&&e<=c&&c<x)cols[k].push(it);}
  }
  const g=document.getElementById('det2grid');g.innerHTML='';
  DET2.modules.forEach((m,k)=>{
    const list=cols[k];
    const box=document.createElement('div');
    box.style.cssText='flex:1;min-width:120px;background:#171a21;'+
      'border:1px solid #2a2f3a;border-radius:8px;padding:6px';
    let html=`<div style="font-weight:bold;color:${MOD2COL[m]||'#ccc'}">${m} `+
             `<span class="hint">(${list.length})</span></div>`;
    html+='<div style="max-height:300px;overflow:auto;font-size:11px;margin-top:4px">';
    list.sort((a,b)=>a[0]-b[0]);
    for(const it of list){
      const sq=it[3]?'<span style="color:#e74c3c">\u2717</span>':'';
      html+=`<div style="white-space:nowrap">${sq}#${it[0]} `+
        `<span class="hint">${DET.pc[it[1]]}</span> ${DET.asm[it[2]]}</div>`;
    }
    if(!list.length)html+='<div class="hint">(empty)</div>';
    html+='</div>';box.innerHTML=html;g.appendChild(box);
  });
}
dcyc.oninput=()=>{drng.value=dcyc.value;renderDet();renderDet2();};
drng.oninput=()=>{dcyc.value=drng.value;renderDet();renderDet2();};
renderDet();renderDet2();

// ---- per-instruction table ----
const COLS=[['id','ID'],['absF','absF'],['pc','PC'],['dec','dec'],['ren','ren'],['isq','isq'],
            ['fu','fu'],['wb','wb'],['cmt','cmt'],['tot','tot'],['reason','Reason(real)'],['scyc','sCyc'],
            ['raw','rawDominant'],['asm','disasm']];
let sortKey='__SORTKEY__',sortDir=-1;
const thr=document.getElementById('th');
const THS=[];
COLS.forEach(([k,lab])=>{const th=document.createElement('th');th.textContent=lab;
  if(k==='pc'||k==='asm'||k==='reason'||k==='raw')th.className='l';
  th.onclick=()=>{if(sortKey===k)sortDir=-sortDir;
    else{sortKey=k;sortDir=(k==='pc'||k==='id'||k==='absF')?1:-1;}render();};
  thr.appendChild(th);THS.push([k,lab,th]);});
function updateHeaders(){THS.forEach(([k,lab,th])=>{
  th.textContent=lab+(sortKey===k?(sortDir>0?' \u25B2':' \u25BC'):'');});}

// stall-reason -> stable color (HoLBlocked = muted gray since it is passive)
// distinct colors for replay-wait segments, keyed by replay type name
const RWCOL={'Reschedule wait':'#b5651d','STLF wait':'#c98a3a','Nuke wait':'#a0522d',
  'BankConflict wait':'#8c6d1f','CacheMiss wait':'#9c5a2a','TLBMiss wait':'#7a5230',
  'CacheBlocked wait':'#b07d4a','RAR wait':'#8a6f3d','RAW wait':'#6e5a2e',
  'MdpAddr wait':'#a36b3a','MshrAliasFail wait':'#7f5a35','HitInWriteBuffer wait':'#956b40',
  'MshrArbFail wait':'#86643a','Replay wait':'#b5651d'};
function colorFor(name){
  if(name==='HoLBlocked')return '#555a66';
  if(/\u2191BE$/.test(name))return '#5b6b8a';   // backend reason inherited in FE
  if(RWCOL[name])return RWCOL[name];
  if(name==='NoStall')return '#2e7d32';
  let h=0;for(let i=0;i<name.length;i++)h=(h*31+name.charCodeAt(i))>>>0;
  return `hsl(${h%360},58%,52%)`;
}
// A reason recorded *before* the inst reaches the IssueQ is not its own work:
// rename charges every inst stuck in its buffer with the ROB/LQ/SQ-head reason
// (checkRenameStallFromIEW) -- the backend bottleneck it is queued behind. Tag
// these so e.g. "MemNotReady" while still in Rename reads as an inherited reason.
function feLabel(r,name,s1){
  if(r.isq!=null && s1!=null && s1<=r.isq && name!=='NoStall' && !/ wait$/.test(name))
    return name+' \u2191BE';
  return name;
}
// expandable per-inst stall timeline (the full span sequence incl. HoLBlocked)
// module-residency bar: split the full lifetime into pipeline stages, with
// vertical separators and an inline label per stage (Fetch/Decode/.../CommitWait).
function moduleBar(r){
  const life=r.tot||0;
  if(!life)return '';
  const ex=r.execcyc||[], rp=r.repcyc||[];
  const saw=(r.rep && ex.length>1);   // replayed load -> per-pass sawtooth
  let segs;
  if(saw){
    // front-end is a single pass; back-end repeats once per AtFU pass:
    //   pass i:  IssueQ[reissue_i, exec_i]  then  Execute[exec_i, end_i]
    //   reissue_0 = AtIssueQue, reissue_i = replay_{i-1}
    //   end_i = replay_i (failed pass) or AtWriteVal (final, successful pass)
    segs=[['Fetch',0,r.dec],['Decode',r.dec,r.ren],['Rename',r.ren,r.isq]];
    for(let i=0;i<ex.length;i++){
      const istart=(i===0)?r.isq:rp[i-1];
      const estart=ex[i];
      const eend=(i<rp.length)?rp[i]:r.wb;
      // first wait = real IssueQ; later waits = ReplayQ (load sits in the
      // replay queue after being bounced, then re-issues)
      const wname=(i===0)?'IssueQ':'ReplayQ';
      if(istart!=null&&estart!=null&&estart>istart)segs.push([wname,istart,estart]);
      if(estart!=null&&eend!=null&&eend>estart)segs.push(['Execute',estart,eend]);
    }
    segs.push(['CommitWait',r.wb,r.cmt]);
  } else {
    segs=[['Fetch',0,r.dec],['Decode',r.dec,r.ren],['Rename',r.ren,r.isq],
          ['IssueQ',r.isq,r.fu],['Execute',r.fu,r.wb],['CommitWait',r.wb,r.cmt]];
  }
  // single-lane bar: stages are laid out left-to-right in time order. For
  // replayed loads the IssueQ/ReplayQ/Execute segments simply alternate along
  // the same lane (the issue->execute->replay sawtooth), no separate lane.
  const H=22;
  let bar=`<div style="position:relative;height:${H}px;border-radius:4px;overflow:hidden;`+
          'max-width:760px;border:1px solid #2a2f3a;background:#181c24">';
  for(const [nm,a,b] of segs){
    if(a==null||b==null||b<=a)continue;
    const col=MODCOL[nm]||'#888';
    const left=100*a/life, w=100*(b-a)/life, lbl=(w>9)?nm:'';
    bar+=`<div title="${nm}: cyc ${a}\u2013${b} (${b-a} cyc)" style="position:absolute;`+
         `left:${left.toFixed(3)}%;width:${Math.max(0.3,w).toFixed(3)}%;top:0;bottom:0;`+
         `background:${col};border-right:1px solid #0d0f14;display:flex;align-items:center;`+
         `justify-content:center;font-size:10px;color:#0d0f14;overflow:hidden;white-space:nowrap">${lbl}</div>`;
  }
  bar+=replayMarks(r,life)+'</div>';
  // list every segment in the order it happened (not aggregated by name)
  let leg='';
  for(const [nm,a,b] of segs){
    if(a==null||b==null||b<=a)continue;
    leg+=chip(MODCOL[nm]||'#888',nm,b-a);
  }
  const np=saw?` &mdash; <b>${ex.length}</b> passes (IssueQ\u2192Execute\u2192ReplayQ sawtooth)`:'';
  return `<div class="hint" style="margin:6px 0 3px">module per cycle (in time order, `+
         `lifetime ${life} cyc${np}):</div>`+
         bar+`<div style="font-size:11px;margin-top:5px;line-height:1.8">${leg}</div>`;
}
// replay events as magenta dashed verticals at the exact cycle each replay fired
function replayMarks(r,life){
  if(!r.rep||!life)return '';
  const cy=r.repcyc||[], ty=r.reptypes||'';
  let g='';
  for(let i=0;i<cy.length;i++){
    const c=cy[i]; if(c==null||c<0||c>life)continue;
    const t=RPMAP[ty[i]]||ty[i]||'?';
    g+=`<div title="replay #${i+1} @cyc ${c}: ${t}" style="position:absolute;`+
       `left:${(100*c/life).toFixed(3)}%;top:-2px;bottom:-2px;width:0;`+
       `border-left:2px dashed #ff3df0;z-index:3"></div>`;
  }
  return g;
}
// stage boundary vertical lines (shared with the module bar) so both bars line up
function stageLines(r,life){
  let g='';
  for(const b of [r.dec,r.ren,r.isq,r.fu,r.wb]){
    if(b!=null&&life>0&&b>0&&b<life)
      g+=`<div style="position:absolute;left:${(100*b/life).toFixed(3)}%;top:0;bottom:0;`+
         `width:0;border-left:1px solid #0d0f14"></div>`;
  }
  return g;
}
function chip(col,label,val){
  return `<span style="margin-right:14px;white-space:nowrap"><span style="display:inline-block;`+
         `width:11px;height:11px;background:${col};border-radius:2px;vertical-align:middle"></span> `+
         `${label}: <b>${val}</b></span>`;
}
const RPMAP={T:'TLBMiss',C:'CacheMiss',E:'Reschedule',F:'STLF',M:'MdpAddr',N:'Nuke',
             K:'CacheBlocked',B:'BankConflict',R:'RAR',W:'RAW',A:'MshrAliasFail',
             H:'HitInWriteBuffer',G:'MshrArbFail',O:'Other'};
function replayNote(r){
  if(!r.rep)return '';
  const cnt={};for(const ch of (r.reptypes||''))cnt[ch]=(cnt[ch]||0)+1;
  const parts=Object.keys(cnt).map(k=>`${RPMAP[k]||k}\u00d7${cnt[k]}`).join(', ');
  // per-replay event list with the exact cycle each one fired
  const cy=r.repcyc||[], ty=r.reptypes||'';
  let evs='';
  if(cy.length){
    const items=cy.map((c,i)=>`#${i+1} @cyc <b>${c}</b> ${RPMAP[ty[i]]||ty[i]||'?'}`).join(' &nbsp; ');
    evs=`<div style="margin-top:3px">\u2502 ${items}</div>`;
  }
  return `<div style="margin:6px 0;padding:6px 9px;border-radius:4px;background:#3a2a12;`+
    `border:1px solid #6b4a1a;color:#f0c987;font-size:12px">\u27f3 <b>replayed ${r.rep}\u00d7</b> `+
    `(${parts})${evs}</div>`;
}
// Legacy-DB fallback. New gem5 commits no longer record HoLBlocked before the
// final Execute (commit gates HoLBlocked on !needReplay()&&!inPipe()), so the
// real replay/memory reasons already sit in StallSpans and this is a no-op.
// For DBs captured before that fix, any HoLBlocked span landing before the
// final successful execution (r.fu) was speculative CanCommit waiting, not true
// head-of-line blocking: split it at the replay boundaries and label each
// sub-segment with the replay type that caused it (e.g. "Reschedule wait").
function splitReplayWaits(r,sp){
  if(!r.rep||r.fu==null)return sp;
  const rc=r.repcyc||[], rt=r.reptypes||'';
  const out=[];
  for(const s of sp){
    if(s[0]!=='HoLBlocked'||s[2]==null||s[2]>=r.fu){out.push(s);continue;}
    const a=s[2], b=s[3];
    const bnds=rc.filter(c=>c>a&&c<b).sort((x,y)=>x-y);
    const pts=[a,...bnds,b];
    for(let k=0;k<pts.length-1;k++){
      const ss=pts[k], ee=pts[k+1];
      let gi=-1;for(let j=0;j<rc.length;j++)if(rc[j]<=ss)gi=j;   // governing replay
      const tn=(gi>=0)?(RPMAP[rt[gi]]||rt[gi]||'Replay'):'Replay';
      out.push([tn+' wait',Math.max(1,ee-ss),ss,ee]);
    }
  }
  return out;
}
// After the commit-side fix, the inter-pass ReplayQ waits carry no stall reason
// (the load is bouncing in the replay queue -- not truly HoL-blocked, and not
// front-end queueing), so they would otherwise render as a misleading "not
// attributed" block. Fill each gap [replay_{i-1}, exec_i] with the replay type
// that bounced the load (e.g. "Reschedule wait"), mirroring the ReplayQ segments
// in the module bar above. These gaps never overlap the real reasons, which are
// recorded only during the brief Execute passes.
function fillReplayWaits(r,sp){
  const ex=r.execcyc||[], rp=r.repcyc||[], rt=r.reptypes||'';
  if(!r.rep||ex.length<2)return sp;
  // real recorded intervals -- carve them out of each gap so we never draw the
  // wait on top of a genuine reason (the tail of the last ReplayQ wait overlaps
  // the final pass's recorded InstNotReady/LoadL*Bound).
  const real=sp.filter(s=>s[2]!=null&&s[3]!=null).map(s=>[s[2],s[3]])
               .sort((x,y)=>x[0]-y[0]);
  const out=sp.slice();
  for(let i=1;i<ex.length;i++){
    const a=rp[i-1], b=ex[i];
    if(a==null||b==null||b<=a)continue;
    const tn=(RPMAP[rt[i-1]]||rt[i-1]||'Replay')+' wait';
    let cur=a;
    for(const [ra,rb] of real){
      if(rb<=cur||ra>=b)continue;
      if(ra>cur)out.push([tn,ra-cur,cur,ra]);
      cur=Math.max(cur,rb);
      if(cur>=b)break;
    }
    if(cur<b)out.push([tn,b-cur,cur,b]);
  }
  return out;
}
function spanDetail(r){
  let sp=fillReplayWaits(r, splitReplayWaits(r, r.spans||[]));
  const life=r.tot||0;                       // full fetch->commit lifetime (cyc)
  if(!sp.length && !life)return '<span class="hint">(this inst never stalled)</span>';
  const positioned = life>0 && sp.length && sp.every(s=>s[2]!=null && s[3]!=null);
  let bar,leg='';
  if(positioned){
    // place each span at the real cycle it happened (aligned to the module bar)
    bar='<div style="position:relative;height:20px;max-width:760px;border-radius:4px;'+
        'border:1px solid #2a2f3a;background:#222732;overflow:hidden">';
    // chronological order: sort segments by their start cycle
    const segs=sp.filter(s=>s[2]!=null&&s[3]!=null).slice().sort((x,y)=>x[2]-y[2]);
    for(const [name,cyc,s0,s1] of segs){
      const dn=feLabel(r,name,s1), col=colorFor(dn);
      const left=100*Math.max(0,s0)/life, w=Math.max(0.4,100*Math.max(1,s1-s0)/life);
      const tip=(dn!==name)
        ?`${dn}: ${cyc} cyc (cyc ${s0}\u2013${s1}) \u2014 backend `+
         `ROB/LQ/SQ-head reason inherited while still in the front `+
         `end (not this inst's own ${name})`
        :`${name}: ${cyc} cyc (cyc ${s0}\u2013${s1} from fetch)`;
      bar+=`<div title="${tip}" style="position:absolute;`+
           `left:${left.toFixed(3)}%;width:${w.toFixed(3)}%;top:0;bottom:0;background:${col}"></div>`;
    }
    bar+=stageLines(r,life)+replayMarks(r,life)+'</div>';
    // legend lists EACH segment in time order (incl. gray gaps), not aggregated
    let cur=0;
    for(const [name,cyc,s0,s1] of segs){
      if(s0>cur)leg+=chip('#222732','not attributed',s0-cur);
      const dn=feLabel(r,name,s1);
      leg+=chip(colorFor(dn),dn,Math.max(1,s1-s0));
      cur=Math.max(cur,s1);
    }
    if(life>cur)leg+=chip('#222732','not attributed',life-cur);
  } else {
    // fallback: DB without per-span ticks -> packed (not time-aligned)
    const W=Math.max(life,tot,1);
    bar='<div style="display:flex;height:20px;border-radius:4px;overflow:hidden;'+
        'max-width:760px;border:1px solid #2a2f3a">';
    for(const [name,cyc] of sp){const col=colorFor(name);
      bar+=`<div title="${name}: ${cyc} cyc" style="width:${(100*cyc/W).toFixed(3)}%;background:${col}"></div>`;
      leg+=chip(col,name,cyc);}
    const rest=Math.max(0,life-tot);
    if(rest>0){bar+=`<div style="width:${(100*rest/W).toFixed(3)}%;background:#222732"></div>`;
      leg+=chip('#222732','not attributed',rest);}
    bar+='</div>';
  }
  return replayNote(r)+moduleBar(r)+
         `<div class="hint" style="margin:8px 0 3px">stall reason per cycle (in time order):</div>`+
         bar+`<div style="font-size:11px;margin-top:5px;line-height:1.8">${leg}</div>`;
}
const expanded=new Set();
function render(){
  updateHeaders();
  const q=document.getElementById('q').value.toLowerCase();
  const realOnly=document.getElementById('ronly').value==='real';
  let rows=D.table.filter(r=>{
    if(realOnly&&r.reason==='NoStall')return false;
    if(!q)return true;
    return (r.pc+' '+r.asm+' '+r.reason+' '+r.raw).toLowerCase().includes(q);});
  rows.sort((a,b)=>{let x=a[sortKey],y=b[sortKey];
    if(typeof x==='string')return sortDir*(x<y?-1:x>y?1:0);
    return sortDir*((x??-1)-(y??-1));});
  const tb=document.getElementById('tb');tb.innerHTML='';
  const frag=document.createDocumentFragment();
  const CAP=50000;                 // DOM safety cap (browser would freeze beyond)
  const shown=Math.min(rows.length,CAP);
  for(let i=0;i<shown;i++){const r=rows[i];const tr=document.createElement('tr');
    tr.style.cursor='pointer';
    const ns=(r.spans||[]).length;
    tr.onclick=()=>{if(expanded.has(r.id))expanded.delete(r.id);else expanded.add(r.id);render();};
    COLS.forEach(([k])=>{const td=document.createElement('td');
      let v=r[k]==null?'':r[k];
      if(k==='id')v=(expanded.has(r.id)?'\u25BC ':(ns?'\u25B6 ':'  '))+(r.rep?'\u27f3 ':'')+v;
      td.textContent=v;
      if(k==='pc'||k==='asm'||k==='reason'||k==='raw'||k==='id')td.className='l';tr.appendChild(td);});
    frag.appendChild(tr);
    if(expanded.has(r.id)){const dtr=document.createElement('tr');
      const td=document.createElement('td');td.colSpan=COLS.length;td.className='l';
      td.style.background='#10131a';td.innerHTML=spanDetail(r);
      dtr.appendChild(td);frag.appendChild(dtr);}}
  tb.appendChild(frag);
  document.getElementById('cnt').textContent=
    `${rows.length} rows`+(rows.length>CAP?` (showing first ${CAP})`:'');
}
render();
</script>
</body></html>"""


def _base_path(out, sqldb):
    """Derive the shared output base (without _overview/_detail/.html suffix)."""
    if out:
        base = out
        for suf in ('.html', '.htm'):
            if base.lower().endswith(suf):
                base = base[:-len(suf)]
        for suf in ('_overview', '_detail', '_report'):
            if base.endswith(suf):
                base = base[:-len(suf)]
        return base
    return os.path.splitext(sqldb)[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sqldb")
    ap.add_argument("-o", "--out", default=None,
                    help="output base path; writes <base>_overview.html and/or "
                         "<base>_detail.html (default: next to the db)")
    ap.add_argument("-p", "--period", type=int, default=333,
                    help="ticks per cycle (3GHz=333, 2GHz=500)")
    ap.add_argument("--part", choices=["overview", "detail", "both"],
                    default="both", help="which report(s) to write (default both)")
    ap.add_argument("--max-rows", type=int, default=4000,
                    help="per-inst table rows embedded, top-N (default 4000; "
                         "0 = ALL insts)")
    ap.add_argument("--rank", choices=["lat", "stall"], default="stall",
                    help="metric the embedded top-N table keeps (default "
                         "'stall'): 'stall' = own stall cycles (excl. HoL), "
                         "'lat' = fetch->commit latency (includes passive "
                         "HoL/in-flight time, so not pure stall)")
    ap.add_argument("--occ-buckets", type=int, default=1000,
                    help="number of time buckets for the occupancy chart")
    ap.add_argument("--detail-start", type=int, default=None,
                    help="start cycle for per-cycle module-instruction detail "
                         "(default: window around peak InFlight cycle)")
    ap.add_argument("--detail-end", type=int, default=None,
                    help="end cycle for the module-instruction detail")
    ap.add_argument("--detail-window", type=int, default=2000,
                    help="width (cycles) of the auto detail window (default 2000)")
    ap.add_argument("--detail-all", action="store_true",
                    help="embed the WHOLE run in the module-instruction detail "
                         "so the cycle slider covers every cycle (large file!)")
    ap.add_argument("--cycacct", action="store_true",
                    help="show the cycle-accounting banner (CPU work vs "
                         "device/MMIO I/O wait); hidden by default")
    ap.add_argument("--viewer", action="store_true",
                    help="also emit <base>_viewer.html: a static, db-agnostic "
                         "viewer that loads the WHOLE db on demand (sql.js in "
                         "the browser, no embedded data, no size limit / lag). "
                         "Open it and pick the lifetime.db file.")
    ap.add_argument("--io-method", choices=["addr", "lat"], default="addr",
                    help="viewer's default device-I/O detection: 'addr' "
                         "(physical addr below --dram-base => MMIO; robust) or "
                         "'lat' (FU->WriteVal latency heuristic)")
    ap.add_argument("--dram-base", default="0x80000000",
                    help="physical DRAM base address; loads below it are treated "
                         "as device/MMIO by the viewer's address method "
                         "(default 0x80000000)")
    args = ap.parse_args()
    if not os.path.isfile(args.sqldb):
        sys.exit("error: db not found: " + args.sqldb)

    base = _base_path(args.out, args.sqldb)
    data = build(args.sqldb, args.period, args.max_rows, args.occ_buckets,
                 args.detail_start, args.detail_end, args.detail_window,
                 args.detail_all, args.rank)

    if args.part in ("overview", "both"):
        keys = ('db', 'period', 'ninsts', 'span', 'stall',
                'stage_lat', 'lat_x', 'lat_y', 'occ_x', 'occ', 'occ_labels',
                'top_blk') + (('cycacct',) if args.cycacct else ())
        od = {k: data[k] for k in keys}
        html = (HTML_OVERVIEW.replace("__STYLE__", _STYLE)
                             .replace("__CYCACCT_JS__", _CYCACCT_JS)
                             .replace("__DB__", data["db"])
                             .replace("__DATA__", json.dumps(od)))
        p = base + "_overview.html"
        with open(p, "w") as f:
            f.write(html)
        print(f"wrote {p}  ({os.path.getsize(p)//1024} KB; charts)")

    if args.part in ("detail", "both"):
        keys = ('db', 'period', 'ninsts', 'span', 'table',
                'max_rows', 'ntable', 'detail', 'detail2', 'rank') + \
               (('cycacct',) if args.cycacct else ())
        dd = {k: data[k] for k in keys}
        rank_lbl = "stall cycles" if args.rank == "stall" else "latency"
        sort_key = "scyc" if args.rank == "stall" else "tot"
        html = (HTML_DETAIL.replace("__STYLE__", _STYLE)
                           .replace("__CYCACCT_JS__", _CYCACCT_JS)
                           .replace("__DB__", data["db"])
                           .replace("__MAXROWS__", str(data["max_rows"]))
                           .replace("__RANK__", rank_lbl)
                           .replace("__SORTKEY__", sort_key)
                           .replace("__DATA__", json.dumps(dd)))
        p = base + "_detail.html"
        with open(p, "w") as f:
            f.write(html)
        print(f"wrote {p}  ({os.path.getsize(p)//1024} KB; "
              f"{data['ntable']} table rows, {len(data['detail']['insts'])} "
              f"detail insts)")

    if args.viewer:
        # static, db-agnostic viewer: load the WHOLE db on demand (sql.js in the
        # browser), no embedded data, no size limit. We copy it next to the
        # output and bake the chosen CLI options into its DEFAULTS (the viewer
        # exposes them as editable controls in its header).
        import re
        src = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "perfcct_viewer.html")
        with open(src) as f:
            vhtml = f.read()

        def _js(v):
            if v is None:
                return "null"
            if v is True:
                return "true"
            if v is False:
                return "false"
            if isinstance(v, str):
                return "'%s'" % v
            return str(v)

        defaults = {
            "period": args.period,
            "excludeIO": True,                 # report always discounts device I/O
            "ioMethod": args.io_method,        # 'addr' (PAddr<DRAM base) or 'lat'
            "dramBase": args.dram_base,        # MMIO is below this physical addr
            "ioLat": 200,                      # latency fallback (matches IO_TH)
            "window": args.detail_window,
            "sort": "id",                      # table3 initial sort column
            "sortDir": 1,                      # 1 = asc, -1 = desc
            "realOnly": False,
            "pageSize": 100,                   # insts loaded per "load more"
            "startCyc": None if args.detail_all else args.detail_start,
            "endCyc": None if args.detail_all else args.detail_end,
        }
        js = ("const DEFAULTS={"
              + ",".join("%s:%s" % (k, _js(v)) for k, v in defaults.items())
              + "};")
        vhtml, n = re.subn(r"const DEFAULTS=\{.*?\};", js, vhtml,
                           count=1, flags=re.S)
        dst = base + "_viewer.html"
        with open(dst, "w") as f:
            f.write(vhtml)
        note = "" if n else "  (warning: DEFAULTS block not found; defaults unchanged)"
        print(f"wrote {dst}  (open it and pick "
              f"{os.path.basename(args.sqldb)}){note}")


if __name__ == "__main__":
    main()
