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
    c.execute("SELECT " + ",".join(STAGES) +
              ",StallReason,StallCycles,SecondaryReason,StallSpans,DisAsm,PC,ID "
              "FROM LifeTimeCommitTrace WHERE AtFetch>0")
    rows = c.fetchall()
    P = period
    ninsts = len(rows)
    # column layout: STAGES..., reason(R), scyc(R+1), secondary(R+2),
    #                spans(R+3), disasm(R+4), pc(-2), id(-1)
    R = len(STAGES)
    DIS = R + 4              # disasm column index

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

    # --- stall histogram ---
    # HoLBlocked is passive waiting (like NoStall: e.g. a load auto-waits a few
    # cycles before the L1 lookup), so ranking it against real, actionable stall
    # reasons is meaningless. We collapse it into the inst's *effective* reason:
    #   effReason = SecondaryReason when dominant==HoLBlocked, else StallReason
    # and count by #insts (NOT stall cycles: a victim's StallCycles is mostly HoL
    # wait time, which would wrongly inflate the secondary reason). NoStall is
    # dropped from the chart since it is not an actionable stall.
    from collections import Counter, defaultdict
    eff_n = Counter()        # effective (real) reason, by inst count
    sec_n = Counter()        # secondary reason among HoLBlocked-dominant insts

    def eff_reason(r):
        return r[R + 2] if r[R] == 'HoLBlocked' else r[R]

    for r in rows:
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
        for r in rows:
            if r[a] > 0 and r[b] >= r[a]:
                tot += (r[b] - r[a]); n += 1
        stage_lat.append((STAGES[i - 1] + "->" + STAGES[i],
                          (tot / n / P) if n else 0.0))

    # --- latency distribution (fetch->commit cycles) ---
    lats = [(r[idx['AtCommit']] - r[idx['AtFetch']]) // P for r in rows]
    lo, hi = min(lats), max(lats)
    nb = 60
    width = max(1, (hi - lo) // nb + 1)
    hist = Counter()
    for v in lats:
        hist[(v - lo) // width] += 1
    lat_x = [lo + k * width for k in range(((hi - lo) // width) + 1)]
    lat_y = [hist.get(k, 0) for k in range(len(lat_x))]

    # --- occupancy over time (downsampled) ---
    fmin = min(r[idx['AtFetch']] for r in rows) // P
    cmax = max(r[idx['AtCommit']] for r in rows) // P
    span = cmax - fmin + 1
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

    # --- top blocker PCs (excluding HoLBlocked / NoStall) ---
    blk_cyc = defaultdict(int)
    blk_reason = {}
    blk_asm = {}
    for r in rows:
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
        table.append({
            "id": r[-1],
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
    # to detail_window cycles centred on the peak InFlight cycle (the busiest
    # moment), or can be set explicitly via --detail-start/--detail-end.
    det_mods = [m for m in MODULES if m[0] != 'InFlight']
    if detail_all:
        ds, de = fmin, cmax
    elif detail_start is not None and detail_end is not None:
        ds, de = detail_start, detail_end
    else:
        half = detail_window // 2
        ds = max(fmin, peak_cyc - half)
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

    return {
        "db": os.path.basename(db),
        "period": P,
        "ninsts": ninsts,
        "span": span,
        "detail": {"start": ds, "end": de,
                   "peak_cyc": peak_cyc, "peak_val": peak_val,
                   "modules": [m[0] for m in det_mods],
                   "pc": pctab, "asm": asmtab, "insts": det_insts},
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
 input,select{background:#1c2129;color:#e6e6e6;
 border:1px solid #39414f;border-radius:6px;padding:6px 8px;font-size:13px}
 table{border-collapse:collapse;width:100%;font-size:12px;margin-top:8px}
 th,td{border:1px solid #2a2f3a;padding:3px 6px;text-align:right;white-space:nowrap}
 th{cursor:pointer;position:sticky;top:0;background:#1c2129}
 td.l,th.l{text-align:left}
 tr:nth-child(even){background:#13161c}
 .wrap{max-height:560px;overflow:auto;border:1px solid #2a2f3a;border-radius:8px}
 .hint{color:#9aa;font-size:12px}
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

// ---- per-cycle module-instruction detail ----
const DET=D.detail;
const MODCOL={Fetch:'#3498db',Decode:'#1abc9c',Rename:'#2ecc71',IssueQ:'#e74c3c',
              Execute:'#e67e22',CommitWait:'#9b59b6'};
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
            `<span class="hint">${DET.pc[it[1]]}</span> `+
            `${DET.asm[it[2]]}</div>`;
    if(!list.length)html+='<div class="hint">(empty)</div>';
    html+='</div>';box.innerHTML=html;g.appendChild(box);
  });
  document.getElementById('detcnt').textContent=`cycle ${c}: ${tot} insts in flight`;
}
dcyc.oninput=()=>{drng.value=dcyc.value;renderDet();};
drng.oninput=()=>{dcyc.value=drng.value;renderDet();};
renderDet();

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
function colorFor(name){
  if(name==='HoLBlocked')return '#555a66';
  if(name==='NoStall')return '#2e7d32';
  let h=0;for(let i=0;i<name.length;i++)h=(h*31+name.charCodeAt(i))>>>0;
  return `hsl(${h%360},58%,52%)`;
}
// expandable per-inst stall timeline (the full span sequence incl. HoLBlocked)
// module-residency bar: split the full lifetime into pipeline stages, with
// vertical separators and an inline label per stage (Fetch/Decode/.../CommitWait).
function moduleBar(r){
  const life=r.tot||0;
  if(!life)return '';
  const stages=[['Fetch',0,r.dec],['Decode',r.dec,r.ren],['Rename',r.ren,r.isq],
                ['IssueQ',r.isq,r.fu],['Execute',r.fu,r.wb],['CommitWait',r.wb,r.cmt]];
  let bar='<div style="display:flex;height:22px;border-radius:4px;overflow:hidden;'+
          'max-width:760px;border:1px solid #2a2f3a">';
  let leg='';
  for(const [nm,a,b] of stages){
    const col=MODCOL[nm]||'#888';
    const dur=(a!=null&&b!=null)?(b-a):0;
    leg+=`<span style="margin-right:14px;white-space:nowrap">`+
         `<span style="display:inline-block;width:11px;height:11px;background:${col};`+
         `border-radius:2px;vertical-align:middle"></span> ${nm}: <b>${dur}</b></span>`;
    if(a==null||b==null||b<=a)continue;       // zero-width stage: legend only
    const w=100*dur/life, lbl=(w>9)?`${nm} ${dur}`:(w>4?`${dur}`:'');
    bar+=`<div title="${nm}: ${dur} cyc" style="width:${w.toFixed(3)}%;background:${col};`+
         `border-right:2px solid #0d0f14;display:flex;align-items:center;justify-content:center;`+
         `font-size:10px;color:#0d0f14;overflow:hidden;white-space:nowrap">${lbl}</div>`;
  }
  bar+='</div>';
  return `<div class="hint" style="margin:6px 0 3px">time spent in each module `+
         `(left\u2192right = full lifetime ${life} cyc; vertical lines separate the stages):</div>`+
         bar+`<div style="font-size:11px;margin-top:5px;line-height:1.8">${leg}</div>`;
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
function spanDetail(r){
  const sp=r.spans||[];
  const life=r.tot||0;                       // full fetch->commit lifetime (cyc)
  const tot=sp.reduce((a,s)=>a+s[1],0);      // stall cycles attributed to THIS inst
  if(!sp.length && !life)return '<span class="hint">(this inst never stalled)</span>';
  const positioned = life>0 && sp.length && sp.every(s=>s[2]!=null && s[3]!=null);
  let bar,leg='';
  if(positioned){
    // place each span at the real cycle it happened (aligned to the module bar)
    bar='<div style="position:relative;height:20px;max-width:760px;border-radius:4px;'+
        'border:1px solid #2a2f3a;background:#222732;overflow:hidden">';
    for(const [name,cyc,s0,s1] of sp){
      const col=colorFor(name);
      const left=100*Math.max(0,s0)/life, w=Math.max(0.4,100*Math.max(1,s1-s0)/life);
      bar+=`<div title="${name}: ${cyc} cyc (cyc ${s0}\u2013${s1} from fetch)" style="position:absolute;`+
           `left:${left.toFixed(3)}%;width:${w.toFixed(3)}%;top:0;bottom:0;background:${col}"></div>`;
      leg+=chip(col,name,cyc);
    }
    bar+=stageLines(r,life)+'</div>';
    leg+=chip('#222732','not attributed (in-flight, no reason this cyc)',Math.max(0,life-tot));
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
  const fe=(r.ren!=null)?` Of the ${life}-cyc lifetime, fetch\u2192rename alone was ${r.ren} cyc.`:'';
  return moduleBar(r)+
         `<div class="hint" style="margin:8px 0 3px">stall reasons placed at the actual cycle they occurred `+
         `(same lifetime scale &amp; stage gridlines as the bar above &mdash; the two align in time). `+
         `Colored = <b>${tot}</b> stall cyc attributed to this inst (incl. HoLBlocked); `+
         `gray = in-flight but no stall reason recorded that cycle `+
         `(mostly front-end queueing, whose stall is charged to the older `+
         `blocking inst &mdash; so it is <i>not</i> the same as `+
         `&quot;no stall&quot;).${fe}</div>`+
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
      if(k==='id')v=(expanded.has(r.id)?'\u25BC ':(ns?'\u25B6 ':'  '))+v;
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
    args = ap.parse_args()
    if not os.path.isfile(args.sqldb):
        sys.exit("error: db not found: " + args.sqldb)

    base = _base_path(args.out, args.sqldb)
    data = build(args.sqldb, args.period, args.max_rows, args.occ_buckets,
                 args.detail_start, args.detail_end, args.detail_window,
                 args.detail_all, args.rank)

    if args.part in ("overview", "both"):
        keys = ('db', 'period', 'ninsts', 'span', 'stall', 'stage_lat',
                'lat_x', 'lat_y', 'occ_x', 'occ', 'occ_labels', 'top_blk')
        od = {k: data[k] for k in keys}
        html = (HTML_OVERVIEW.replace("__STYLE__", _STYLE)
                             .replace("__DB__", data["db"])
                             .replace("__DATA__", json.dumps(od)))
        p = base + "_overview.html"
        with open(p, "w") as f:
            f.write(html)
        print(f"wrote {p}  ({os.path.getsize(p)//1024} KB; charts)")

    if args.part in ("detail", "both"):
        keys = ('db', 'period', 'ninsts', 'span', 'table', 'max_rows',
                'ntable', 'detail', 'rank')
        dd = {k: data[k] for k in keys}
        rank_lbl = "stall cycles" if args.rank == "stall" else "latency"
        sort_key = "scyc" if args.rank == "stall" else "tot"
        html = (HTML_DETAIL.replace("__STYLE__", _STYLE)
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


if __name__ == "__main__":
    main()
