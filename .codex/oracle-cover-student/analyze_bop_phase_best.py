#!/usr/bin/env python3

import argparse
import csv
import sqlite3
from collections import Counter, deque
from dataclasses import dataclass
from pathlib import Path


BLK = 64
RR = 256
TAG_BITS = 12
TAG_MASK = (1 << TAG_BITS) - 1
DELAY_TICKS = 300 * 333

VBOP_OFFSETS = [
    x
    for i in [
        1,
        2,
        3,
        4,
        5,
        6,
        8,
        9,
        10,
        12,
        15,
        16,
        18,
        20,
        24,
        25,
        27,
        30,
        32,
        36,
        40,
        45,
        48,
        50,
        54,
        60,
        64,
        72,
        75,
        80,
        81,
        90,
        96,
        100,
        108,
        120,
        125,
        128,
        135,
        144,
        150,
        160,
        162,
        180,
        192,
        200,
        216,
        225,
        240,
        243,
        250,
    ]
    for x in (i, -i)
] + [-256]

PBOP_OFFSETS = [
    x
    for i in [1, 2, 3, 4, 5, 6, 8, 9, 10, 12, 15, 16, 18, 20, 24, 25, 27, 30]
    for x in (i, -i)
] + [-32]


@dataclass
class TraceConfig:
    kind: str
    offsets: list[int]
    score_max: int = 31
    round_max: int = 50
    bad_score: int = 2
    initial_best: int = 2


CONFIGS = {
    "vbop": TraceConfig("vbop", VBOP_OFFSETS),
    "pbop": TraceConfig("pbop", PBOP_OFFSETS),
}


def list_tables(con: sqlite3.Connection) -> list[str]:
    rows = con.execute("SELECT name FROM sqlite_master WHERE type='table'").fetchall()
    return [row[0] for row in rows if row[0] != "sqlite_sequence"]


def get_columns(con: sqlite3.Connection, table: str) -> dict[str, str]:
    rows = con.execute(f"PRAGMA table_info('{table}')").fetchall()
    return {str(row[1]).upper(): str(row[1]) for row in rows}


def rr_hash(addr: int) -> int:
    line_addr = addr >> 6
    mask = RR - 1
    return ((line_addr & mask) ^ ((line_addr >> 8) & mask)) & mask


def rr_tag(addr: int) -> int:
    line_addr = addr >> 6
    return (line_addr >> 8) & TAG_MASK


def pick_table(con: sqlite3.Connection, kind: str, hart: int, override: str | None) -> str:
    if override:
        return override

    tables = set(list_tables(con))
    preferred = [
        f"{kind.upper()}TrainTrace_h{hart}",
        f"{kind.upper()}TrainTrace",
        f"{kind}TrainTraceTable",
    ]
    for name in preferred:
        if name in tables:
            return name

    raise SystemExit(
        f"no train trace table found for {kind}; tried {preferred}"
    )


def load_rows(con: sqlite3.Connection, table: str) -> list[tuple[int, int, int]]:
    cols = get_columns(con, table)
    stamp_col = cols.get("STAMP") or cols.get("TICK")
    addr_col = cols.get("ADDR") or cols.get("TRAINADDR") or cols.get("TRAINVADDR")
    if stamp_col is None or addr_col is None:
        raise SystemExit(
            f"table {table} does not have a supported stamp/address column set; "
            f"columns={sorted(cols.values())}"
        )

    sql = f"SELECT ID, {stamp_col}, {addr_col} FROM {table} ORDER BY {stamp_col}, ID"
    return [(int(row[0]), int(row[1]), int(row[2])) for row in con.execute(sql)]


def simulate_teacher_phases(
    rows: list[tuple[int, int, int]], cfg: TraceConfig
) -> tuple[list[dict], int]:
    rr: list[tuple[int, int] | None] = [None] * RR
    dq: deque[tuple[int, int]] = deque()
    scores = [0] * len(cfg.offsets)
    idx = 0
    roundn = 0
    best_score = 0
    phase_best = cfg.initial_best
    phase_start = 0
    phases: list[dict] = []

    for pos, (row_id, stamp, addr) in enumerate(rows):
        while dq and dq[0][0] <= stamp:
            _, ready_addr = dq.popleft()
            rr[rr_hash(ready_addr)] = (rr_tag(ready_addr), ready_addr)
        if len(dq) < 16:
            dq.append((stamp + DELAY_TICKS, addr))

        off = cfg.offsets[idx]
        lookup = addr - off * BLK
        ent = rr[rr_hash(lookup)]
        if ent is not None and ent[0] == rr_tag(lookup):
            scores[idx] += 1
            if scores[idx] > best_score:
                best_score = scores[idx]
                phase_best = off

        idx += 1
        if idx == len(cfg.offsets):
            idx = 0
            roundn += 1
            if best_score >= cfg.score_max or roundn == cfg.round_max:
                phase_rows = rows[phase_start : pos + 1]
                phases.append(
                    {
                        "phase_idx": len(phases),
                        "start_row_id": phase_rows[0][0],
                        "end_row_id": phase_rows[-1][0],
                        "start_stamp": phase_rows[0][1],
                        "end_stamp": phase_rows[-1][1],
                        "phase_len": len(phase_rows),
                        "teacher_best": phase_best,
                        "teacher_best_score": best_score,
                        "teacher_issue_enable": int(best_score >= cfg.bad_score),
                        "end_reason": "score_max" if best_score >= cfg.score_max else "round_max",
                    }
                )
                phase_start = pos + 1
                scores = [0] * len(cfg.offsets)
                idx = 0
                roundn = 0
                best_score = 0
                phase_best = cfg.initial_best

    return phases, len(rows) - phase_start


def write_csv(path: Path, rows: list[dict]) -> None:
    fieldnames = [
        "phase_idx",
        "start_row_id",
        "end_row_id",
        "start_stamp",
        "end_stamp",
        "phase_len",
        "teacher_best",
        "teacher_best_score",
        "teacher_issue_enable",
        "end_reason",
    ]
    with path.open("w", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Summarize per-phase teacher best offset from RTL BOP train trace."
    )
    parser.add_argument("--db", required=True, help="Path to SQLite db")
    parser.add_argument(
        "--kind",
        required=True,
        choices=sorted(CONFIGS.keys()),
        help="Choose vbop or pbop",
    )
    parser.add_argument("--hart", type=int, default=0, help="Hart id for *_hX tables")
    parser.add_argument("--table", default="", help="Override train table name")
    parser.add_argument(
        "--out",
        default="",
        help="Optional CSV output path for per-phase summary",
    )
    parser.add_argument(
        "--topk",
        type=int,
        default=10,
        help="How many winner counts to print",
    )
    args = parser.parse_args()

    db = Path(args.db)
    if not db.is_file():
        raise SystemExit(f"db file not found: {db}")

    cfg = CONFIGS[args.kind]
    con = sqlite3.connect(str(db))
    table = pick_table(con, args.kind, args.hart, args.table or None)
    rows = load_rows(con, table)
    phases, tail_rows = simulate_teacher_phases(rows, cfg)

    if args.out:
        out_path = Path(args.out)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        write_csv(out_path, phases)
        print(f"wrote {out_path}")

    winner_counter = Counter(phase["teacher_best"] for phase in phases)
    enabled_phases = sum(phase["teacher_issue_enable"] for phase in phases)

    print(f"db={db}")
    print(f"table={table}")
    print(f"rows={len(rows)}")
    print(f"phase_count={len(phases)}")
    print(f"incomplete_tail_rows={tail_rows}")
    print(f"issue_enabled_phases={enabled_phases}/{len(phases) if phases else 0}")
    print("top_phase_winners:")
    for off, cnt in winner_counter.most_common(args.topk):
        print(f"  offset={off:>4} phases={cnt}")

    if phases:
        print("sample_phases:")
        sample = phases[:3] + phases[-3:] if len(phases) > 6 else phases
        seen = set()
        for phase in sample:
            idx = phase["phase_idx"]
            if idx in seen:
                continue
            seen.add(idx)
            print(
                "  "
                f"phase={idx:>3} len={phase['phase_len']:>5} "
                f"best={phase['teacher_best']:>4} score={phase['teacher_best_score']:>2} "
                f"enable={phase['teacher_issue_enable']} "
                f"stamp=[{phase['start_stamp']},{phase['end_stamp']}] "
                f"rows=[{phase['start_row_id']},{phase['end_row_id']}] "
                f"reason={phase['end_reason']}"
            )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
