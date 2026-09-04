#!/usr/bin/env python3
import argparse
import sqlite3
from typing import Dict, List, Tuple


def parse_pc(text: str) -> int:
    text = text.strip().lower()
    if text.startswith("0x"):
        return int(text, 16)
    return int(text, 16)


def load_gem5_events(bpdb: str, branch_pc: int, limit: int) -> List[Dict[str, int]]:
    conn = sqlite3.connect(bpdb)
    conn.row_factory = sqlite3.Row
    rows = conn.execute(
        """
        select
          TICK,
          startPC as start_pc,
          controlPC as branch_pc,
          controlType as branch_type,
          taken,
          mispred,
          source,
          target,
          fallThruPC as fallthrough
        from BPTRACE
        where controlPC = ?
        order by TICK
        limit ?
        """,
        (branch_pc, limit),
    ).fetchall()
    return [dict(r) for r in rows]


def load_rtl_events(db: str, branch_pc: int, limit: int) -> List[Dict[str, int]]:
    conn = sqlite3.connect(db)
    conn.row_factory = sqlite3.Row
    selects = []
    for i in range(8):
        selects.append(
            f"""
            select
              STAMP,
              TRAIN_STARTPC_ADDR as start_pc,
              TRAIN_BRANCHES_{i}_BITS_DEBUG_REALCFIPC as branch_pc,
              TRAIN_BRANCHES_{i}_BITS_ATTRIBUTE_BRANCHTYPE as branch_type,
              TRAIN_BRANCHES_{i}_BITS_TAKEN as taken,
              TRAIN_BRANCHES_{i}_BITS_MISPREDICT as mispred,
              TRAIN_BRANCHES_{i}_BITS_TARGET_ADDR as target,
              {i} as slot
            from BpuTrainTrace
            where TRAIN_BRANCHES_{i}_BITS_DEBUG_REALCFIPC = ?
            """
        )
    sql = " union all ".join(selects) + " order by STAMP, slot limit ?"
    params: List[int] = [branch_pc] * 8 + [limit]
    rows = conn.execute(sql, params).fetchall()
    return [dict(r) for r in rows]


def fmt_hex(value: int) -> str:
    return f"0x{value:x}"


def compare_prefix(gem5_events: List[Dict[str, int]], rtl_events: List[Dict[str, int]]) -> Tuple[int, Tuple[int, int], Tuple[int, int]]:
    common = min(len(gem5_events), len(rtl_events))
    for idx in range(common):
        g = gem5_events[idx]
        r = rtl_events[idx]
        if (g["taken"], g["mispred"]) != (r["taken"], r["mispred"]):
            return idx, (g["taken"], g["mispred"]), (r["taken"], r["mispred"])
    return common, (-1, -1), (-1, -1)


def print_table(title: str, events: List[Dict[str, int]], is_rtl: bool) -> None:
    print(f"[{title}]")
    if is_rtl:
        print("idx\tstamp\tstart_pc\tslot\ttaken\tmispred\tbranch_type\ttarget")
        for idx, e in enumerate(events):
            print(
                f"{idx}\t{e['STAMP']}\t{fmt_hex(e['start_pc'])}\t{e['slot']}\t"
                f"{e['taken']}\t{e['mispred']}\t{e['branch_type']}\t{fmt_hex(e['target'])}"
            )
    else:
        print("idx\ttick\tstart_pc\ttaken\tmispred\tsource\tbranch_type\ttarget\tfallthrough")
        for idx, e in enumerate(events):
            print(
                f"{idx}\t{e['TICK']}\t{fmt_hex(e['start_pc'])}\t{e['taken']}\t{e['mispred']}\t"
                f"{e['source']}\t{e['branch_type']}\t{fmt_hex(e['target'])}\t{fmt_hex(e['fallthrough'])}"
            )


def main() -> None:
    parser = argparse.ArgumentParser(description="Compare per-branch event sequences between gem5 BPTRACE and RTL BpuTrainTrace.")
    parser.add_argument("--gem5-bpdb", required=True)
    parser.add_argument("--rtl-db", required=True)
    parser.add_argument("--branch-pc", required=True, help="Hex branch PC, e.g. 0x133a8")
    parser.add_argument("--limit", type=int, default=128)
    parser.add_argument("--print-limit", type=int, default=24)
    args = parser.parse_args()

    branch_pc = parse_pc(args.branch_pc)
    gem5_events = load_gem5_events(args.gem5_bpdb, branch_pc, args.limit)
    rtl_events = load_rtl_events(args.rtl_db, branch_pc, args.limit)

    print(f"[summary] branch_pc={fmt_hex(branch_pc)} gem5_events={len(gem5_events)} rtl_events={len(rtl_events)}")
    mismatch_idx, gem5_pair, rtl_pair = compare_prefix(gem5_events, rtl_events)
    if gem5_pair == (-1, -1):
        print(f"[prefix] first {mismatch_idx} events match on (taken, mispred)")
    else:
        print(
            f"[prefix] first mismatch at idx={mismatch_idx}: "
            f"gem5(taken={gem5_pair[0]}, mispred={gem5_pair[1]}) vs "
            f"rtl(taken={rtl_pair[0]}, mispred={rtl_pair[1]})"
        )

    print_table("gem5", gem5_events[: args.print_limit], is_rtl=False)
    print()
    print_table("rtl", rtl_events[: args.print_limit], is_rtl=True)


if __name__ == "__main__":
    main()
