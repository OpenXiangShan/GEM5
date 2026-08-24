#!/usr/bin/env python3
"""Probe SC table effectiveness on mgsc_test workloads.

This script helps answer:
1) Which existing micro-tests are sensitive to SC (vs SC off)?
2) For a target table (e.g., G / IMLI), can that table alone improve mispredicts?
3) For improved branches, does MGSCTRACE indicate SC is fixing TAGE mistakes?

Typical usage:
  python3 .agents/skills/mgsc-table-probe/scripts/mgsc_table_probe.py \
    --outdir debug/sc_table_probe \
    --profiles off,l_only,g_only,i_only,full \
    --max-workers 4
"""

from __future__ import annotations

import argparse
import csv
import dataclasses
import json
import shutil
import sqlite3
import subprocess
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_GEM5 = REPO_ROOT / "build" / "RISCV" / "gem5.opt"
DEFAULT_CONFIG = REPO_ROOT / "configs" / "example" / "kmhv3.py"
DEFAULT_CPT_DIR = Path("/nfs/home/yanyue/tools/nexus-am/tests/frontendtest/mgsc_test/build")
DEFAULT_SRC_DIR = Path("/nfs/home/yanyue/tools/nexus-am/tests/frontendtest/mgsc_test/tests")

TOP_CSV = "topMispredictsByBranch.csv"
STATS_TXT = "stats.txt"
BP_DB = "bp.db"

TABLE_COLS = {
    "bw": "bwPercsum",
    "l": "lPercsum",
    "i": "iPercsum",
    "g": "gPercsum",
    "p": "pPercsum",
    "bias": "biasPercsum",
}


@dataclasses.dataclass(frozen=True)
class Profile:
    name: str
    params: Tuple[str, ...]
    focus_table: Optional[str]
    enable_db: bool = True


@dataclasses.dataclass
class Case:
    name: str
    bin_path: Path
    disasm_path: Optional[Path]
    src_path: Optional[Path]


@dataclasses.dataclass
class RunResult:
    case: Case
    profile: Profile
    run_dir: Path
    ok: bool
    cmd: List[str]
    stats: Dict[str, float]
    top: Dict[int, Dict[str, float]]
    db_overall: Dict[str, float]
    db_by_pc: Dict[int, Dict[str, float]]
    error: str = ""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="SC table probe harness")
    parser.add_argument("--gem5-bin", default=str(DEFAULT_GEM5))
    parser.add_argument("--config", default=str(DEFAULT_CONFIG))
    parser.add_argument("--cpt-dir", default=str(DEFAULT_CPT_DIR))
    parser.add_argument("--src-dir", default=str(DEFAULT_SRC_DIR))
    parser.add_argument("--outdir", default=str(REPO_ROOT / "debug" / "sc_table_probe"))
    parser.add_argument(
        "--profiles",
        default="off,l_only,g_only,i_only,full",
        help="Comma separated profile names",
    )
    parser.add_argument("--tests", default="", help="Comma separated test names, empty means all")
    parser.add_argument("--extra-param", action="append", default=[])
    parser.add_argument("--max-workers", type=int, default=1)
    parser.add_argument("--skip-run", action="store_true", help="Reuse existing outdir results")
    parser.add_argument("--copy-cpt-to-tmp", action="store_true", default=True)
    parser.add_argument("--no-copy-cpt-to-tmp", action="store_false", dest="copy_cpt_to_tmp")
    parser.add_argument("--top-branch-limit", type=int, default=200)
    return parser.parse_args()


def builtin_profiles() -> Dict[str, Profile]:
    return {
        "off": Profile(
            name="off",
            params=(
                "system.cpu[0].branchPred.mgsc.enabled=False",
                "system.cpu[0].branchPred.microtage.enabled=False",
            ),
            focus_table=None,
            enable_db=False,
        ),
        "full": Profile(
            name="full",
            params=(
                "system.cpu[0].branchPred.mgsc.enabled=True",
                "system.cpu[0].branchPred.mgsc.enableBwTable=True",
                "system.cpu[0].branchPred.mgsc.enableLTable=True",
                "system.cpu[0].branchPred.mgsc.enableITable=True",
                "system.cpu[0].branchPred.mgsc.enableGTable=True",
                "system.cpu[0].branchPred.mgsc.enablePTable=True",
                "system.cpu[0].branchPred.mgsc.enableBiasTable=True",
                "system.cpu[0].branchPred.microtage.enabled=False",
            ),
            focus_table=None,
        ),
        "l_only": Profile(
            name="l_only",
            params=(
                "system.cpu[0].branchPred.mgsc.enabled=True",
                "system.cpu[0].branchPred.mgsc.enableBwTable=False",
                "system.cpu[0].branchPred.mgsc.enableLTable=True",
                "system.cpu[0].branchPred.mgsc.enableITable=False",
                "system.cpu[0].branchPred.mgsc.enableGTable=False",
                "system.cpu[0].branchPred.mgsc.enablePTable=False",
                "system.cpu[0].branchPred.mgsc.enableBiasTable=False",
                "system.cpu[0].branchPred.microtage.enabled=False",
            ),
            focus_table="l",
        ),
        "g_only": Profile(
            name="g_only",
            params=(
                "system.cpu[0].branchPred.mgsc.enabled=True",
                "system.cpu[0].branchPred.mgsc.enableBwTable=False",
                "system.cpu[0].branchPred.mgsc.enableLTable=False",
                "system.cpu[0].branchPred.mgsc.enableITable=False",
                "system.cpu[0].branchPred.mgsc.enableGTable=True",
                "system.cpu[0].branchPred.mgsc.enablePTable=False",
                "system.cpu[0].branchPred.mgsc.enableBiasTable=False",
                "system.cpu[0].branchPred.microtage.enabled=False",
            ),
            focus_table="g",
        ),
        "i_only": Profile(
            name="i_only",
            params=(
                "system.cpu[0].branchPred.mgsc.enabled=True",
                "system.cpu[0].branchPred.mgsc.enableBwTable=False",
                "system.cpu[0].branchPred.mgsc.enableLTable=False",
                "system.cpu[0].branchPred.mgsc.enableITable=True",
                "system.cpu[0].branchPred.mgsc.enableGTable=False",
                "system.cpu[0].branchPred.mgsc.enablePTable=False",
                "system.cpu[0].branchPred.mgsc.enableBiasTable=False",
                "system.cpu[0].branchPred.microtage.enabled=False",
            ),
            focus_table="i",
        ),
    }


def parse_hex_or_int(v: str) -> int:
    s = v.strip().lower()
    if not s:
        return 0
    if s.startswith("0x"):
        return int(s, 16)
    if any(ch in "abcdef" for ch in s):
        return int(s, 16)
    return int(s, 10)


def parse_stats(path: Path) -> Dict[str, float]:
    keys = {
        "system.cpu.ipc",
        "system.cpu.fetch.rate",
        "system.cpu.branchPred.condNum",
        "system.cpu.branchPred.condMiss",
        "system.cpu.commit.branchMispredicts",
        "system.cpu.branchPred.mgsc.scUsed",
        "system.cpu.branchPred.mgsc.scCorrectTageWrong",
        "system.cpu.branchPred.mgsc.scWrongTageCorrect",
        "simTicks",
    }
    out: Dict[str, float] = {}
    if not path.exists():
        return out
    for line in path.read_text(encoding="utf-8", errors="ignore").splitlines():
        parts = line.split()
        if len(parts) < 2:
            continue
        if parts[0] not in keys:
            continue
        try:
            out[parts[0]] = float(parts[1])
        except ValueError:
            continue
    return out


def parse_top_csv(path: Path, limit: int) -> Dict[int, Dict[str, float]]:
    out: Dict[int, Dict[str, float]] = {}
    if not path.exists():
        return out
    with path.open(encoding="utf-8", newline="") as fp:
        rows = list(csv.DictReader(fp))
    for row in rows[:limit]:
        try:
            pc_text = (row.get("pc", "") or "").strip()
            # topMispredictsByBranch.csv stores PC in hex form without "0x" prefix.
            pc = int(pc_text, 16) if pc_text else 0
            out[pc] = {
                "mispredicts": float(row.get("mispredicts", 0)),
                "total": float(row.get("total", 0)),
                "misPermil": float(row.get("misPermil", 0)),
                "dirMiss": float(row.get("dirMiss", 0)),
            }
        except (ValueError, TypeError):
            continue
    return out


def pct(old: float, new: float) -> float:
    if old == 0:
        return 0.0
    return (new - old) / old * 100.0


def query_mgsc_db(db_path: Path) -> Tuple[Dict[str, float], Dict[int, Dict[str, float]]]:
    if not db_path.exists():
        return {}, {}
    con = sqlite3.connect(str(db_path))
    cur = con.cursor()
    cur.execute("PRAGMA temp_store=MEMORY")

    overall_row = cur.execute(
        """
        SELECT
          COUNT(*) AS rows,
          SUM(CASE WHEN useSc=1 THEN 1 ELSE 0 END) AS use_sc_rows,
          SUM(CASE WHEN useSc=1 AND tagePred!=actualTaken AND scPred=actualTaken THEN 1 ELSE 0 END) AS fix_use,
          SUM(CASE WHEN useSc=1 AND tagePred=actualTaken AND scPred!=actualTaken THEN 1 ELSE 0 END) AS hurt_use
        FROM MGSCTRACE
        """
    ).fetchone()
    overall = {
        "rows": float(overall_row[0] or 0),
        "use_sc_rows": float(overall_row[1] or 0),
        "fix_use": float(overall_row[2] or 0),
        "hurt_use": float(overall_row[3] or 0),
    }
    overall["net_use"] = overall["fix_use"] - overall["hurt_use"]

    select_cols = [
        "branchPC",
        "COUNT(*) AS rows",
        "SUM(CASE WHEN useSc=1 THEN 1 ELSE 0 END) AS use_sc",
        "SUM(CASE WHEN useSc=1 AND tagePred!=actualTaken AND scPred=actualTaken THEN 1 ELSE 0 END) AS fix_use",
        "SUM(CASE WHEN useSc=1 AND tagePred=actualTaken AND scPred!=actualTaken THEN 1 ELSE 0 END) AS hurt_use",
    ]
    for short, col in TABLE_COLS.items():
        select_cols.append(
        f"SUM(CASE WHEN useSc=1 AND ((totalSum>=0) != ((totalSum - {col})>=0)) THEN 1 ELSE 0 END) AS {short}_decisive"
        )
        select_cols.append(
            f"SUM(CASE WHEN useSc=1 AND tagePred!=actualTaken AND scPred=actualTaken "
            f"AND (({col}>=0)=actualTaken) THEN 1 ELSE 0 END) AS {short}_agree_fix"
        )

    rows = cur.execute(
        f"""
        SELECT {", ".join(select_cols)}
        FROM MGSCTRACE
        GROUP BY branchPC
        """
    ).fetchall()
    con.close()

    by_pc: Dict[int, Dict[str, float]] = {}
    for row in rows:
        idx = 0
        pc = int(row[idx]); idx += 1
        rows_cnt = float(row[idx] or 0); idx += 1
        use_sc = float(row[idx] or 0); idx += 1
        fix_use = float(row[idx] or 0); idx += 1
        hurt_use = float(row[idx] or 0); idx += 1

        ent: Dict[str, float] = {
            "rows": rows_cnt,
            "use_sc": use_sc,
            "fix_use": fix_use,
            "hurt_use": hurt_use,
            "net_use": fix_use - hurt_use,
        }
        for short in TABLE_COLS:
            decisive = float(row[idx] or 0); idx += 1
            agree_fix = float(row[idx] or 0); idx += 1
            ent[f"{short}_decisive"] = decisive
            ent[f"{short}_agree_fix"] = agree_fix
            ent[f"{short}_decisive_ratio"] = decisive / use_sc if use_sc else 0.0
            ent[f"{short}_agree_fix_ratio"] = agree_fix / fix_use if fix_use else 0.0
        by_pc[pc] = ent
    return overall, by_pc


def discover_cases(cpt_dir: Path, src_dir: Path, selected: Optional[Iterable[str]]) -> List[Case]:
    allow = set(selected) if selected else None
    cases: List[Case] = []
    for bin_path in sorted(cpt_dir.glob("*-riscv64-xs.bin")):
        stem = bin_path.name.replace("-riscv64-xs.bin", "")
        if allow is not None and stem not in allow:
            continue
        disasm = cpt_dir / f"{stem}-riscv64-xs.txt"
        src = src_dir / f"{stem}.c"
        cases.append(
            Case(
                name=stem,
                bin_path=bin_path,
                disasm_path=disasm if disasm.exists() else None,
                src_path=src if src.exists() else None,
            )
        )
    return cases


def maybe_copy_to_tmp(case: Case, run_dir: Path) -> Path:
    tmp_path = Path("/tmp") / f"{case.name}-riscv64-xs.bin"
    shutil.copy2(case.bin_path, tmp_path)
    return tmp_path


def run_one(
    case: Case,
    profile: Profile,
    args: argparse.Namespace,
    outdir: Path,
) -> RunResult:
    run_dir = outdir / profile.name / case.name
    run_dir.mkdir(parents=True, exist_ok=True)
    cmd = [
        str(Path(args.gem5_bin)),
        "--outdir",
        str(run_dir),
        str(Path(args.config)),
        "--raw-cpt",
    ]
    cpt_path = maybe_copy_to_tmp(case, run_dir) if args.copy_cpt_to_tmp else case.bin_path
    cmd.extend(["--generic-rv-cpt", str(cpt_path)])
    if profile.enable_db:
        cmd.extend(["--enable-bp-db", "mgsc"])
    for p in profile.params:
        cmd.extend(["--param", p])
    for p in args.extra_param:
        cmd.extend(["--param", p])

    ok = True
    err = ""
    if not args.skip_run:
        stdout = (run_dir / "gem5.stdout").open("w", encoding="utf-8")
        stderr = (run_dir / "gem5.stderr").open("w", encoding="utf-8")
        try:
            proc = subprocess.run(cmd, stdout=stdout, stderr=stderr, text=True)
            ok = proc.returncode == 0
            if not ok:
                err = f"returncode={proc.returncode}"
        finally:
            stdout.close()
            stderr.close()
    else:
        ok = (run_dir / STATS_TXT).exists()
        if not ok:
            err = "skip-run but stats not found"

    stats = parse_stats(run_dir / STATS_TXT)
    top = parse_top_csv(run_dir / TOP_CSV, args.top_branch_limit)
    db_overall, db_by_pc = query_mgsc_db(run_dir / BP_DB) if profile.enable_db else ({}, {})
    return RunResult(
        case=case,
        profile=profile,
        run_dir=run_dir,
        ok=ok,
        cmd=cmd,
        stats=stats,
        top=top,
        db_overall=db_overall,
        db_by_pc=db_by_pc,
        error=err,
    )


def build_reports(results: List[RunResult], profiles: List[Profile], outdir: Path) -> None:
    baseline: Dict[str, RunResult] = {}
    for r in results:
        if r.profile.name == "off":
            baseline[r.case.name] = r

    summary_rows: List[Dict[str, object]] = []
    branch_rows: List[Dict[str, object]] = []

    for r in results:
        base = baseline.get(r.case.name)
        off_cond_miss = base.stats.get("system.cpu.branchPred.condMiss", 0.0) if base else 0.0
        on_cond_miss = r.stats.get("system.cpu.branchPred.condMiss", 0.0)
        off_cond_num = base.stats.get("system.cpu.branchPred.condNum", 0.0) if base else 0.0
        on_cond_num = r.stats.get("system.cpu.branchPred.condNum", 0.0)
        off_rate = off_cond_miss / off_cond_num if off_cond_num else 0.0
        on_rate = on_cond_miss / on_cond_num if on_cond_num else 0.0

        summary_rows.append(
            {
                "case": r.case.name,
                "profile": r.profile.name,
                "ok": int(r.ok),
                "off_condMiss": off_cond_miss,
                "on_condMiss": on_cond_miss,
                "condMiss_delta": on_cond_miss - off_cond_miss,
                "off_condMissRate": off_rate,
                "on_condMissRate": on_rate,
                "condMissRate_delta_pct": pct(off_rate, on_rate),
                "off_branchMisp": base.stats.get("system.cpu.commit.branchMispredicts", 0.0) if base else 0.0,
                "on_branchMisp": r.stats.get("system.cpu.commit.branchMispredicts", 0.0),
                "mgsc_fix_use": r.db_overall.get("fix_use", 0.0),
                "mgsc_hurt_use": r.db_overall.get("hurt_use", 0.0),
                "mgsc_net_use": r.db_overall.get("net_use", 0.0),
                "source": str(r.case.src_path) if r.case.src_path else "",
            }
        )

        if base is None or r.profile.name == "off":
            continue
        pcs = set(base.top.keys()) | set(r.top.keys())
        for pc in sorted(pcs):
            off = base.top.get(pc, {})
            on = r.top.get(pc, {})
            off_m = float(off.get("mispredicts", 0.0))
            on_m = float(on.get("mispredicts", 0.0))
            db = r.db_by_pc.get(pc, {})
            row = {
                "case": r.case.name,
                "profile": r.profile.name,
                "pc_hex": f"0x{pc:x}",
                "off_misp": off_m,
                "on_misp": on_m,
                "delta_misp": on_m - off_m,
                "off_total": float(off.get("total", 0.0)),
                "on_total": float(on.get("total", 0.0)),
                "fix_use": db.get("fix_use", 0.0),
                "hurt_use": db.get("hurt_use", 0.0),
                "net_use": db.get("net_use", 0.0),
                "use_sc": db.get("use_sc", 0.0),
            }
            for short in TABLE_COLS:
                row[f"{short}_decisive_ratio"] = db.get(f"{short}_decisive_ratio", 0.0)
                row[f"{short}_agree_fix_ratio"] = db.get(f"{short}_agree_fix_ratio", 0.0)
            if r.profile.focus_table:
                focus = r.profile.focus_table
                row["focus_table"] = focus
                row["focus_decisive_ratio"] = row[f"{focus}_decisive_ratio"]
                row["focus_agree_fix_ratio"] = row[f"{focus}_agree_fix_ratio"]
            else:
                row["focus_table"] = ""
                row["focus_decisive_ratio"] = 0.0
                row["focus_agree_fix_ratio"] = 0.0
            branch_rows.append(row)

    summary_csv = outdir / "summary.csv"
    branch_csv = outdir / "branch_delta.csv"
    write_csv(summary_csv, summary_rows)
    write_csv(branch_csv, branch_rows)

    md_lines = render_markdown(summary_rows, branch_rows, profiles)
    (outdir / "report.md").write_text("\n".join(md_lines), encoding="utf-8")
    (outdir / "report.json").write_text(
        json.dumps({"summary": summary_rows, "branch_delta": branch_rows}, indent=2),
        encoding="utf-8",
    )


def write_csv(path: Path, rows: List[Dict[str, object]]) -> None:
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    keys = list(rows[0].keys())
    with path.open("w", encoding="utf-8", newline="") as fp:
        writer = csv.DictWriter(fp, fieldnames=keys)
        writer.writeheader()
        writer.writerows(rows)


def render_markdown(
    summary_rows: List[Dict[str, object]],
    branch_rows: List[Dict[str, object]],
    profiles: List[Profile],
) -> List[str]:
    lines: List[str] = []
    lines.append("# SC Table Probe Report")
    lines.append("")
    lines.append("## Profiles")
    lines.append("")
    for p in profiles:
        focus = p.focus_table if p.focus_table else "-"
        lines.append(f"- `{p.name}`: focus={focus}, db={'on' if p.enable_db else 'off'}")
    lines.append("")

    lines.append("## Overall (sorted by condMiss reduction)")
    lines.append("")
    lines.append("| case | profile | off condMiss | on condMiss | delta | off rate | on rate | delta% | net_use |")
    lines.append("| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
    sorted_rows = sorted(summary_rows, key=lambda x: float(x.get("condMiss_delta", 0.0)))
    for r in sorted_rows[:80]:
        lines.append(
            f"| {r['case']} | {r['profile']} | {r['off_condMiss']:.0f} | {r['on_condMiss']:.0f} | "
            f"{r['condMiss_delta']:.0f} | {r['off_condMissRate']:.4f} | {r['on_condMissRate']:.4f} | "
            f"{r['condMissRate_delta_pct']:+.2f}% | {r['mgsc_net_use']:.0f} |"
        )
    lines.append("")

    lines.append("## G / I candidate branches (best improvements)")
    lines.append("")
    lines.append("| case | profile | pc | off misp | on misp | delta | net_use | focus decisive | focus agree_fix |")
    lines.append("| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |")
    focus_rows = [r for r in branch_rows if r.get("focus_table") in {"g", "i"} and r["off_misp"] >= 50]
    focus_rows.sort(key=lambda x: float(x["delta_misp"]))
    for r in focus_rows[:80]:
        lines.append(
            f"| {r['case']} | {r['profile']} | {r['pc_hex']} | {r['off_misp']:.0f} | {r['on_misp']:.0f} | "
            f"{r['delta_misp']:.0f} | {r['net_use']:.0f} | {r['focus_decisive_ratio']:.3f} | "
            f"{r['focus_agree_fix_ratio']:.3f} |"
        )
    lines.append("")
    lines.append("Interpretation tips:")
    lines.append("- `delta<0` means SC profile improves that branch against `off`.")
    lines.append("- High `focus_decisive_ratio` means the focus table often changes SC final sign.")
    lines.append("- High `focus_agree_fix_ratio` means focus table sign aligns with real outcome on SC-fix events.")
    return lines


def main() -> int:
    args = parse_args()
    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    builtins = builtin_profiles()
    profile_names = [x.strip() for x in args.profiles.split(",") if x.strip()]
    profiles: List[Profile] = []
    for name in profile_names:
        if name not in builtins:
            raise ValueError(f"Unknown profile: {name}. choose from {sorted(builtins)}")
        profiles.append(builtins[name])
    if "off" not in {p.name for p in profiles}:
        profiles.insert(0, builtins["off"])

    selected = [x.strip() for x in args.tests.split(",") if x.strip()] or None
    cases = discover_cases(Path(args.cpt_dir), Path(args.src_dir), selected)
    if not cases:
        print("No test cases found.")
        return 1

    tasks = [(case, profile) for case in cases for profile in profiles]
    results: List[RunResult] = []
    with ThreadPoolExecutor(max_workers=max(1, args.max_workers)) as ex:
        futures = [
            ex.submit(run_one, case=case, profile=profile, args=args, outdir=outdir)
            for case, profile in tasks
        ]
        for fut in as_completed(futures):
            res = fut.result()
            results.append(res)
            status = "OK" if res.ok else "FAIL"
            print(f"[{status}] {res.profile.name}/{res.case.name}")

    build_reports(results, profiles, outdir)
    print(f"Report written to: {outdir / 'report.md'}")
    print(f"CSV written to: {outdir / 'summary.csv'} and {outdir / 'branch_delta.csv'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
