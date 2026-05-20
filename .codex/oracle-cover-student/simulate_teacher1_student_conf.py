#!/usr/bin/env python3

import sqlite3
from collections import Counter, deque
from dataclasses import dataclass
from itertools import product


DB = "test/mcf_11392_l2NL/bop_trace.db"
BLK = 64
PAGE = 4096

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
    require_same_page: bool


CONFIGS = [
    Config("vbop", "vbopTrainTraceTable", VBOP_OFFSETS, 31, 50, False),
    Config("pbop", "pbopTrainTraceTable", PBOP_OFFSETS, 31, 50, True),
]

A_VALUES = [0.0, 0.5, 0.75, 0.9, 0.95]
B_VALUES = [0.00, 0.05, 0.10, 0.15, 0.20, 0.25, 0.30]
K_VALUES = [2, 4, 8]
N = 1


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
                phases.append(
                    {
                        "rows": rows[phase_start : ev_idx + 1],
                        "teacher_best": phase_best,
                    }
                )
                phase_start = ev_idx + 1
                scores = [0] * len(cfg.offsets)
                roundn = 0
                best_score = 0
                phase_best = 0

    return phases


def oracle_and_cov(rows: list[tuple[int, int]], cfg: Config) -> tuple[int, Counter]:
    seen = set()
    cov = Counter()
    for _, addr in rows:
        for off in cfg.offsets:
            prev = addr - off * BLK
            if prev in seen and (
                (not cfg.require_same_page) or same_page(prev, addr)
            ):
                cov[off] += 1
        seen.add(addr)
    best = max(cfg.offsets, key=lambda off: (cov[off], -abs(off), -off))
    return best, cov


def pick_min_conf(pool: list[int], conf: dict[int, float], last_cov: dict[int, int]) -> int:
    return min(pool, key=lambda off: (conf.get(off, 0.0), last_cov.get(off, 0), -abs(off), off))


def pick_best(pool: list[int], cov: Counter, conf: dict[int, float]) -> int:
    return max(pool, key=lambda off: (cov.get(off, 0), conf.get(off, 0.0), -abs(off), -off))


def pick_worst(pool: list[int], cov: Counter, conf: dict[int, float]) -> int:
    return min(pool, key=lambda off: (cov.get(off, 0), conf.get(off, 0.0), abs(off), off))


def evaluate_cfg(cfg: Config) -> None:
    phases = simulate_teacher(cfg)
    oracles = [oracle_and_cov(phase["rows"], cfg) for phase in phases]
    oracle_cov_next = sum(oracles[i + 1][1][oracles[i + 1][0]] for i in range(len(phases) - 1))
    teacher_next_cov = sum(
        oracles[i + 1][1].get(phases[i]["teacher_best"], 0) for i in range(len(phases) - 1)
    ) / oracle_cov_next
    teacher_next_match = sum(
        1 for i in range(len(phases) - 1) if phases[i]["teacher_best"] == oracles[i + 1][0]
    ) / (len(phases) - 1)

    print(f"## {cfg.name}")
    print(f"teacher_next_match={teacher_next_match:.4f} teacher_next_cov={teacher_next_cov:.4f}")

    best = None
    for k, a, b in product(K_VALUES, A_VALUES, B_VALUES):
        pool = []
        conf = {}
        last_cov = {}
        keep_p1 = 0
        keep_m1 = 0
        keep_both = 0
        selected_for_next = []

        for i, phase in enumerate(phases):
            if i > 0:
                incoming = phases[i - 1]["teacher_best"]
                if incoming not in pool:
                    if len(pool) < k:
                        pool.append(incoming)
                        conf[incoming] = 0.0
                        last_cov[incoming] = 0
                    else:
                        victim = pick_min_conf(pool, conf, last_cov)
                        pool.remove(victim)
                        conf.pop(victim, None)
                        last_cov.pop(victim, None)
                        pool.append(incoming)
                        conf[incoming] = 0.0
                        last_cov[incoming] = 0

            if 1 in pool:
                keep_p1 += 1
            if -1 in pool:
                keep_m1 += 1
            if 1 in pool and -1 in pool:
                keep_both += 1

            if pool:
                _, cov = oracles[i]
                student_best = pick_best(pool, cov, conf)
                student_cov = cov[student_best]
                student_worst = pick_worst(pool, cov, conf)
                updates = {off: 0.0 for off in pool}
                updates[student_best] = 1.0
                updates[student_worst] = -1.0
                for off in pool:
                    conf[off] = conf.get(off, 0.0) * a + updates[off] * (1.0 - a)
                    last_cov[off] = cov.get(off, 0)
            else:
                student_best = None
                student_cov = 0

            ratio = student_cov / len(phase["rows"]) if phase["rows"] else 0.0
            if student_best is not None and ratio >= b:
                selected_for_next.append(student_best)
            else:
                selected_for_next.append(phase["teacher_best"])

        next_match = sum(
            1 for i in range(len(phases) - 1) if selected_for_next[i] == oracles[i + 1][0]
        ) / (len(phases) - 1)
        next_cov = sum(
            oracles[i + 1][1].get(selected_for_next[i], 0) for i in range(len(phases) - 1)
        ) / oracle_cov_next
        keep_both_ratio = keep_both / len(phases)

        print(
            f"K={k} A={a:.2f} B={b:.2f} next_match={next_match:.4f} "
            f"next_cov={next_cov:.4f} keep+1={keep_p1/len(phases):.4f} "
            f"keep-1={keep_m1/len(phases):.4f} keep_both={keep_both_ratio:.4f}"
        )

        score = (next_cov, next_match, keep_both_ratio)
        if best is None or score > best[0]:
            best = (score, k, a, b)

    _, k, a, b = best
    print(f"best_by_next_cov K={k} A={a} B={b}")


def main() -> None:
    for cfg in CONFIGS:
        evaluate_cfg(cfg)


if __name__ == "__main__":
    main()
