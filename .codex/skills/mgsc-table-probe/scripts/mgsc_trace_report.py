#!/usr/bin/env python3
"""Aggregate MGSC bp.db traces into branch- and run-level reports.

The main goal is to bridge the gap between raw `MGSCTRACE` rows and
actionable SC observability questions:

1. Is SC fixing TAGE or harming it?
2. When SC is bypassed, was that a missed opportunity or a safe bypass?
3. Which SC tables are decisive for a branch?
4. Is the bottleneck likely in table capacity/expression or threshold gating?
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import sqlite3
from collections import defaultdict
from contextlib import closing
from pathlib import Path
from typing import Dict, Iterable, Iterator, List, Tuple


TABLE_COLS = {
    "bw": "bwPercsum",
    "l": "lPercsum",
    "i": "iPercsum",
    "g": "gPercsum",
    "p": "pPercsum",
    "bias": "biasPercsum",
}

SIG_COLS = {
    "bw": "bwIndexSig",
    "l": "lIndexSig",
    "i": "iIndexSig",
    "g": "gIndexSig",
    "p": "pIndexSig",
    "bias": "biasIndexSig",
}


def parse_top_csv(path: Path) -> Dict[int, Dict[str, float]]:
    if not path.exists():
        return {}
    out: Dict[int, Dict[str, float]] = {}
    with path.open(newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            try:
                pc = int((row.get("pc") or "").strip(), 16)
            except ValueError:
                continue
            out[pc] = {
                "mispredicts": float(row.get("mispredicts", 0) or 0),
                "total": float(row.get("total", 0) or 0),
                "misPermil": float(row.get("misPermil", 0) or 0),
                "dirMiss": float(row.get("dirMiss", 0) or 0),
                "tgtMiss": float(row.get("tgtMiss", 0) or 0),
                "noPredMiss": float(row.get("noPredMiss", 0) or 0),
            }
    return out


def sign_taken(x: float) -> int:
    return 1 if x >= 0 else 0


def effective_gate(row: sqlite3.Row) -> float:
    if "effectiveGate" in row.keys():
        return float(row["effectiveGate"])
    total_thres = float(row["totalThres"])
    if row["tageConfHigh"]:
        return total_thres / 2.0
    if row["tageConfMid"]:
        return total_thres / 4.0
    return total_thres / 8.0


def safe_ratio(numer: float, denom: float) -> float:
    return numer / denom if denom else 0.0


def categorize_row(row: sqlite3.Row) -> Dict[str, object]:
    actual = int(row["actualTaken"])
    tage = int(row["tagePred"])
    sc = int(row["scPred"])
    use_sc = int(row["useSc"])
    gate = effective_gate(row)
    total_sum = float(row["totalSum"])
    margin = abs(total_sum) - gate

    tage_correct = tage == actual
    sc_correct = sc == actual

    if use_sc:
        if not tage_correct and sc_correct:
            category = "fix_use"
        elif tage_correct and not sc_correct:
            category = "hurt_use"
        elif tage_correct and sc_correct:
            category = "agree_correct_use"
        else:
            category = "agree_wrong_use"
    else:
        if not tage_correct and sc_correct:
            category = "bypass_fix"
        elif tage_correct and not sc_correct:
            category = "bypass_avoid_hurt"
        elif tage_correct and sc_correct:
            category = "bypass_agree_correct"
        else:
            category = "bypass_agree_wrong"

    return {
        "actual": actual,
        "tage": tage,
        "sc": sc,
        "use_sc": use_sc,
        "tage_correct": tage_correct,
        "sc_correct": sc_correct,
        "gate": gate,
        "margin": margin,
        "category": category,
    }


def iter_trace_rows(db_path: Path) -> Iterator[sqlite3.Row]:
    with closing(sqlite3.connect(str(db_path))) as con:
        con.row_factory = sqlite3.Row
        cur = con.cursor()
        cur.execute("PRAGMA temp_store=MEMORY")
        for row in cur.execute("SELECT * FROM MGSCTRACE"):
            yield row


def aggregate_trace(
    rows: Iterable[sqlite3.Row], top_by_pc: Dict[int, Dict[str, float]]
) -> Tuple[Dict[str, float], List[Dict[str, object]]]:
    overall = defaultdict(float)
    by_pc: Dict[int, Dict[str, object]] = {}
    context_maps: Dict[int, Dict[str, Dict[int, List[float]]]] = defaultdict(
        lambda: {short: defaultdict(lambda: [0.0, 0.0]) for short in TABLE_COLS}
    )

    for row in rows:
        pc = int(row["branchPC"])
        cat = categorize_row(row)
        table_vals = {short: float(row[col]) for short, col in TABLE_COLS.items()}
        sc_sign = sign_taken(float(row["totalSum"]))

        overall["rows"] += 1
        overall[cat["category"]] += 1
        overall["use_sc"] += float(cat["use_sc"])
        overall["sum_margin"] += float(cat["margin"])
        overall["sum_gate"] += float(cat["gate"])
        if float(cat["margin"]) < 0:
            overall["negative_margin_rows"] += 1

        if pc not in by_pc:
            top = top_by_pc.get(pc, {})
            by_pc[pc] = {
                "branchPC": pc,
                "branchPC_hex": hex(pc),
                "rows": 0.0,
                "use_sc": 0.0,
                "fix_use": 0.0,
                "hurt_use": 0.0,
                "bypass_fix": 0.0,
                "bypass_avoid_hurt": 0.0,
                "agree_correct_use": 0.0,
                "agree_wrong_use": 0.0,
                "bypass_agree_correct": 0.0,
                "bypass_agree_wrong": 0.0,
                "sum_margin": 0.0,
                "sum_gate": 0.0,
                "sum_total_sum": 0.0,
                "negative_margin_rows": 0.0,
                "top_mispredicts": top.get("mispredicts", 0.0),
                "top_total": top.get("total", 0.0),
                "top_misPermil": top.get("misPermil", 0.0),
                "top_dirMiss": top.get("dirMiss", 0.0),
                "top_tgtMiss": top.get("tgtMiss", 0.0),
                "top_noPredMiss": top.get("noPredMiss", 0.0),
            }
            if "bbStart" in row.keys():
                by_pc[pc]["first_bbStart"] = int(row["bbStart"])
            for short in TABLE_COLS:
                by_pc[pc][f"{short}_decisive"] = 0.0
                by_pc[pc][f"{short}_agree_fix"] = 0.0
                by_pc[pc][f"{short}_agree_hurt"] = 0.0
                by_pc[pc][f"{short}_remove_lost_fix"] = 0.0
                by_pc[pc][f"{short}_remove_saved_hurt"] = 0.0

        ent = by_pc[pc]
        ent["rows"] += 1
        ent["use_sc"] += float(cat["use_sc"])
        ent[cat["category"]] += 1
        ent["sum_margin"] += float(cat["margin"])
        ent["sum_gate"] += float(cat["gate"])
        ent["sum_total_sum"] += float(row["totalSum"])
        if float(cat["margin"]) < 0:
            ent["negative_margin_rows"] += 1

        for short, val in table_vals.items():
            without_table_sum = float(row["totalSum"]) - val
            without_table_sign = sign_taken(without_table_sum)
            if without_table_sign != sc_sign:
                ent[f"{short}_decisive"] += 1
            if not cat["tage_correct"] and cat["sc_correct"] and sign_taken(val) == cat["actual"]:
                ent[f"{short}_agree_fix"] += 1
            if cat["tage_correct"] and not cat["sc_correct"] and sign_taken(val) != cat["actual"]:
                ent[f"{short}_agree_hurt"] += 1
            if cat["use_sc"] and cat["category"] == "fix_use" and without_table_sign != cat["actual"]:
                ent[f"{short}_remove_lost_fix"] += 1
            if cat["use_sc"] and cat["category"] == "hurt_use" and without_table_sign == cat["actual"]:
                ent[f"{short}_remove_saved_hurt"] += 1

            sig_col = SIG_COLS[short]
            if sig_col in row.keys():
                sig = int(row[sig_col])
                context_maps[pc][short][sig][0] += 1.0
                context_maps[pc][short][sig][1] += float(cat["actual"])

    overall["avg_margin"] = safe_ratio(overall["sum_margin"], overall["rows"])
    overall["avg_gate"] = safe_ratio(overall["sum_gate"], overall["rows"])
    overall["net_use"] = overall["fix_use"] - overall["hurt_use"]
    overall["bypass_net"] = overall["bypass_fix"] - overall["bypass_avoid_hurt"]
    overall["negative_margin_ratio"] = safe_ratio(overall["negative_margin_rows"], overall["rows"])

    branch_rows: List[Dict[str, object]] = []
    for pc, ent in by_pc.items():
        rows_cnt = float(ent["rows"])
        use_sc = float(ent["use_sc"])
        fix_use = float(ent["fix_use"])
        hurt_use = float(ent["hurt_use"])
        bypass_fix = float(ent["bypass_fix"])
        bypass_avoid_hurt = float(ent["bypass_avoid_hurt"])
        branch = dict(ent)
        branch["avg_margin"] = safe_ratio(float(ent["sum_margin"]), rows_cnt)
        branch["avg_gate"] = safe_ratio(float(ent["sum_gate"]), rows_cnt)
        branch["avg_total_sum"] = safe_ratio(float(ent["sum_total_sum"]), rows_cnt)
        branch["use_ratio"] = safe_ratio(use_sc, rows_cnt)
        branch["net_use"] = fix_use - hurt_use
        branch["bypass_net"] = bypass_fix - bypass_avoid_hurt
        branch["negative_margin_ratio"] = safe_ratio(float(ent["negative_margin_rows"]), rows_cnt)
        for short in TABLE_COLS:
            branch[f"{short}_decisive_ratio"] = safe_ratio(float(ent[f"{short}_decisive"]), rows_cnt)
            branch[f"{short}_agree_fix_ratio"] = safe_ratio(float(ent[f"{short}_agree_fix"]), fix_use + bypass_fix)
            branch[f"{short}_agree_hurt_ratio"] = safe_ratio(float(ent[f"{short}_agree_hurt"]), hurt_use)
            branch[f"{short}_remove_lost_fix_ratio"] = safe_ratio(float(ent[f"{short}_remove_lost_fix"]), fix_use)
            branch[f"{short}_remove_saved_hurt_ratio"] = safe_ratio(float(ent[f"{short}_remove_saved_hurt"]), hurt_use)

            buckets = context_maps[pc][short]
            if buckets:
                total_bucket_rows = sum(v[0] for v in buckets.values())
                branch[f"{short}_context_strength"] = safe_ratio(
                    sum(abs((2.0 * taken) - cnt) for cnt, taken in buckets.values()),
                    total_bucket_rows,
                )
                top_sig, top_vals = max(buckets.items(), key=lambda kv: kv[1][0])
                branch[f"{short}_top_bucket_sig"] = hex(top_sig)
                branch[f"{short}_top_bucket_share"] = safe_ratio(top_vals[0], total_bucket_rows)
                branch[f"{short}_top_bucket_taken_rate"] = safe_ratio(top_vals[1], top_vals[0])
            else:
                branch[f"{short}_context_strength"] = 0.0
                branch[f"{short}_top_bucket_sig"] = ""
                branch[f"{short}_top_bucket_share"] = 0.0
                branch[f"{short}_top_bucket_taken_rate"] = 0.0

        best_table = max(
            TABLE_COLS,
            key=lambda short: (
                float(branch[f"{short}_agree_fix_ratio"]),
                float(branch[f"{short}_decisive_ratio"]),
            ),
        )
        branch["dominant_table"] = best_table
        branch["best_context_table"] = max(TABLE_COLS, key=lambda short: float(branch[f"{short}_context_strength"]))
        branch_rows.append(branch)

    branch_rows.sort(
        key=lambda r: (
            float(r["net_use"]),
            float(r["bypass_fix"]),
            float(r["top_mispredicts"]),
            -float(r["hurt_use"]),
        ),
        reverse=True,
    )
    return dict(overall), branch_rows


def write_csv(path: Path, rows: List[Dict[str, object]]) -> None:
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    fields = list(rows[0].keys())
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def build_report_md(summary: Dict[str, float], rows: List[Dict[str, object]], limit: int) -> str:
    lines = []
    lines.append("# MGSC Trace Report")
    lines.append("")
    lines.append("## Overall")
    lines.append("")
    lines.append(f"- rows: {summary.get('rows', 0):.0f}")
    lines.append(f"- use_sc: {summary.get('use_sc', 0):.0f}")
    lines.append(f"- fix_use: {summary.get('fix_use', 0):.0f}")
    lines.append(f"- hurt_use: {summary.get('hurt_use', 0):.0f}")
    lines.append(f"- net_use: {summary.get('net_use', 0):+.0f}")
    lines.append(f"- bypass_fix: {summary.get('bypass_fix', 0):.0f}")
    lines.append(f"- bypass_avoid_hurt: {summary.get('bypass_avoid_hurt', 0):.0f}")
    lines.append(f"- bypass_net: {summary.get('bypass_net', 0):+.0f}")
    lines.append(f"- avg_margin: {summary.get('avg_margin', 0):+.3f}")
    lines.append(f"- negative_margin_ratio: {summary.get('negative_margin_ratio', 0):.2%}")
    lines.append("")
    lines.append("## Top Branches")
    lines.append("")
    lines.append("| branchPC | rows | topMis | use | fix | hurt | bypass_fix | dom_table | ctx_table | avg_margin |")
    lines.append("| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- | --- | ---: |")
    for row in rows[:limit]:
        lines.append(
            f"| {hex(int(row['branchPC']))} | {float(row['rows']):.0f} | {float(row['top_mispredicts']):.0f} | "
            f"{float(row['use_sc']):.0f} | {float(row['fix_use']):.0f} | {float(row['hurt_use']):.0f} | "
            f"{float(row['bypass_fix']):.0f} | {row['dominant_table']} | {row['best_context_table']} | {float(row['avg_margin']):+.2f} |"
        )
    lines.append("")
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description="Aggregate MGSC MGSCTRACE rows into branch-level reports.")
    parser.add_argument("--db", required=True, type=Path, help="Path to bp.db containing MGSCTRACE")
    parser.add_argument("--top-csv", type=Path, default=None, help="Optional topMispredictsByBranch.csv")
    parser.add_argument("--outdir", type=Path, required=True, help="Directory for output files")
    parser.add_argument("--limit", type=int, default=20, help="How many top branches to show in report.md")
    args = parser.parse_args()

    top_by_pc = parse_top_csv(args.top_csv) if args.top_csv else {}
    summary, branch_rows = aggregate_trace(iter_trace_rows(args.db), top_by_pc)
    if not summary.get("rows", 0):
        raise SystemExit(f"no MGSCTRACE rows found in {args.db}")

    args.outdir.mkdir(parents=True, exist_ok=True)
    (args.outdir / "summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True), encoding="utf-8")
    write_csv(args.outdir / "branch_summary.csv", branch_rows)
    (args.outdir / "report.md").write_text(build_report_md(summary, branch_rows, args.limit), encoding="utf-8")

    print(f"rows={summary['rows']:.0f} use_sc={summary['use_sc']:.0f} fix_use={summary['fix_use']:.0f} "
          f"hurt_use={summary['hurt_use']:.0f} bypass_fix={summary['bypass_fix']:.0f}")
    print(args.outdir / "report.md")


if __name__ == "__main__":
    main()
