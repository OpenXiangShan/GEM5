#!/usr/bin/env python3

import sqlite3
from collections import Counter, deque
from dataclasses import dataclass


DB = "test/mcf_11392_l2NL/bop_trace.db"
PAGE = 4096
BLK = 64

RR = 256
TAG_BITS = 12
TAG_MASK = (1 << TAG_BITS) - 1
DELAY = 300 * 333

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
class Config:
    name: str
    table: str
    offsets: list[int]
    score_max: int
    round_max: int
    bad_score: int


CONFIGS = [
    Config("vbop", "vbopTrainTraceTable", VBOP_OFFSETS, 31, 50, 2),
    Config("pbop", "pbopTrainTraceTable", PBOP_OFFSETS, 31, 50, 1),
]


def same_page(a: int, b: int) -> bool:
    return (a // PAGE) == (b // PAGE)


def rr_hash(addr: int) -> int:
    line_addr = addr >> 6
    mask = RR - 1
    return ((line_addr & mask) ^ ((line_addr >> 8) & mask)) & mask


def rr_tag(addr: int) -> int:
    line_addr = addr >> 6
    return (line_addr >> 8) & TAG_MASK


def load_rows(table: str) -> list[tuple[int, int]]:
    con = sqlite3.connect(DB)
    cur = con.cursor()
    return list(cur.execute(f"select Tick, TrainAddr from {table} order by Tick, ID"))


def simulate_teacher(cfg: Config) -> list[dict]:
    rows = load_rows(cfg.table)
    rr = [None] * RR
    dq = deque()
    scores = [0] * len(cfg.offsets)
    idx = 0
    roundn = 0
    best_score = 0
    phase_best = 0
    phase_start = 0
    phases = []

    for ev_idx, (tick, addr) in enumerate(rows):
        while dq and dq[0][0] <= tick:
            _, a = dq.popleft()
            rr[rr_hash(a)] = (rr_tag(a), a)
        if len(dq) < 16:
            dq.append((tick + DELAY, addr))

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
                end = ev_idx + 1
                ranked = sorted(
                    [(off, scores[i], i) for i, off in enumerate(cfg.offsets)],
                    key=lambda x: (-x[1], x[2]),
                )
                phases.append(
                    {
                        "rows": rows[phase_start:end],
                        "ranked": ranked,
                        "teacher_best": phase_best,
                    }
                )
                phase_start = end
                scores = [0] * len(cfg.offsets)
                roundn = 0
                best_score = 0
                phase_best = 0

    return phases


def phase_oracle(rows: list[tuple[int, int]], offsets: list[int]) -> dict:
    seen = set()
    cov = Counter()
    for _, addr in rows:
        for off in offsets:
            prev = addr - off * BLK
            if prev in seen and same_page(prev, addr):
                cov[off] += 1
        seen.add(addr)
    best = max(offsets, key=lambda off: (cov[off], -abs(off), -off))
    return {"best": best, "cover": cov[best], "cov": cov}


def pinned_topk(prev_ranked: list[tuple[int, int, int]], k: int) -> list[int]:
    chosen = [1, -1]
    for off, _, _ in prev_ranked:
        if off in chosen:
            continue
        chosen.append(off)
        if len(chosen) == k:
            break
    return chosen


def shadow_cov_winner(rows: list[tuple[int, int]], offsets: list[int]) -> tuple[int, Counter]:
    seen = set()
    cov = Counter()
    for _, addr in rows:
        for off in offsets:
            prev = addr - off * BLK
            if prev in seen and same_page(prev, addr):
                cov[off] += 1
        seen.add(addr)
    winner = max(offsets, key=lambda off: (cov[off], -abs(off), -off))
    return winner, cov


def evaluate(cfg: Config, k_values=(4, 8)) -> None:
    phases = simulate_teacher(cfg)
    oracles = [phase_oracle(p["rows"], cfg.offsets) for p in phases]

    print(f"## {cfg.name}")
    print(f"num_phases {len(phases)}")
    print(
        "teacher_match",
        sum(1 for p, o in zip(phases, oracles) if p["teacher_best"] == o["best"]),
        "/",
        len(phases),
    )

    for k in k_values:
        total = len(phases) - 1
        oracle_cov_sum = sum(o["cover"] for o in oracles[1:])
        next_match = 0
        next_cov = 0
        cand_hit = 0
        cand_best_cov = 0

        for i in range(total):
            cand = pinned_topk(phases[i]["ranked"], k)
            nxt = oracles[i + 1]
            if nxt["best"] in cand:
                cand_hit += 1
            best_in_cand = max(cand, key=lambda off: (nxt["cov"][off], -abs(off), -off))
            cand_best_cov += nxt["cov"][best_in_cand]

            winner, _ = shadow_cov_winner(phases[i]["rows"], cand)
            if winner == nxt["best"]:
                next_match += 1
            next_cov += nxt["cov"][winner]

        print(
            f"K={k} cand_hit={cand_hit}/{total}={cand_hit/total:.4f} "
            f"cand_upper_cov={cand_best_cov/oracle_cov_sum:.4f} "
            f"prev_shadow_to_next_match={next_match}/{total}={next_match/total:.4f} "
            f"prev_shadow_to_next_cov={next_cov/oracle_cov_sum:.4f}"
        )

        samples = [1, 2, 3, len(phases) - 3, len(phases) - 2, len(phases) - 1]
        for phase_idx in samples:
            if phase_idx <= 0 or phase_idx >= len(phases):
                continue
            cand = pinned_topk(phases[phase_idx - 1]["ranked"], k)
            prev_win, prev_cov = shadow_cov_winner(phases[phase_idx - 1]["rows"], cand)
            nxt = oracles[phase_idx]
            print(
                "phase",
                phase_idx + 1,
                "cand",
                cand,
                "prev_shadow",
                prev_win,
                "next_oracle",
                nxt["best"],
                "next_oracle_cov",
                nxt["cover"],
                "next_cov(prev_shadow)",
                nxt["cov"][prev_win],
            )


def main() -> None:
    for cfg in CONFIGS:
        evaluate(cfg)


if __name__ == "__main__":
    main()
