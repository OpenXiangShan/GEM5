#!/usr/bin/env python3
import argparse
import sqlite3
from typing import Iterable


COND_TABLES = [f"CondTrace_{i}" for i in range(8)]


def union_sql(columns: Iterable[str]) -> str:
    selects = []
    cols = ", ".join(columns)
    for table in COND_TABLES:
        selects.append(f"select {cols} from {table}")
    return " union all ".join(selects)


def main() -> None:
    parser = argparse.ArgumentParser(description="Aggregate XiangShan CondTrace tables.")
    parser.add_argument("--rtl-db", required=True, help="Path to RTL sqlite db")
    parser.add_argument("--top", type=int, default=12, help="Top branch rows to print")
    args = parser.parse_args()

    conn = sqlite3.connect(args.rtl_db)
    conn.row_factory = sqlite3.Row

    summary_sql = f"""
    with cond as (
      {union_sql(['MISPREDICT', 'HASPROVIDER', 'USEPROVIDER', 'HASALT', 'USEALT',
                  'USEMETA', 'NEEDALLOCATE', 'ALLOCATESUCCESS', 'ALLOCATEFAILURE'])}
    )
    select
      count(*) as total,
      sum(MISPREDICT) as mispred,
      sum(HASPROVIDER) as has_provider,
      sum(USEPROVIDER) as use_provider,
      sum(HASALT) as has_alt,
      sum(USEALT) as use_alt,
      sum(USEMETA) as use_meta,
      sum(NEEDALLOCATE) as need_alloc,
      sum(ALLOCATESUCCESS) as alloc_ok,
      sum(ALLOCATEFAILURE) as alloc_fail
    from cond
    """

    top_sql = f"""
    with cond as (
      {union_sql(['CFIPC', 'STARTPC_ADDR', 'MISPREDICT', 'HASPROVIDER', 'USEPROVIDER',
                  'HASALT', 'USEALT', 'USEMETA', 'NEEDALLOCATE', 'ALLOCATESUCCESS'])}
    )
    select
      printf('0x%x', CFIPC) as branch_pc,
      printf('0x%x', min(STARTPC_ADDR)) as sample_startpc,
      count(*) as total,
      sum(MISPREDICT) as mispred,
      round(100.0 * sum(MISPREDICT) / count(*), 2) as mispred_pct,
      sum(HASPROVIDER) as has_provider,
      sum(USEPROVIDER) as use_provider,
      sum(HASALT) as has_alt,
      sum(USEALT) as use_alt,
      sum(USEMETA) as use_meta,
      sum(NEEDALLOCATE) as need_alloc,
      sum(ALLOCATESUCCESS) as alloc_ok
    from cond
    group by CFIPC
    order by mispred desc, total desc
    limit ?
    """

    summary = conn.execute(summary_sql).fetchone()
    print("[RTL summary]")
    for key in summary.keys():
        print(f"{key}: {summary[key]}")

    print("\n[RTL top branches]")
    rows = conn.execute(top_sql, (args.top,)).fetchall()
    if not rows:
        return

    headers = rows[0].keys()
    print("\t".join(headers))
    for row in rows:
        print("\t".join(str(row[h]) for h in headers))


if __name__ == "__main__":
    main()
