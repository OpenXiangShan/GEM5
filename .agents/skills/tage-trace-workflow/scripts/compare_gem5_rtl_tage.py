#!/usr/bin/env python3
import argparse
import csv
import re
import sqlite3
from pathlib import Path
from typing import Dict, Iterable, List


COND_TABLES = [f"CondTrace_{i}" for i in range(8)]
GEM5_KEYS = [
    "system.cpu.branchPred.tage.allocateNeeded",
    "system.cpu.branchPred.tage.allocateSkipHighestProvider",
    "system.cpu.branchPred.tage.resolveBranchHasProvider",
    "system.cpu.branchPred.tage.resolveBranchUseProvider",
    "system.cpu.branchPred.tage.resolveBranchHasAlt",
    "system.cpu.branchPred.tage.resolveBranchUseAltTable",
    "system.cpu.branchPred.tage.resolveBranchUseBaseTable",
    "system.cpu.branchPred.tage.mispredictBranchHasProvider",
    "system.cpu.branchPred.tage.mispredictBranchUseProvider",
    "system.cpu.branchPred.tage.mispredictBranchHasAlt",
    "system.cpu.branchPred.tage.mispredictBranchUseAltTable",
    "system.cpu.branchPred.tage.mispredictBranchUseBaseTable",
]


def union_sql(columns: Iterable[str]) -> str:
    cols = ", ".join(columns)
    return " union all ".join(f"select {cols} from {t}" for t in COND_TABLES)


def parse_stats(path: Path) -> Dict[str, int]:
    result: Dict[str, int] = {}
    pattern = re.compile(r"^(\S+)\s+([0-9]+)")
    with path.open() as fh:
        for line in fh:
            match = pattern.match(line)
            if not match:
                continue
            key, value = match.groups()
            if key in GEM5_KEYS or key in {"simInsts", "system.cpu.numCycles"}:
                result[key] = int(value)
    return result


def query_gem5_bpdb(path: Path) -> Dict[str, int]:
    conn = sqlite3.connect(path)
    conn.row_factory = sqlite3.Row
    row = conn.execute(
        """
        select
          count(*) as rows,
          sum(case when actualTaken != predTaken then 1 else 0 end) as dir_mispred,
          sum(mainFound) as has_provider,
          sum(altFound) as has_alt,
          sum(useAlt) as use_alt_any,
          sum(case when useAlt != 0 and altFound != 0 then 1 else 0 end) as use_alt_table,
          sum(case when (useAlt = 0 and mainFound != 0) then 1 else 0 end) as use_provider,
          sum(case when (mainFound = 0 or (useAlt != 0 and altFound = 0)) then 1 else 0 end) as use_base,
          sum(allocSuccess) as alloc_ok
        from TAGEMISSTRACE
        """
    ).fetchone()
    return dict(row)


def query_rtl_db(path: Path) -> Dict[str, int]:
    conn = sqlite3.connect(path)
    conn.row_factory = sqlite3.Row
    row = conn.execute(
        f"""
        with cond as (
          {union_sql(['MISPREDICT', 'HASPROVIDER', 'USEPROVIDER', 'HASALT', 'USEALT',
                      'USEMETA', 'NEEDALLOCATE', 'ALLOCATESUCCESS', 'ALLOCATEFAILURE'])}
        )
        select
          count(*) as rows,
          sum(MISPREDICT) as mispred,
          sum(HASPROVIDER) as has_provider,
          sum(USEPROVIDER) as use_provider,
          sum(HASALT) as has_alt,
          sum(USEALT) as use_alt_table,
          sum(USEMETA) as use_meta,
          sum(NEEDALLOCATE) as need_alloc,
          sum(ALLOCATESUCCESS) as alloc_ok,
          sum(ALLOCATEFAILURE) as alloc_fail
        from cond
        """
    ).fetchone()
    return dict(row)


def load_gem5_top(csv_path: Path, top: int) -> List[Dict[str, str]]:
    rows: List[Dict[str, str]] = []
    with csv_path.open() as fh:
        reader = csv.DictReader(fh)
        for idx, row in enumerate(reader):
            if idx >= top:
                break
            rows.append(row)
    return rows


def query_rtl_top(path: Path, top: int) -> Dict[str, Dict[str, int]]:
    conn = sqlite3.connect(path)
    conn.row_factory = sqlite3.Row
    rows = conn.execute(
        f"""
        with cond as (
          {union_sql(['CFIPC', 'MISPREDICT', 'HASPROVIDER', 'USEPROVIDER',
                      'HASALT', 'USEALT', 'ALLOCATESUCCESS'])}
        )
        select
          printf('0x%x', CFIPC) as branch_pc,
          count(*) as total,
          sum(MISPREDICT) as mispred,
          sum(HASPROVIDER) as has_provider,
          sum(USEPROVIDER) as use_provider,
          sum(HASALT) as has_alt,
          sum(USEALT) as use_alt,
          sum(ALLOCATESUCCESS) as alloc_ok
        from cond
        group by CFIPC
        order by mispred desc, total desc
        limit ?
        """,
        (top,),
    ).fetchall()
    return {row["branch_pc"]: dict(row) for row in rows}


def main() -> None:
    parser = argparse.ArgumentParser(description="Compare gem5 and RTL TAGE summaries.")
    parser.add_argument("--gem5-stats", required=True, help="Path to gem5 stats.txt")
    parser.add_argument("--gem5-bpdb", required=True, help="Path to gem5 bp.db")
    parser.add_argument("--gem5-top-branch-csv", required=True, help="Path to gem5 topMispredictsByBranch.csv")
    parser.add_argument("--rtl-db", required=True, help="Path to RTL sqlite db")
    parser.add_argument("--top", type=int, default=12, help="Top branch count to compare")
    args = parser.parse_args()

    gem5_stats = parse_stats(Path(args.gem5_stats))
    gem5_bpdb = query_gem5_bpdb(Path(args.gem5_bpdb))
    rtl = query_rtl_db(Path(args.rtl_db))
    gem5_top = load_gem5_top(Path(args.gem5_top_branch_csv), args.top)
    rtl_top = query_rtl_top(Path(args.rtl_db), args.top)

    print("[gem5 stats]")
    for key in ["simInsts", "system.cpu.numCycles"] + GEM5_KEYS:
        if key in gem5_stats:
            print(f"{key}: {gem5_stats[key]}")

    print("\n[gem5 TAGEMISSTRACE aggregate]")
    for key, value in gem5_bpdb.items():
        print(f"{key}: {value}")

    print("\n[RTL CondTrace aggregate]")
    for key, value in rtl.items():
        print(f"{key}: {value}")

    print("\n[Hot branch overlap]")
    print(
        "branch_pc\tgem5_mispred\tgem5_total\trtl_mispred\trtl_total\t"
        "rtl_use_alt_table\tgem5_dirMiss"
    )
    for row in gem5_top:
        branch_pc = f"0x{row['pc'].lower()}"
        rtl_row = rtl_top.get(branch_pc)
        if rtl_row is None:
            print(
                f"{branch_pc}\t{row['mispredicts']}\t{row['total']}\t-\t-\t-\t{row['dirMiss']}"
            )
            continue
        print(
            f"{branch_pc}\t{row['mispredicts']}\t{row['total']}\t"
            f"{rtl_row['mispred']}\t{rtl_row['total']}\t{rtl_row['use_alt']}\t{row['dirMiss']}"
        )

    print("\n[note]")
    print("TAGEMISSTRACE.useAlt is pred.useAlt; compare stats against (useAlt && altFound).")


if __name__ == "__main__":
    main()
