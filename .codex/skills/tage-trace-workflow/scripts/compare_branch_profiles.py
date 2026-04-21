#!/usr/bin/env python3
import argparse
import csv
import sqlite3
from pathlib import Path
from typing import Dict, List


def parse_pc(text: str) -> int:
    text = text.strip().lower()
    if text.startswith("0x"):
        return int(text, 16)
    return int(text, 16)


def fmt_pc(pc: int) -> str:
    return f"0x{pc:x}"


def rate(num: int, den: int) -> float:
    if den == 0:
        return 0.0
    return num / den


def load_gem5_top(csv_path: Path, top: int) -> List[int]:
    pcs: List[int] = []
    with csv_path.open() as fh:
        reader = csv.DictReader(fh)
        for idx, row in enumerate(reader):
            if idx >= top:
                break
            pcs.append(int(row["pc"], 16))
    return pcs


def make_in_clause(items: List[int]) -> str:
    return ",".join("?" for _ in items)


def load_gem5_bp_profiles(bpdb: Path, pcs: List[int]) -> Dict[int, Dict[str, int]]:
    conn = sqlite3.connect(bpdb)
    conn.row_factory = sqlite3.Row
    if not pcs:
        return {}
    rows = conn.execute(
        f"""
        select
          controlPC as branch_pc,
          count(*) as bp_total,
          sum(mispred) as bp_mispred,
          sum(taken) as bp_taken
        from BPTRACE
        where controlPC in ({make_in_clause(pcs)})
        group by controlPC
        """,
        pcs,
    ).fetchall()
    result = {row["branch_pc"]: dict(row) for row in rows}
    for pc in pcs:
        result.setdefault(pc, {})
    return result


def load_rtl_top(db: Path, top: int) -> List[int]:
    conn = sqlite3.connect(db)
    rows = conn.execute(
        """
        with cond as (
          select CFIPC, MISPREDICT from CondTrace_0
          union all select CFIPC, MISPREDICT from CondTrace_1
          union all select CFIPC, MISPREDICT from CondTrace_2
          union all select CFIPC, MISPREDICT from CondTrace_3
          union all select CFIPC, MISPREDICT from CondTrace_4
          union all select CFIPC, MISPREDICT from CondTrace_5
          union all select CFIPC, MISPREDICT from CondTrace_6
          union all select CFIPC, MISPREDICT from CondTrace_7
        )
        select CFIPC
        from cond
        group by CFIPC
        order by sum(MISPREDICT) desc, count(*) desc
        limit ?
        """,
        (top,),
    ).fetchall()
    return [row[0] for row in rows]


def load_gem5_tage_profiles(bpdb: Path, pcs: List[int]) -> Dict[int, Dict[str, int]]:
    conn = sqlite3.connect(bpdb)
    conn.row_factory = sqlite3.Row
    if not pcs:
        return {}
    rows = conn.execute(
        f"""
        select
          branchPC as branch_pc,
          count(*) as sidecar_rows,
          sum(mainFound) as has_provider,
          sum(case when (useAlt = 0 and mainFound != 0) then 1 else 0 end) as use_provider,
          sum(altFound) as has_alt,
          sum(case when useAlt != 0 and altFound != 0 then 1 else 0 end) as use_alt_table,
          sum(case when (mainFound = 0 or (useAlt != 0 and altFound = 0)) then 1 else 0 end) as use_base,
          sum(allocSuccess) as alloc_ok
        from TAGEMISSTRACE
        where branchPC in ({make_in_clause(pcs)})
        group by branchPC
        """,
        pcs,
    ).fetchall()
    result = {row["branch_pc"]: dict(row) for row in rows}
    for pc in pcs:
        result.setdefault(pc, {})
    return result


def load_rtl_profiles(db: Path, pcs: List[int]) -> Dict[int, Dict[str, int]]:
    conn = sqlite3.connect(db)
    conn.row_factory = sqlite3.Row
    if not pcs:
        return {}
    union = """
      select CFIPC, MISPREDICT, ACTUALTAKEN, HASPROVIDER, USEPROVIDER, HASALT, USEALT, USEMETA, NEEDALLOCATE, ALLOCATESUCCESS
      from CondTrace_0
      union all
      select CFIPC, MISPREDICT, ACTUALTAKEN, HASPROVIDER, USEPROVIDER, HASALT, USEALT, USEMETA, NEEDALLOCATE, ALLOCATESUCCESS
      from CondTrace_1
      union all
      select CFIPC, MISPREDICT, ACTUALTAKEN, HASPROVIDER, USEPROVIDER, HASALT, USEALT, USEMETA, NEEDALLOCATE, ALLOCATESUCCESS
      from CondTrace_2
      union all
      select CFIPC, MISPREDICT, ACTUALTAKEN, HASPROVIDER, USEPROVIDER, HASALT, USEALT, USEMETA, NEEDALLOCATE, ALLOCATESUCCESS
      from CondTrace_3
      union all
      select CFIPC, MISPREDICT, ACTUALTAKEN, HASPROVIDER, USEPROVIDER, HASALT, USEALT, USEMETA, NEEDALLOCATE, ALLOCATESUCCESS
      from CondTrace_4
      union all
      select CFIPC, MISPREDICT, ACTUALTAKEN, HASPROVIDER, USEPROVIDER, HASALT, USEALT, USEMETA, NEEDALLOCATE, ALLOCATESUCCESS
      from CondTrace_5
      union all
      select CFIPC, MISPREDICT, ACTUALTAKEN, HASPROVIDER, USEPROVIDER, HASALT, USEALT, USEMETA, NEEDALLOCATE, ALLOCATESUCCESS
      from CondTrace_6
      union all
      select CFIPC, MISPREDICT, ACTUALTAKEN, HASPROVIDER, USEPROVIDER, HASALT, USEALT, USEMETA, NEEDALLOCATE, ALLOCATESUCCESS
      from CondTrace_7
    """
    rows = conn.execute(
        f"""
        with cond as (
          {union}
        )
        select
          CFIPC as branch_pc,
          count(*) as total,
          sum(MISPREDICT) as mispred,
          sum(ACTUALTAKEN) as taken,
          sum(HASPROVIDER) as has_provider,
          sum(USEPROVIDER) as use_provider,
          sum(HASALT) as has_alt,
          sum(USEALT) as use_alt_table,
          sum(USEMETA) as use_meta,
          sum(NEEDALLOCATE) as need_alloc,
          sum(ALLOCATESUCCESS) as alloc_ok
        from cond
        where CFIPC in ({make_in_clause(pcs)})
        group by CFIPC
        """,
        pcs,
    ).fetchall()
    result = {row["branch_pc"]: dict(row) for row in rows}
    for pc in pcs:
        result.setdefault(pc, {})
    return result


def summarize_branch(
    pc: int,
    gem5_bp: Dict[str, int],
    gem5_tage: Dict[str, int],
    rtl: Dict[str, int],
) -> List[str]:
    g_total = gem5_bp.get("bp_total", 0)
    r_total = rtl.get("total", 0)
    g_misp = gem5_bp.get("bp_mispred", 0)
    r_misp = rtl.get("mispred", 0)
    g_taken = gem5_bp.get("bp_taken", 0)
    g_sidecar_rows = gem5_tage.get("sidecar_rows", 0)
    g_has_alt = gem5_tage.get("has_alt", 0)
    r_has_alt = rtl.get("has_alt", 0)
    g_use_alt = gem5_tage.get("use_alt_table", 0)
    r_use_alt = rtl.get("use_alt_table", 0)
    g_alloc = gem5_tage.get("alloc_ok", 0)
    r_alloc = rtl.get("alloc_ok", 0)
    r_taken = rtl.get("taken", 0)

    return [
        fmt_pc(pc),
        str(g_total),
        str(r_total),
        f"{rate(g_misp, g_total) * 100:.2f}",
        f"{rate(r_misp, r_total) * 100:.2f}",
        f"{(rate(g_misp, g_total) - rate(r_misp, r_total)) * 100:+.2f}",
        f"{rate(g_taken, g_total) * 100:.2f}",
        f"{rate(r_taken, r_total) * 100:.2f}",
        str(g_sidecar_rows),
        f"{rate(g_sidecar_rows, g_total) * 100:.2f}",
        str(g_has_alt),
        str(r_has_alt),
        f"{rate(g_has_alt, g_sidecar_rows) * 100:.2f}",
        f"{rate(r_has_alt, r_total) * 100:.2f}",
        str(g_use_alt),
        str(r_use_alt),
        f"{rate(g_use_alt, g_has_alt) * 100:.2f}",
        f"{rate(r_use_alt, r_has_alt) * 100:.2f}",
        f"{rate(g_alloc, g_total) * 100:.2f}",
        f"{rate(r_alloc, r_total) * 100:.2f}",
        f"{(rate(g_alloc, g_total) - rate(r_alloc, r_total)) * 100:+.2f}",
        f"{rate(rtl.get('use_meta', 0), r_total) * 100:.2f}",
    ]


def main() -> None:
    parser = argparse.ArgumentParser(description="Compare gem5/RTL per-branch TAGE profiles.")
    parser.add_argument("--gem5-bpdb", required=True)
    parser.add_argument("--gem5-top-branch-csv", required=True)
    parser.add_argument("--rtl-db", required=True)
    parser.add_argument("--top", type=int, default=15)
    parser.add_argument("--branch-pc", action="append", default=[], help="Optional hex branch PC to include")
    args = parser.parse_args()

    gem5_top = load_gem5_top(Path(args.gem5_top_branch_csv), args.top)
    rtl_top = load_rtl_top(Path(args.rtl_db), args.top)
    extra = [parse_pc(pc) for pc in args.branch_pc]
    pcs = []
    seen = set()
    for pc in gem5_top + rtl_top + extra:
        if pc not in seen:
            seen.add(pc)
            pcs.append(pc)

    gem5_bp_profiles = load_gem5_bp_profiles(Path(args.gem5_bpdb), pcs)
    gem5_tage_profiles = load_gem5_tage_profiles(Path(args.gem5_bpdb), pcs)
    rtl_profiles = load_rtl_profiles(Path(args.rtl_db), pcs)

    header = [
        "branch_pc",
        "gem5_total",
        "rtl_total",
        "gem5_misp_pct",
        "rtl_misp_pct",
        "delta_misp_pct",
        "gem5_taken_pct",
        "rtl_taken_pct",
        "gem5_sidecar_rows",
        "gem5_sidecar_rows_per_bp_pct",
        "gem5_has_alt_count",
        "rtl_has_alt_count",
        "gem5_has_alt_per_sidecar_pct",
        "rtl_has_alt_pct",
        "gem5_use_alt_count",
        "rtl_use_alt_count",
        "gem5_use_alt_given_alt_pct",
        "rtl_use_alt_given_alt_pct",
        "gem5_alloc_pct",
        "rtl_alloc_pct",
        "delta_alloc_pct",
        "rtl_use_meta_pct",
    ]
    print("\t".join(header))
    for pc in pcs:
        print(
            "\t".join(
                summarize_branch(
                    pc,
                    gem5_bp_profiles.get(pc, {}),
                    gem5_tage_profiles.get(pc, {}),
                    rtl_profiles.get(pc, {}),
                )
            )
        )


if __name__ == "__main__":
    main()
