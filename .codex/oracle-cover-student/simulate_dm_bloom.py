#!/usr/bin/env python3

import math
import sqlite3
from collections import Counter, deque
from dataclasses import dataclass


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
class RefConfig:
    name: str
    table: str
    offsets: list[int]
    score_max: int
    round_max: int
    require_same_page: bool
    k: int
    a: float
    b: float


REF_CONFIGS = [
    RefConfig("vbop", "vbopTrainTraceTable", VBOP_OFFSETS, 31, 50, False, 2, 0.0, 0.0),
    RefConfig("pbop", "pbopTrainTraceTable", PBOP_OFFSETS, 31, 50, True, 4, 0.0, 0.05),
]

M_VALUES = [256, 512, 1024, 2048, 4096]
HASH_MODES = ["lowbits", "bop_rr", "splitmix"]
HASH_COUNTS = [1, 2]


def same_page(a: int, b: int) -> bool:
    return (a // PAGE) == (b // PAGE)


def load_rows(table: str) -> list[tuple[int, int]]:
    con = sqlite3.connect(DB)
    cur = con.cursor()
    return list(cur.execute(f"select Tick, TrainAddr from {table} order by Tick, ID"))


def rr_hash(addr: int) -> int:
    line_addr = addr >> 6
    mask = RR - 1
    return ((line_addr & mask) ^ ((line_addr >> 8) & mask)) & mask


def rr_tag(addr: int) -> int:
    line_addr = addr >> 6
    return (line_addr >> 8) & TAG_MASK


def simulate_teacher(cfg: RefConfig) -> list[dict]:
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


def oracle_and_cov(rows: list[tuple[int, int]], cfg: RefConfig) -> tuple[int, Counter]:
    seen = set()
    cov = Counter()
    for _, addr in rows:
        for off in cfg.offsets:
            prev = addr - off * BLK
            if prev in seen and ((not cfg.require_same_page) or same_page(prev, addr)):
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


def build_exact_reference(cfg: RefConfig) -> tuple[list[dict], list[tuple[int, Counter]], float]:
    phases = simulate_teacher(cfg)
    oracles = [oracle_and_cov(phase["rows"], cfg) for phase in phases]
    oracle_cov_next = sum(oracles[i + 1][1][oracles[i + 1][0]] for i in range(len(phases) - 1))
    return phases, oracles, oracle_cov_next


def build_exact_pool_trace(cfg: RefConfig, phases: list[dict], oracles: list[tuple[int, Counter]]) -> list[dict]:
    pool = []
    conf = {}
    last_cov = {}
    out = []

    for i, phase in enumerate(phases):
        if i > 0:
            incoming = phases[i - 1]["teacher_best"]
            if incoming not in pool:
                if len(pool) < cfg.k:
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

        if pool:
            _, cov = oracles[i]
            student_best = pick_best(pool, cov, conf)
            student_cov = cov[student_best]
            student_worst = pick_worst(pool, cov, conf)
            updates = {off: 0.0 for off in pool}
            updates[student_best] = 1.0
            updates[student_worst] = -1.0
            for off in pool:
                conf[off] = conf.get(off, 0.0) * cfg.a + updates[off] * (1.0 - cfg.a)
                last_cov[off] = cov.get(off, 0)
        else:
            student_best = None
            student_cov = 0

        ratio = student_cov / len(phase["rows"]) if phase["rows"] else 0.0
        selected = student_best if (student_best is not None and ratio >= cfg.b) else phase["teacher_best"]

        out.append(
            {
                "pool": list(pool),
                "exact_best": student_best,
                "exact_cov": student_cov,
                "selected": selected,
                "phase_len": len(phase["rows"]),
            }
        )

    return out


def splitmix64(x: int) -> int:
    x = (x + 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9 & 0xFFFFFFFFFFFFFFFF
    x = (x ^ (x >> 27)) * 0x94D049BB133111EB & 0xFFFFFFFFFFFFFFFF
    return x ^ (x >> 31)


def hash_indexes(line_addr: int, m: int, mode: str, hcount: int) -> list[int]:
    mask = m - 1
    if mode == "lowbits":
        base1 = line_addr
        base2 = ((line_addr >> 6) ^ (line_addr >> 12) ^ 0x9E37) | 1
    elif mode == "xorfold":
        lgm = int(math.log2(m))
        base = line_addr ^ (line_addr >> lgm) ^ (line_addr >> (2 * lgm))
        base1 = base
        base2 = ((base >> 7) ^ line_addr ^ 0x85EB) | 1
    elif mode == "bop_rr":
        lgm = int(math.log2(m))
        base = ((line_addr & mask) ^ ((line_addr >> lgm) & mask)) & mask
        base1 = base
        base2 = (((line_addr >> (2 * lgm)) & mask) ^ line_addr ^ 0xC2B2) | 1
    elif mode == "splitmix":
        base1 = splitmix64(line_addr)
        base2 = splitmix64(line_addr ^ 0x9E3779B97F4A7C15) | 1
    else:
        raise ValueError(mode)
    return [((base1 + i * base2) & 0xFFFFFFFFFFFFFFFF) & mask for i in range(hcount)]


def dm_bloom_phase(
    rows: list[tuple[int, int]],
    pool: list[int],
    require_same_page: bool,
    m: int,
    mode: str,
    hcount: int,
) -> tuple[Counter, Counter]:
    table = [0] * m
    exact_seen = set()
    exact_cov = Counter()
    approx_cov = Counter()

    for _, addr in rows:
        line_addr = addr >> 6

        for bit_idx, off in enumerate(pool):
            prev = addr - off * BLK
            if prev in exact_seen and ((not require_same_page) or same_page(prev, addr)):
                exact_cov[off] += 1

            qidxs = hash_indexes(line_addr, m, mode, hcount)
            mask = 1 << bit_idx
            if all(table[idx] & mask for idx in qidxs):
                approx_cov[off] += 1

        for bit_idx, off in enumerate(pool):
            pred = addr + off * BLK
            if require_same_page and not same_page(addr, pred):
                continue
            pidxs = hash_indexes(pred >> 6, m, mode, hcount)
            mask = 1 << bit_idx
            for idx in pidxs:
                table[idx] |= mask

        exact_seen.add(addr)

    return exact_cov, approx_cov


def proxy_scan(cfg: RefConfig, phases: list[dict], oracles, pool_trace: list[dict]) -> list[dict]:
    results = []
    for m in M_VALUES:
        if m & (m - 1):
            continue
        for mode in HASH_MODES:
            for hcount in HASH_COUNTS:
                winner_match = 0
                winner_cov_sum = 0
                exact_cov_sum = 0
                fp_total = 0
                exact_total = 0
                next_cov_sum = 0
                next_match = 0

                for i, phase in enumerate(phases):
                    pool = pool_trace[i]["pool"]
                    if not pool:
                        continue

                    exact_cov, approx_cov = dm_bloom_phase(
                        phase["rows"], pool, cfg.require_same_page, m, mode, hcount
                    )
                    exact_best = max(pool, key=lambda off: (exact_cov[off], -abs(off), -off))
                    approx_best = max(pool, key=lambda off: (approx_cov[off], -abs(off), -off))

                    if approx_best == exact_best:
                        winner_match += 1
                    winner_cov_sum += exact_cov[approx_best]
                    exact_cov_sum += exact_cov[exact_best]
                    fp_total += sum(max(0, approx_cov[off] - exact_cov[off]) for off in pool)
                    exact_total += sum(exact_cov[off] for off in pool)

                    if i < len(phases) - 1:
                        ratio = approx_cov[approx_best] / len(phase["rows"]) if phase["rows"] else 0.0
                        selected = approx_best if ratio >= cfg.b else phases[i]["teacher_best"]
                        nxt_best, nxt_cov = oracles[i + 1]
                        next_cov_sum += nxt_cov.get(selected, 0)
                        if selected == nxt_best:
                            next_match += 1

                results.append(
                    {
                        "m": m,
                        "mode": mode,
                        "hcount": hcount,
                        "winner_match": winner_match / len(phases),
                        "winner_cov_ratio": winner_cov_sum / exact_cov_sum if exact_cov_sum else 0.0,
                        "fp_ratio": fp_total / exact_total if exact_total else 0.0,
                        "next_cov_ratio": next_cov_sum / sum(
                            oracles[i + 1][1][oracles[i + 1][0]] for i in range(len(phases) - 1)
                        ),
                        "next_match": next_match / (len(phases) - 1),
                    }
                )
    return results


def closed_loop_with_dm_bloom(
    cfg: RefConfig,
    phases: list[dict],
    oracles,
    m: int,
    mode: str,
    hcount: int,
) -> dict:
    pool = []
    conf = {}
    last_cov = {}
    keep_p1 = keep_m1 = keep_both = 0
    selected = []

    for i, phase in enumerate(phases):
        if i > 0:
            incoming = phases[i - 1]["teacher_best"]
            if incoming not in pool:
                if len(pool) < cfg.k:
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
            _, approx_cov = dm_bloom_phase(phase["rows"], pool, cfg.require_same_page, m, mode, hcount)
            student_best = pick_best(pool, approx_cov, conf)
            student_worst = pick_worst(pool, approx_cov, conf)
            updates = {off: 0.0 for off in pool}
            updates[student_best] = 1.0
            updates[student_worst] = -1.0
            for off in pool:
                conf[off] = conf.get(off, 0.0) * cfg.a + updates[off] * (1.0 - cfg.a)
                last_cov[off] = approx_cov.get(off, 0)
            ratio = approx_cov[student_best] / len(phase["rows"]) if phase["rows"] else 0.0
        else:
            student_best = None
            ratio = 0.0

        selected.append(student_best if (student_best is not None and ratio >= cfg.b) else phase["teacher_best"])

    next_cov_sum = 0
    next_match = 0
    oracle_cov_next = sum(oracles[i + 1][1][oracles[i + 1][0]] for i in range(len(phases) - 1))
    for i in range(len(phases) - 1):
        nxt_best, nxt_cov = oracles[i + 1]
        next_cov_sum += nxt_cov.get(selected[i], 0)
        if selected[i] == nxt_best:
            next_match += 1

    return {
        "next_cov": next_cov_sum / oracle_cov_next,
        "next_match": next_match / (len(phases) - 1),
        "keep_p1": keep_p1 / len(phases),
        "keep_m1": keep_m1 / len(phases),
        "keep_both": keep_both / len(phases),
    }


def main() -> None:
    for cfg in REF_CONFIGS:
        print(f"scanning {cfg.name}...", flush=True)
        phases, oracles, _ = build_exact_reference(cfg)
        pool_trace = build_exact_pool_trace(cfg, phases, oracles)
        proxy = proxy_scan(cfg, phases, oracles, pool_trace)
        proxy.sort(key=lambda x: (x["next_cov_ratio"], x["winner_match"], -x["fp_ratio"]), reverse=True)

        print(f"## {cfg.name}")
        print(
            f"reference K={cfg.k} A={cfg.a} B={cfg.b} "
            f"exact_next_cov={sum(oracles[i + 1][1].get(pool_trace[i]['selected'], 0) for i in range(len(phases) - 1)) / sum(oracles[i + 1][1][oracles[i + 1][0]] for i in range(len(phases) - 1)):.4f}"
        )
        print("top_proxy")
        for row in proxy[:8]:
            print(
                f"M={row['m']} mode={row['mode']} H={row['hcount']} "
                f"winner_match={row['winner_match']:.4f} winner_cov={row['winner_cov_ratio']:.4f} "
                f"fp_ratio={row['fp_ratio']:.4f} next_match={row['next_match']:.4f} "
                f"next_cov={row['next_cov_ratio']:.4f}"
            )

        print("closed_loop_check")
        for row in proxy[:3]:
            cl = closed_loop_with_dm_bloom(cfg, phases, oracles, row["m"], row["mode"], row["hcount"])
            print(
                f"M={row['m']} mode={row['mode']} H={row['hcount']} "
                f"closed_next_match={cl['next_match']:.4f} closed_next_cov={cl['next_cov']:.4f} "
                f"keep+1={cl['keep_p1']:.4f} keep-1={cl['keep_m1']:.4f} keep_both={cl['keep_both']:.4f}"
            )


if __name__ == "__main__":
    main()
