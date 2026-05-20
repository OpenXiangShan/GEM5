#!/usr/bin/env python3

from __future__ import annotations

import csv
import json
import math
import sqlite3
from collections import Counter, deque
from dataclasses import dataclass
from pathlib import Path


@dataclass
class BOPConfig:
    block_size: int
    page_bytes: int
    cross_page: bool
    ticks_per_cycle: int
    rr_size: int
    tag_bits: int
    score_max: int
    round_max: int
    bad_score: int
    teacher_delay_ticks: int
    teacher_delay_queue_size: int
    student_pool_size: int
    student_conf_alpha: float
    student_cov_threshold: float
    student_teacher_top_n: int
    student_filter_entries: int
    student_hash_count: int
    student_hash_mode: str
    offsets: list[int]

    @property
    def tag_mask(self) -> int:
        return (1 << self.tag_bits) - 1

    @property
    def l_blk_size(self) -> int:
        return int(math.log2(self.block_size))


@dataclass
class Phase:
    phase_idx: int
    start_row_id: int
    end_row_id: int
    start_tick: int
    end_tick: int
    rows: list[tuple[int, int, int]]
    teacher_best: int
    teacher_best_score: int
    teacher_issue_enabled: bool


@dataclass
class StudentEntry:
    offset: int
    conf: float = 0.0
    last_phase_cov: int = 0


def same_page(a: int, b: int, page_bytes: int) -> bool:
    return (a // page_bytes) == (b // page_bytes)


def splitmix64(x: int) -> int:
    x = (x + 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF
    x = ((x ^ (x >> 30)) * 0xBF58476D1CE4E5B9) & 0xFFFFFFFFFFFFFFFF
    x = ((x ^ (x >> 27)) * 0x94D049BB133111EB) & 0xFFFFFFFFFFFFFFFF
    return x ^ (x >> 31)


def hash_indexes(line_addr: int, entries: int, mode: str, hcount: int) -> list[int]:
    mask = entries - 1
    if mode == "lowbits":
        base1 = line_addr
        base2 = ((line_addr >> 6) ^ (line_addr >> 12) ^ 0x9E37) | 1
    elif mode == "bop_rr":
        lgm = int(math.log2(entries))
        base = ((line_addr & mask) ^ ((line_addr >> lgm) & mask)) & mask
        base1 = base
        base2 = ((((line_addr >> (2 * lgm)) & mask) ^ line_addr ^ 0xC2B2) | 1)
    elif mode == "splitmix":
        base1 = splitmix64(line_addr)
        base2 = splitmix64(line_addr ^ 0x9E3779B97F4A7C15) | 1
    else:
        raise ValueError(f"unsupported hash mode: {mode}")
    return [((base1 + i * base2) & 0xFFFFFFFFFFFFFFFF) & mask for i in range(hcount)]


def parse_ini_sections(config_path: Path) -> dict[str, dict[str, str]]:
    sections: dict[str, dict[str, str]] = {}
    current: str | None = None
    for raw in config_path.read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("[") and line.endswith("]"):
            current = line[1:-1]
            sections[current] = {}
            continue
        if current is None:
            continue
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        sections[current][key] = value
    return sections


def load_config(config_path: Path, section: str) -> BOPConfig:
    sections = parse_ini_sections(config_path)
    sec = sections[section]
    clk_domain = sec["clk_domain"]
    ticks_per_cycle = int(sections[clk_domain]["clock"])

    def as_bool(value: str) -> bool:
        return value.lower() == "true"

    return BOPConfig(
        block_size=int(sec["block_size"]),
        page_bytes=int(sec["page_bytes"]),
        cross_page=as_bool(sec["crossPage"]),
        ticks_per_cycle=ticks_per_cycle,
        rr_size=int(sec["rr_size"]),
        tag_bits=int(sec["tag_bits"]),
        score_max=int(sec["score_max"]),
        round_max=int(sec["round_max"]),
        bad_score=int(sec["bad_score"]),
        teacher_delay_ticks=int(sec["delay_queue_cycles"]) * ticks_per_cycle,
        teacher_delay_queue_size=int(sec["delay_queue_size"]),
        student_pool_size=int(sec["student_pool_size"]),
        student_conf_alpha=float(sec["student_conf_alpha"]),
        student_cov_threshold=float(sec["student_cov_threshold"]),
        student_teacher_top_n=int(sec["student_teacher_top_n"]),
        student_filter_entries=int(sec["student_filter_entries"]),
        student_hash_count=int(sec["student_hash_count"]),
        student_hash_mode=sec["student_hash_mode"],
        offsets=[int(x) for x in sec["offsets"].split()],
    )


class DelayedEventQueue:
    def __init__(self, delay_ticks: int, cycle_ticks: int, max_entries: int | None = None):
        self.delay_ticks = delay_ticks
        self.cycle_ticks = cycle_ticks
        self.max_entries = max_entries
        self.queue = deque()
        self.next_event_tick: int | None = None

    def push(self, cur_tick: int, payload) -> bool:
        if self.max_entries is not None and len(self.queue) >= self.max_entries:
            return False
        due_tick = cur_tick + self.delay_ticks
        self.queue.append((due_tick, payload))
        if self.next_event_tick is None:
            self.next_event_tick = due_tick
        return True

    def drain_until(self, cur_tick: int, apply_fn) -> None:
        while self.queue and self.next_event_tick is not None and self.next_event_tick <= cur_tick:
            event_tick = self.next_event_tick
            due_tick, payload = self.queue.popleft()
            if due_tick <= event_tick:
                apply_fn(payload)

            if not self.queue:
                self.next_event_tick = None
                break

            next_due_tick = self.queue[0][0]
            if next_due_tick <= event_tick:
                self.next_event_tick = event_tick + self.cycle_ticks
            else:
                self.next_event_tick = next_due_tick


def load_train_rows(db_path: Path, table: str) -> list[tuple[int, int, int]]:
    con = sqlite3.connect(db_path)
    cur = con.cursor()
    rows = list(cur.execute(f"SELECT ID, Tick, TrainAddr FROM {table} ORDER BY Tick, ID"))
    con.close()
    return rows


def load_actual_pf_offset_counts(db_path: Path, table: str) -> dict[int, int]:
    con = sqlite3.connect(db_path)
    cur = con.cursor()
    counts = {
        int(offset): int(total)
        for offset, total in cur.execute(
            f"""
            SELECT BestOffset, COUNT(*) AS total
            FROM {table}
            WHERE PrefetchDisable = 0
            GROUP BY BestOffset
            ORDER BY BestOffset
            """
        )
    }
    con.close()
    return counts


def load_stats_selected_counts(stats_path: Path, prefix: str) -> dict[int, int]:
    counts: dict[int, int] = {}
    for line in stats_path.read_text().splitlines():
        token = f"{prefix}::"
        if not line.startswith(token):
            continue
        name, rest = line.split(token, 1)
        bucket = rest.split()[0]
        if not bucket or bucket.startswith(("samples", "mean", "stdev", "underflows", "overflows", "min_value", "max_value", "total")):
            continue
        try:
            offset = int(bucket)
            count = int(rest.split()[1])
        except ValueError:
            continue
        if count:
            counts[offset] = count
    return counts


def _apply_filter_payload(table: list[int], payload: tuple[list[int], int]) -> None:
    indexes, mask = payload
    for idx in indexes:
        table[idx] |= mask


def rr_hash(addr: int, cfg: BOPConfig) -> int:
    rr_idx_bits = int(math.log2(cfg.rr_size))
    line_addr = addr >> cfg.l_blk_size
    mask = cfg.rr_size - 1
    hash1 = line_addr & mask
    hash2 = (line_addr >> rr_idx_bits) & mask
    return (hash1 ^ hash2) & mask


def rr_tag(addr: int, cfg: BOPConfig) -> int:
    rr_idx_bits = int(math.log2(cfg.rr_size))
    line_addr = addr >> cfg.l_blk_size
    return (line_addr >> rr_idx_bits) & cfg.tag_mask


def simulate_teacher_phases(rows: list[tuple[int, int, int]], cfg: BOPConfig) -> list[Phase]:
    rr = [None] * cfg.rr_size
    delay_queue = DelayedEventQueue(
        delay_ticks=cfg.teacher_delay_ticks,
        cycle_ticks=cfg.ticks_per_cycle,
        max_entries=cfg.teacher_delay_queue_size,
    )
    scores = [0] * len(cfg.offsets)
    offset_idx = 0
    roundn = 0
    best_score = 0
    phase_best = 0
    phase_start_pos = 0
    phases: list[Phase] = []

    for pos, (row_id, tick, addr) in enumerate(rows):
        delay_queue.drain_until(
            tick,
            lambda old_addr: rr.__setitem__(rr_hash(old_addr, cfg), (rr_tag(old_addr, cfg), old_addr)),
        )
        delay_queue.push(tick, addr)

        offset = cfg.offsets[offset_idx]
        lookup = addr - offset * cfg.block_size
        ent = rr[rr_hash(lookup, cfg)]
        if ent is not None and ent[0] == rr_tag(lookup, cfg):
            scores[offset_idx] += 1
            if scores[offset_idx] > best_score:
                best_score = scores[offset_idx]
                phase_best = offset

        offset_idx += 1
        if offset_idx == len(cfg.offsets):
            offset_idx = 0
            roundn += 1
            if best_score >= cfg.score_max or roundn == cfg.round_max:
                phase_rows = rows[phase_start_pos : pos + 1]
                phases.append(
                    Phase(
                        phase_idx=len(phases),
                        start_row_id=phase_rows[0][0],
                        end_row_id=phase_rows[-1][0],
                        start_tick=phase_rows[0][1],
                        end_tick=phase_rows[-1][1],
                        rows=phase_rows,
                        teacher_best=phase_best,
                        teacher_best_score=best_score,
                        teacher_issue_enabled=(best_score > cfg.bad_score),
                    )
                )
                phase_start_pos = pos + 1
                scores = [0] * len(cfg.offsets)
                roundn = 0
                best_score = 0
                phase_best = 0

    return phases


def pick_best(pool: list[StudentEntry], cov: Counter) -> int:
    return max(
        range(len(pool)),
        key=lambda i: (cov[pool[i].offset], pool[i].conf, -abs(pool[i].offset), -pool[i].offset),
    )


def pick_worst(pool: list[StudentEntry], cov: Counter) -> int:
    return min(
        range(len(pool)),
        key=lambda i: (cov[pool[i].offset], pool[i].conf, abs(pool[i].offset), pool[i].offset),
    )


def pick_evict(pool: list[StudentEntry]) -> int:
    return min(
        range(len(pool)),
        key=lambda i: (pool[i].conf, pool[i].last_phase_cov, -abs(pool[i].offset), pool[i].offset),
    )


def simulate_phase_cov(
    rows: list[tuple[int, int, int]],
    pool: list[StudentEntry],
    cfg: BOPConfig,
    student_delay_ticks: int,
) -> Counter:
    cov = Counter()
    if not pool:
        return cov

    table = [0] * cfg.student_filter_entries
    pending = DelayedEventQueue(
        delay_ticks=student_delay_ticks,
        cycle_ticks=cfg.ticks_per_cycle,
        max_entries=None,
    )

    for _, tick, addr in rows:
        pending.drain_until(tick, lambda payload: _apply_filter_payload(table, payload))

        line_addr = addr >> cfg.l_blk_size
        query_indexes = hash_indexes(
            line_addr, cfg.student_filter_entries, cfg.student_hash_mode, cfg.student_hash_count
        )
        hit_mask = ~0
        for idx in query_indexes:
            hit_mask &= table[idx]

        for bit_idx, entry in enumerate(pool):
            if hit_mask & (1 << bit_idx):
                cov[entry.offset] += 1

        for bit_idx, entry in enumerate(pool):
            predicted = addr + entry.offset * cfg.block_size
            if predicted < 0:
                continue
            if (not cfg.cross_page) and (not same_page(addr, predicted, cfg.page_bytes)):
                continue
            indexes = hash_indexes(
                predicted >> cfg.l_blk_size,
                cfg.student_filter_entries,
                cfg.student_hash_mode,
                cfg.student_hash_count,
            )
            mask = 1 << bit_idx
            if student_delay_ticks == 0:
                for idx in indexes:
                    table[idx] |= mask
            else:
                pending.push(tick, (indexes, mask))

    return cov


def simulate_student_variant(
    phases: list[Phase],
    cfg: BOPConfig,
    student_delay_ticks: int,
) -> list[dict]:
    pool: list[StudentEntry] = []
    records: list[dict] = []

    for phase_idx, phase in enumerate(phases):
        if phase_idx > 0 and cfg.student_teacher_top_n != 0:
            incoming = phases[phase_idx - 1].teacher_best
            if incoming != 0 and all(entry.offset != incoming for entry in pool):
                if len(pool) >= cfg.student_pool_size:
                    victim_idx = pick_evict(pool)
                    del pool[victim_idx]
                pool.append(StudentEntry(offset=incoming))

        cov = simulate_phase_cov(phase.rows, pool, cfg, student_delay_ticks)
        pool_before = [entry.offset for entry in pool]

        if pool:
            best_idx = pick_best(pool, cov)
            worst_idx = pick_worst(pool, cov)
            student_best = pool[best_idx].offset
            student_best_cov = cov[student_best]
            student_ratio = student_best_cov / len(phase.rows)
            student_enable = student_ratio >= cfg.student_cov_threshold
            final_offset = student_best if student_enable else phase.teacher_best
            final_issue_enable = phase.teacher_issue_enabled or student_enable

            for idx, entry in enumerate(pool):
                update = 0.0
                if idx == best_idx:
                    update = 1.0
                if idx == worst_idx:
                    update = -1.0
                entry.conf = entry.conf * cfg.student_conf_alpha + update * (1.0 - cfg.student_conf_alpha)
                entry.last_phase_cov = cov[entry.offset]
        else:
            student_best = None
            student_best_cov = 0
            student_ratio = 0.0
            student_enable = False
            final_offset = phase.teacher_best
            final_issue_enable = phase.teacher_issue_enabled

        records.append(
            {
                "phase_idx": phase.phase_idx,
                "teacher_best": phase.teacher_best,
                "teacher_best_score": phase.teacher_best_score,
                "teacher_issue_enabled": phase.teacher_issue_enabled,
                "phase_len": len(phase.rows),
                "start_row_id": phase.start_row_id,
                "end_row_id": phase.end_row_id,
                "start_tick": phase.start_tick,
                "end_tick": phase.end_tick,
                "pool_before": pool_before,
                "cov_by_offset": {offset: cov[offset] for offset in pool_before},
                "student_best": student_best,
                "student_best_cov": student_best_cov,
                "student_ratio": student_ratio,
                "student_enable": student_enable,
                "final_offset_next_phase": final_offset,
                "final_issue_enable_next_phase": final_issue_enable,
            }
        )

    return records


def phase_offset_counts(records: list[dict], key: str) -> dict[int, int]:
    counter = Counter()
    for record in records:
        value = record[key]
        if value is not None:
            counter[int(value)] += 1
    return dict(sorted(counter.items()))


def average_abs_selected(records: list[dict], key: str) -> float:
    vals = [abs(record[key]) for record in records if record[key] is not None]
    return sum(vals) / len(vals) if vals else 0.0


def main() -> None:
    repo = Path(__file__).resolve().parents[3]
    workdir = repo
    db_path = workdir / "test/libq_15361_nol2NL_studentBop/bop_trace.db"
    config_path = workdir / "test/libq_15361_nol2NL_studentBop/config.ini"
    stats_path = workdir / "test/libq_15361_nol2NL_studentBop/stats.txt"
    outdir = Path(__file__).resolve().parent

    cfg = load_config(config_path, "system.l2_wrappers.prefetcher.bop_large")
    rows = load_train_rows(db_path, "vbopTrainTraceTable")
    phases = simulate_teacher_phases(rows, cfg)

    baseline = simulate_student_variant(phases, cfg, student_delay_ticks=0)
    delayed = simulate_student_variant(phases, cfg, student_delay_ticks=cfg.teacher_delay_ticks)

    actual_issue_counts = load_actual_pf_offset_counts(db_path, "vbopPrefetchTraceTable")
    actual_student_selected = load_stats_selected_counts(
        stats_path, "system.l2_wrappers.prefetcher.bop_large.studentSelectedOffsetDist"
    )

    changed_winner = []
    changed_final = []
    longer_final = []
    shorter_final = []
    for base, delay in zip(baseline, delayed):
        if base["student_best"] != delay["student_best"]:
            changed_winner.append(base["phase_idx"])
        if base["final_offset_next_phase"] != delay["final_offset_next_phase"]:
            changed_final.append(base["phase_idx"])
            if (
                base["final_offset_next_phase"] is not None
                and delay["final_offset_next_phase"] is not None
            ):
                if abs(delay["final_offset_next_phase"]) > abs(base["final_offset_next_phase"]):
                    longer_final.append(base["phase_idx"])
                elif abs(delay["final_offset_next_phase"]) < abs(base["final_offset_next_phase"]):
                    shorter_final.append(base["phase_idx"])

    summary = {
        "db_path": str(db_path),
        "config_path": str(config_path),
        "teacher_phase_count": len(phases),
        "teacher_phase_lengths": dict(sorted(Counter(len(phase.rows) for phase in phases).items())),
        "teacher_best_counts": phase_offset_counts(
            [{"teacher_best": phase.teacher_best} for phase in phases], "teacher_best"
        ),
        "actual_issue_offset_counts_from_trace": actual_issue_counts,
        "actual_student_selected_counts_from_stats": actual_student_selected,
        "baseline_student_best_counts": phase_offset_counts(baseline, "student_best"),
        "baseline_final_offset_counts": phase_offset_counts(baseline, "final_offset_next_phase"),
        "delayed_student_best_counts": phase_offset_counts(delayed, "student_best"),
        "delayed_final_offset_counts": phase_offset_counts(delayed, "final_offset_next_phase"),
        "baseline_avg_abs_student_best": average_abs_selected(baseline, "student_best"),
        "baseline_avg_abs_final_offset": average_abs_selected(baseline, "final_offset_next_phase"),
        "delayed_avg_abs_student_best": average_abs_selected(delayed, "student_best"),
        "delayed_avg_abs_final_offset": average_abs_selected(delayed, "final_offset_next_phase"),
        "phases_with_changed_student_winner": changed_winner,
        "phases_with_changed_final_offset": changed_final,
        "phases_with_longer_final_offset_under_delay": longer_final,
        "phases_with_shorter_final_offset_under_delay": shorter_final,
        "student_delay_cycles": int(cfg.teacher_delay_ticks / 333),
        "student_delay_ticks": cfg.teacher_delay_ticks,
    }

    summary_path = outdir / "summary.json"
    phase_summary_path = outdir / "phase_winners.csv"
    phase_cov_path = outdir / "phase_cov_long.csv"

    summary_path.write_text(json.dumps(summary, indent=2, sort_keys=True))

    with phase_summary_path.open("w", newline="") as fh:
        writer = csv.writer(fh)
        writer.writerow(
            [
                "phase_idx",
                "phase_len",
                "teacher_best",
                "teacher_best_score",
                "teacher_issue_enabled",
                "baseline_pool",
                "baseline_covs",
                "baseline_student_best",
                "baseline_student_cov",
                "baseline_student_ratio",
                "baseline_student_enable",
                "baseline_final_offset",
                "delayed_pool",
                "delayed_covs",
                "delayed_student_best",
                "delayed_student_cov",
                "delayed_student_ratio",
                "delayed_student_enable",
                "delayed_final_offset",
                "winner_changed",
                "final_changed",
            ]
        )
        for base, delay in zip(baseline, delayed):
            writer.writerow(
                [
                    base["phase_idx"],
                    base["phase_len"],
                    base["teacher_best"],
                    base["teacher_best_score"],
                    int(base["teacher_issue_enabled"]),
                    " ".join(str(x) for x in base["pool_before"]),
                    " ".join(f"{off}:{base['cov_by_offset'][off]}" for off in base["pool_before"]),
                    base["student_best"],
                    base["student_best_cov"],
                    f"{base['student_ratio']:.6f}",
                    int(base["student_enable"]),
                    base["final_offset_next_phase"],
                    " ".join(str(x) for x in delay["pool_before"]),
                    " ".join(f"{off}:{delay['cov_by_offset'][off]}" for off in delay["pool_before"]),
                    delay["student_best"],
                    delay["student_best_cov"],
                    f"{delay['student_ratio']:.6f}",
                    int(delay["student_enable"]),
                    delay["final_offset_next_phase"],
                    int(base["student_best"] != delay["student_best"]),
                    int(base["final_offset_next_phase"] != delay["final_offset_next_phase"]),
                ]
            )

    with phase_cov_path.open("w", newline="") as fh:
        writer = csv.writer(fh)
        writer.writerow(["phase_idx", "variant", "offset", "cov", "phase_len", "cov_ratio", "rank"])
        for variant_name, records in [("baseline", baseline), ("delay300", delayed)]:
            for record in records:
                ranking = sorted(
                    record["cov_by_offset"].items(),
                    key=lambda item: (-item[1], -abs(item[0]), -item[0]),
                )
                rank_map = {offset: rank for rank, (offset, _) in enumerate(ranking, start=1)}
                for offset, cov in ranking:
                    writer.writerow(
                        [
                            record["phase_idx"],
                            variant_name,
                            offset,
                            cov,
                            record["phase_len"],
                            f"{cov / record['phase_len']:.6f}",
                            rank_map[offset],
                        ]
                    )

    print("teacher_phase_count", len(phases))
    print("actual_student_selected_counts", actual_student_selected)
    print("baseline_student_best_counts", phase_offset_counts(baseline, "student_best"))
    print("baseline_final_offset_counts", phase_offset_counts(baseline, "final_offset_next_phase"))
    print("delayed_student_best_counts", phase_offset_counts(delayed, "student_best"))
    print("delayed_final_offset_counts", phase_offset_counts(delayed, "final_offset_next_phase"))
    print("changed_student_winner_phases", changed_winner)
    print("changed_final_offset_phases", changed_final)
    print("longer_final_offset_under_delay", longer_final)
    print("shorter_final_offset_under_delay", shorter_final)
    print("wrote", summary_path)
    print("wrote", phase_summary_path)
    print("wrote", phase_cov_path)


if __name__ == "__main__":
    main()
