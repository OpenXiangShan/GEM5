#!/usr/bin/env python3

import argparse
import json
import math
import pathlib
import re
import sqlite3
from collections import Counter, deque
from dataclasses import asdict, dataclass, field, replace
from typing import Dict, Iterable, List, Optional, Sequence, Tuple


MASK64 = (1 << 64) - 1
BLOCK_SIZE = 64
BLOCK_BITS = 6
PAGE_SIZE = 4096
HWP_BOP_SOURCE = 4

PBOP_OFFSETS = [
    value
    for magnitude in [1, 2, 3, 4, 5, 6, 8, 9, 10, 12, 15, 16, 18, 20, 24, 25, 27, 30]
    for value in (magnitude, -magnitude)
] + [-32]

VBOP_OFFSETS = [
    -117, -147, -91, 117, 147, 91,
    -256, -250, -243, -240, -225, -216, -200,
    -192, -180, -162, -160, -150, -144, -135, -128,
    -125, -120, -108, -100, -96, -90, -81, -80,
    -75, -72, -64, -60, -54, -50, -48, -45,
    -40, -36, 32, 36, 40, 45, 48,
    50, 54, 60, 64, 72, 75, 80, 81,
    90, 96, 100, 108, 120, 125, 128, 135,
    144, 150, 160, 162, 180, 192, 200, 216,
    225, 240, 243, 250,
]


def merge_teacher_offsets(
    vbop_offsets: Sequence[int], pbop_offsets: Sequence[int]
) -> List[int]:
    vbop = list(vbop_offsets)
    body_start = vbop.index(min(vbop)) if vbop else 0
    return vbop[:body_start] + sorted(vbop[body_start:] + list(pbop_offsets))


# Teacher order affects tie-breaking. Preserve VBOP's historical priority
# prefix and derive the sorted body from the two family lists.
MERGED_OFFSETS = merge_teacher_offsets(VBOP_OFFSETS, PBOP_OFFSETS)


def block_address(addr: int) -> int:
    return addr & ~(BLOCK_SIZE - 1)


def same_page(lhs: int, rhs: int) -> bool:
    return (lhs // PAGE_SIZE) == (rhs // PAGE_SIZE)


def low_mask(bits: int) -> int:
    return MASK64 if bits >= 64 else (1 << bits) - 1


def fold_bit_groups(value: int, high: int, low: int, width: int) -> int:
    folded = 0
    bit = low
    while bit + width - 1 <= high:
        folded ^= (value >> bit) & low_mask(width)
        bit += width
    if bit <= high:
        folded ^= (value >> bit) & low_mask(high - bit + 1)
    return folded & low_mask(width)


@dataclass(frozen=True)
class Access:
    seq: int
    tick: int
    addr: int
    context_id: int
    has_context_id: bool
    secure: bool
    cache_miss: bool
    late_mshr: bool
    late_pfq: bool
    pf_source: int
    pf_first_hit: bool
    vbop_rr_insert_count: Optional[int]
    pbop_rr_insert_count: Optional[int]

    @property
    def key(self) -> Tuple[int, bool, int]:
        context = self.context_id if self.has_context_id else -1
        return context, self.secure, block_address(self.addr)


@dataclass(frozen=True)
class Candidate:
    seq: int
    tick: int
    addr: int
    context_id: int
    has_context_id: bool
    secure: bool
    offset: int
    rank: int
    role: str
    bop_kind: str
    accepted: bool

    @property
    def key(self) -> Tuple[int, bool, int]:
        context = self.context_id if self.has_context_id else -1
        return context, self.secure, block_address(self.addr)


@dataclass(frozen=True)
class SentCandidate:
    candidate: Candidate
    send_tick: int


@dataclass
class StudentEntry:
    offset: int
    conf: float = 0.0
    last_cov: int = 0
    cur_cov: int = 0


@dataclass(frozen=True)
class BopConfig:
    name: str
    offsets: Tuple[int, ...]
    bad_score: int
    cross_page: bool
    student_pool_size: int
    student_cov_threshold: float
    student_filter_entries: int
    student_hash_mode: str
    student_hash_addr_bits: int
    student_delay_enabled: bool
    student_delay_size: int
    degree: int = 1
    extra_policy: str = "raw_rank"
    extra_threshold: float = 0.0
    teacher_top_n: int = 1
    score_max: int = 31
    round_max: int = 50
    rr_entries: int = 256
    tag_bits: int = 12
    delay_queue_size: int = 16
    delay_cycles: int = 300
    student_delay_cycles: int = 300
    issue_conf_threshold: int = 14
    student_conf_alpha: float = 0.0


def vbop_config(clock_period_ticks: int) -> BopConfig:
    del clock_period_ticks
    return BopConfig(
        name="VBOP",
        offsets=tuple(VBOP_OFFSETS),
        bad_score=2,
        cross_page=True,
        student_pool_size=4,
        student_cov_threshold=0.0,
        student_filter_entries=4096,
        student_hash_mode="pairs_low9",
        student_hash_addr_bits=50,
        student_delay_enabled=True,
        student_delay_size=64,
    )


def pbop_config(clock_period_ticks: int) -> BopConfig:
    del clock_period_ticks
    return BopConfig(
        name="PBOP",
        offsets=tuple(PBOP_OFFSETS),
        bad_score=1,
        cross_page=False,
        student_pool_size=8,
        student_cov_threshold=0.05,
        student_filter_entries=2048,
        student_hash_mode="bop_rr",
        student_hash_addr_bits=48,
        student_delay_enabled=False,
        student_delay_size=16,
    )


def merged_config(
    degree: int,
    extra_policy: str,
    extra_threshold: float,
    teacher_top_n: int,
) -> BopConfig:
    if degree < 1 or degree >= 4:
        raise ValueError("degree must satisfy 1 <= degree < student pool size 4")
    if extra_policy not in ("raw_rank", "greedy_marginal"):
        raise ValueError(f"unsupported extra policy: {extra_policy}")
    return BopConfig(
        name="MergedVBOP",
        offsets=tuple(MERGED_OFFSETS),
        bad_score=2,
        cross_page=True,
        student_pool_size=4,
        student_cov_threshold=0.0,
        student_filter_entries=4096,
        student_hash_mode="pairs_low9",
        student_hash_addr_bits=50,
        student_delay_enabled=True,
        student_delay_size=64,
        degree=degree,
        extra_policy=extra_policy,
        extra_threshold=extra_threshold,
        teacher_top_n=teacher_top_n,
    )


class BopReplay:
    def __init__(self, config: BopConfig, clock_period_ticks: int):
        self.cfg = config
        self.clock_period_ticks = clock_period_ticks
        self.delay_ticks = config.delay_cycles * clock_period_ticks
        self.student_delay_ticks = config.student_delay_cycles * clock_period_ticks
        self.tag_mask = (1 << config.tag_bits) - 1
        self.rr_left = [(0, 0) for _ in range(config.rr_entries)]
        self.rr_right = [(0, 0) for _ in range(config.rr_entries)]
        self.delay_queue: deque[Tuple[int, int, int]] = deque()
        self.next_delay_event: Optional[int] = None

        self.scores = [0 for _ in config.offsets]
        self.offset_index = 0
        self.best_offset = config.offsets[-1]
        self.phase_best_offset = 0
        self.best_score = 0
        self.round = 0
        self.issue_prefetch = False
        self.issue_confidence = 0

        self.student_pool: List[StudentEntry] = []
        self.student_filter = [0 for _ in range(config.student_filter_entries)]
        self.student_delay_queue: deque[Tuple[int, Tuple[int, ...], int]] = deque()
        self.student_exact_seen: set[int] = set()
        self.student_phase_train_count = 0
        self.student_mask_hist = [0 for _ in range(1 << config.student_pool_size)]
        self.student_selected_offsets: List[int] = []
        self.student_selected_valid = False
        self.student_selected_enable = False

        self.stats: Counter[str] = Counter()
        self.phase_records: List[Dict] = []

    def _rr_index(self, addr: int) -> int:
        line_addr = addr >> BLOCK_BITS
        mask = self.cfg.rr_entries - 1
        idx_bits = int(math.log2(self.cfg.rr_entries))
        return ((line_addr & mask) ^ ((line_addr >> idx_bits) & mask)) & mask

    def _rr_tag(self, addr: int) -> int:
        line_addr = addr >> BLOCK_BITS
        idx_bits = int(math.log2(self.cfg.rr_entries))
        return (line_addr >> idx_bits) & self.tag_mask

    def _insert_rr(self, full_addr: int, tag: int) -> None:
        self.rr_left[self._rr_index(full_addr)] = (full_addr, tag)
        self.stats["rr_writes"] += 1

    def _test_rr(self, addr: int) -> bool:
        tag = self._rr_tag(addr)
        idx = self._rr_index(addr)
        return self.rr_left[idx][1] == tag or self.rr_right[idx][1] == tag

    def _run_delay_event(self, event_tick: int) -> None:
        if not self.delay_queue:
            self.next_delay_event = None
            return
        full_addr, tag, ready_tick = self.delay_queue[0]
        if ready_tick <= event_tick:
            self.delay_queue.popleft()
            self._insert_rr(full_addr, tag)
        if not self.delay_queue:
            self.next_delay_event = None
        elif self.delay_queue[0][2] <= event_tick:
            self.next_delay_event = event_tick + self.clock_period_ticks
        else:
            self.next_delay_event = self.delay_queue[0][2]

    def _drain_delay_events(self, tick: int) -> None:
        # The scheduled RR event has lower same-tick ordering than the L2
        # access that drives BOP training. Make entries visible only after all
        # accesses at their ready tick have been observed.
        while self.next_delay_event is not None and self.next_delay_event < tick:
            event_tick = self.next_delay_event
            self._run_delay_event(event_tick)

    def _drain_rr_to_count(self, target_count: int) -> None:
        current_count = self.stats["rr_writes"]
        if target_count < current_count:
            raise ValueError(
                f"{self.cfg.name}: RR insert count moved backwards: "
                f"{current_count} -> {target_count}"
            )
        while self.stats["rr_writes"] < target_count:
            if not self.delay_queue:
                raise ValueError(
                    f"{self.cfg.name}: trace exposes RR insert "
                    f"{target_count} before the replay queue has an entry"
                )
            full_addr, tag, _ = self.delay_queue.popleft()
            self._insert_rr(full_addr, tag)
        if not self.delay_queue:
            self.next_delay_event = None

    def _enqueue_rr(self, addr: int, tick: int) -> None:
        if len(self.delay_queue) >= self.cfg.delay_queue_size:
            self.stats["rr_delay_drops"] += 1
            return
        ready_tick = tick + self.delay_ticks
        self.delay_queue.append((addr, self._rr_tag(addr), ready_tick))
        if self.next_delay_event is None:
            self.next_delay_event = ready_tick

    def _student_hash_indexes(self, line_addr: int) -> Tuple[int, ...]:
        mask = self.cfg.student_filter_entries - 1
        if self.cfg.student_hash_mode == "bop_rr":
            bits = int(math.log2(self.cfg.student_filter_entries))
            base = ((line_addr & mask) ^ ((line_addr >> bits) & mask)) & mask
            return (base,)
        if self.cfg.student_hash_mode == "pairs_low9":
            raw_low_bits = 9
            hash_start_bit = 6
            index_bits = int(math.log2(self.cfg.student_filter_entries))
            hash_bits = index_bits - raw_low_bits
            line_addr_bits = self.cfg.student_hash_addr_bits - BLOCK_BITS
            rtl_line_addr = line_addr & low_mask(line_addr_bits)
            folded = fold_bit_groups(
                rtl_line_addr,
                line_addr_bits - 1,
                hash_start_bit,
                hash_bits,
            )
            return (((folded << raw_low_bits) | (rtl_line_addr & low_mask(raw_low_bits))) & mask,)
        raise ValueError(f"unsupported student hash mode: {self.cfg.student_hash_mode}")

    def _student_insert_mask(self, indexes: Iterable[int], mask: int) -> None:
        for index in indexes:
            self.student_filter[index] |= mask

    def _student_drain_delay(self, tick: int) -> None:
        while self.student_delay_queue and self.student_delay_queue[0][0] <= tick:
            _, indexes, mask = self.student_delay_queue.popleft()
            self._student_insert_mask(indexes, mask)
            self.stats["student_delay_drains"] += 1

    def _student_enqueue_prediction(self, train_addr: int, bit_index: int, offset: int, tick: int) -> None:
        predicted = train_addr + offset * BLOCK_SIZE
        if predicted < 0 or predicted > MASK64:
            return
        if not self.cfg.cross_page and not same_page(train_addr, predicted):
            return
        indexes = self._student_hash_indexes(predicted >> BLOCK_BITS)
        mask = 1 << bit_index
        if not self.cfg.student_delay_enabled:
            self._student_insert_mask(indexes, mask)
            return
        if len(self.student_delay_queue) >= self.cfg.student_delay_size:
            self.stats["student_delay_drops"] += 1
            return
        self.student_delay_queue.append((tick + self.student_delay_ticks, indexes, mask))
        self.stats["student_delay_inserts"] += 1

    def _student_observe(self, addr: int, tick: int) -> None:
        self.student_phase_train_count += 1
        if not self.student_pool:
            return
        if self.cfg.student_delay_enabled:
            self._student_drain_delay(tick)
        indexes = self._student_hash_indexes(addr >> BLOCK_BITS)
        hit_mask = (1 << len(self.student_pool)) - 1
        for index in indexes:
            hit_mask &= self.student_filter[index]
        hit_mask &= (1 << len(self.student_pool)) - 1
        self.student_mask_hist[hit_mask] += 1
        for bit_index, entry in enumerate(self.student_pool):
            if hit_mask & (1 << bit_index):
                entry.cur_cov += 1
        for bit_index, entry in enumerate(self.student_pool):
            self._student_enqueue_prediction(addr, bit_index, entry.offset, tick)

    def _student_best_index(self) -> int:
        return max(
            range(len(self.student_pool)),
            key=lambda index: (
                self.student_pool[index].cur_cov,
                self.student_pool[index].conf,
                -abs(self.student_pool[index].offset),
                -self.student_pool[index].offset,
            ),
        )

    def _student_worst_index(self) -> int:
        return min(
            range(len(self.student_pool)),
            key=lambda index: (
                self.student_pool[index].cur_cov,
                self.student_pool[index].conf,
                abs(self.student_pool[index].offset),
                self.student_pool[index].offset,
            ),
        )

    def _student_confidence_groups(self) -> List[List[int]]:
        return [list(range(len(self.student_pool)))]

    def _update_student_entry_confidence(self) -> None:
        updates = [0.0 for _ in self.student_pool]
        for indexes in self._student_confidence_groups():
            if not indexes:
                continue
            best_index = max(
                indexes,
                key=lambda index: (
                    self.student_pool[index].cur_cov,
                    self.student_pool[index].conf,
                    -abs(self.student_pool[index].offset),
                    -self.student_pool[index].offset,
                ),
            )
            worst_index = min(
                indexes,
                key=lambda index: (
                    self.student_pool[index].cur_cov,
                    self.student_pool[index].conf,
                    abs(self.student_pool[index].offset),
                    self.student_pool[index].offset,
                ),
            )
            updates[best_index] = 1.0
            updates[worst_index] = -1.0

        for index, entry in enumerate(self.student_pool):
            entry.conf = (
                entry.conf * self.cfg.student_conf_alpha
                + updates[index] * (1.0 - self.cfg.student_conf_alpha)
            )
            entry.last_cov = entry.cur_cov

    def _student_evict_index(self) -> int:
        return min(
            range(len(self.student_pool)),
            key=lambda index: (
                self.student_pool[index].conf,
                self.student_pool[index].last_cov,
                -abs(self.student_pool[index].offset),
                self.student_pool[index].offset,
            ),
        )

    def _raw_rank_indexes(self, primary_index: int) -> List[int]:
        remaining = [index for index in range(len(self.student_pool)) if index != primary_index]
        remaining.sort(
            key=lambda index: (
                self.student_pool[index].cur_cov,
                self.student_pool[index].conf,
                -abs(self.student_pool[index].offset),
                -self.student_pool[index].offset,
            ),
            reverse=True,
        )
        return remaining

    def _marginal_coverage(self, candidate_index: int, selected_mask: int) -> int:
        candidate_bit = 1 << candidate_index
        return sum(
            count
            for mask, count in enumerate(self.student_mask_hist)
            if count and (mask & selected_mask) == 0 and (mask & candidate_bit)
        )

    def _select_student_offsets(self) -> Tuple[List[int], List[int]]:
        primary_index = self._student_best_index()
        chosen = [primary_index]
        contributions = [self.student_pool[primary_index].cur_cov]
        selected_mask = 1 << primary_index
        while len(chosen) < min(self.cfg.degree, len(self.student_pool)):
            if self.cfg.extra_policy == "raw_rank":
                candidates = [index for index in self._raw_rank_indexes(primary_index) if index not in chosen]
                if not candidates:
                    break
                next_index = candidates[0]
                contribution = self.student_pool[next_index].cur_cov
            else:
                candidates = [index for index in range(len(self.student_pool)) if index not in chosen]
                if not candidates:
                    break
                scored = []
                for index in candidates:
                    marginal = self._marginal_coverage(index, selected_mask)
                    entry = self.student_pool[index]
                    scored.append((marginal, entry.conf, -abs(entry.offset), -entry.offset, index))
                contribution, _, _, _, next_index = max(scored)
            ratio = (
                contribution / self.student_phase_train_count
                if self.student_phase_train_count
                else 0.0
            )
            if ratio < self.cfg.extra_threshold:
                break
            chosen.append(next_index)
            contributions.append(contribution)
            selected_mask |= 1 << next_index
        return [self.student_pool[index].offset for index in chosen], contributions

    def _insert_teacher_winners(self, offsets: Sequence[int]) -> None:
        existing = {entry.offset for entry in self.student_pool}
        new_offsets: List[int] = []
        for offset in offsets[: self.cfg.teacher_top_n]:
            if offset == 0 or offset in existing or offset in new_offsets:
                continue
            new_offsets.append(offset)
        evict_count = max(
            0,
            len(self.student_pool) + len(new_offsets) - self.cfg.student_pool_size,
        )
        for _ in range(evict_count):
            del self.student_pool[self._student_evict_index()]
        for offset in new_offsets:
            self.student_pool.append(StudentEntry(offset=offset))
            self.stats["teacher_injected"] += 1
            self.stats[f"teacher_injected_offset::{offset}"] += 1

    def _clear_student_phase(self) -> None:
        self.student_filter = [0 for _ in range(self.cfg.student_filter_entries)]
        self.student_delay_queue.clear()
        self.student_exact_seen.clear()
        self.student_phase_train_count = 0
        self.student_mask_hist = [0 for _ in range(1 << self.cfg.student_pool_size)]
        for entry in self.student_pool:
            entry.cur_cov = 0

    def _student_phase_end(self, teacher_winners: Sequence[int]) -> None:
        self.stats["student_phases"] += 1
        record: Dict = {
            "phase": self.stats["student_phases"],
            "train_count": self.student_phase_train_count,
            "pool": [entry.offset for entry in self.student_pool],
            "coverage": [entry.cur_cov for entry in self.student_pool],
            "teacher_winners": list(teacher_winners),
        }
        if self.student_pool:
            best_index = self._student_best_index()
            selected_offsets, contributions = self._select_student_offsets()
            best_cov = self.student_pool[best_index].cur_cov
            ratio = best_cov / self.student_phase_train_count if self.student_phase_train_count else 0.0
            self.student_selected_offsets = selected_offsets
            self.student_selected_valid = True
            self.student_selected_enable = ratio >= self.cfg.student_cov_threshold
            if self.student_selected_enable:
                self.stats["student_issue_phases"] += 1
                self.stats[
                    f"student_selected_offset::{self.student_selected_offsets[0]}"
                ] += 1
            else:
                self.stats["student_fallback_phases"] += 1
            self._update_student_entry_confidence()
            record["selected_offsets"] = selected_offsets
            record["contributions"] = contributions
            record["primary_ratio"] = ratio
        else:
            self.student_selected_offsets = []
            self.student_selected_valid = False
            self.student_selected_enable = False
            self.stats["student_fallback_phases"] += 1
            record["selected_offsets"] = []
            record["contributions"] = []
            record["primary_ratio"] = 0.0
        self._insert_teacher_winners(teacher_winners)
        record["next_pool"] = [entry.offset for entry in self.student_pool]
        self.phase_records.append(record)
        self._clear_student_phase()

    def _rank_teacher_winners(self) -> List[int]:
        primary_winner = self.phase_best_offset
        ranked_indexes = sorted(
            (index for index, score in enumerate(self.scores) if score > 0),
            key=lambda index: (-self.scores[index], index),
        )
        winners = [primary_winner] if primary_winner != 0 else []
        winners.extend(
            self.cfg.offsets[index]
            for index in ranked_indexes
            if self.cfg.offsets[index] != primary_winner
        )
        return winners[: self.cfg.teacher_top_n]

    def _teacher_train(self, addr: int) -> Optional[List[int]]:
        index = self.offset_index
        offset = self.cfg.offsets[index]
        lookup_addr = (addr - offset * BLOCK_SIZE) & MASK64
        if self._test_rr(lookup_addr):
            self.scores[index] += 1
            if self.scores[index] > self.best_score:
                self.best_score = self.scores[index]
                self.phase_best_offset = offset
        self.offset_index += 1
        if self.offset_index != len(self.cfg.offsets):
            return None
        self.offset_index = 0
        self.round += 1
        if self.best_score < self.cfg.score_max and self.round != self.cfg.round_max:
            return None

        winners = self._rank_teacher_winners()
        if winners:
            self.stats[f"teacher_winner_offset::{winners[0]}"] += 1
        self.stats["teacher_rounds_sum"] += self.round
        self.stats["teacher_best_score_sum"] += self.best_score
        if self.best_score >= self.cfg.score_max:
            self.stats["teacher_score_max_phases"] += 1
        else:
            self.stats["teacher_round_max_phases"] += 1
        self.issue_prefetch = self.best_score > self.cfg.bad_score
        self.stats[
            "teacher_issue_enabled_phases"
            if self.issue_prefetch
            else "teacher_issue_disabled_phases"
        ] += 1
        self.best_offset = self.phase_best_offset
        self.stats["teacher_phases"] += 1
        self.round = 0
        self.best_score = 0
        self.phase_best_offset = 0
        self.scores = [0 for _ in self.scores]
        return winners

    def _issue_gate_allows(self) -> bool:
        return self.issue_confidence < self.cfg.issue_conf_threshold

    def _update_issue_confidence(self) -> None:
        if not self.issue_prefetch:
            self.issue_confidence = 15 if self.issue_confidence >= 13 else self.issue_confidence + 2
        else:
            self.issue_confidence = max(0, self.issue_confidence - 1)

    def step(self, access: Access) -> List[Candidate]:
        addr = block_address(access.addr)
        issue_gate = self._issue_gate_allows()
        rr_insert_count = (
            access.pbop_rr_insert_count
            if self.cfg.name == "PBOP"
            else access.vbop_rr_insert_count
        )
        if rr_insert_count is None:
            self._drain_delay_events(access.tick)
        else:
            self._drain_rr_to_count(rr_insert_count)
        self._enqueue_rr(addr, access.tick)
        teacher_winners = self._teacher_train(addr)
        self._student_observe(addr, access.tick)
        phase_end = teacher_winners is not None
        if phase_end:
            self._student_phase_end(teacher_winners)

        student_issue = self.student_selected_valid and self.student_selected_enable
        if student_issue:
            issue_offsets = list(self.student_selected_offsets)
        elif self.issue_prefetch:
            issue_offsets = [self.best_offset]
        else:
            issue_offsets = []
        do_issue = bool(issue_offsets) and issue_gate
        candidates: List[Candidate] = []
        if do_issue:
            for rank, offset in enumerate(issue_offsets):
                prefetch_addr = (addr + offset * BLOCK_SIZE) & MASK64
                accepted = self.cfg.cross_page or same_page(access.addr, prefetch_addr)
                role = "primary" if rank == 0 else "extra"
                candidates.append(
                    Candidate(
                        seq=access.seq,
                        tick=access.tick,
                        addr=prefetch_addr,
                        context_id=access.context_id,
                        has_context_id=access.has_context_id,
                        secure=access.secure,
                        offset=offset,
                        rank=rank,
                        role=role,
                        bop_kind=self.cfg.name,
                        accepted=accepted,
                    )
                )
                self.stats["generated"] += 1
                self.stats[f"generated_rank_{rank}"] += 1
                self.stats[f"issued_offset::{offset}"] += 1
                self.stats["accepted" if accepted else "cross_page_filtered"] += 1
        else:
            self.stats["throttled"] += 1
        if phase_end:
            self._update_issue_confidence()
        return candidates


@dataclass
class CoverageMetrics:
    demand_count: int = 0
    covered_count: int = 0
    generated_count: int = 0
    accepted_count: int = 0
    duplicate_count: int = 0
    covered_by_rank: Counter = field(default_factory=Counter)
    covered_by_bop: Counter = field(default_factory=Counter)

    def to_dict(self) -> Dict:
        result = asdict(self)
        result["coverage"] = self.covered_count / self.demand_count if self.demand_count else 0.0
        result["covered_by_rank"] = dict(sorted(self.covered_by_rank.items()))
        result["covered_by_bop"] = dict(sorted(self.covered_by_bop.items()))
        return result


@dataclass(frozen=True)
class ResourceConfig:
    name: str
    primary_fifo_size: int
    extra_fifo_size: int
    primary_slots: int
    extra_slots: int
    recent_filter_entries: int = 0
    role_mode: str = "rank"
    strict_primary: bool = False

    def __post_init__(self) -> None:
        if self.primary_fifo_size <= 0 or self.extra_fifo_size <= 0:
            raise ValueError("FIFO sizes must be positive")
        if self.primary_slots <= 0 or self.extra_slots <= 0:
            raise ValueError("arbiter slot counts must be positive")
        if self.recent_filter_entries < 0:
            raise ValueError("recent filter entries must not be negative")
        if self.role_mode not in ("rank", "bop_kind"):
            raise ValueError(
                "resource role mode must be rank or bop_kind"
            )


@dataclass
class ResourceMetrics:
    demand_count: int = 0
    covered_count: int = 0
    generated_count: int = 0
    accepted_count: int = 0
    duplicate_sent_count: int = 0
    fifo_late_demand_count: int = 0
    enqueued_by_role: Counter = field(default_factory=Counter)
    dropped_by_role: Counter = field(default_factory=Counter)
    sent_by_role: Counter = field(default_factory=Counter)
    duplicate_sent_by_role: Counter = field(default_factory=Counter)
    recent_filtered_by_role: Counter = field(default_factory=Counter)
    fifo_late_by_role: Counter = field(default_factory=Counter)
    covered_by_rank: Counter = field(default_factory=Counter)
    covered_by_role: Counter = field(default_factory=Counter)
    covered_with_min_lead_cycles: Counter = field(default_factory=Counter)
    queue_delay_cycles_sum: Counter = field(default_factory=Counter)
    queue_delay_cycles_max: Counter = field(default_factory=Counter)

    def to_dict(self) -> Dict:
        result = asdict(self)
        result["coverage"] = (
            self.covered_count / self.demand_count if self.demand_count else 0.0
        )
        for field_name in (
            "enqueued_by_role",
            "dropped_by_role",
            "sent_by_role",
            "duplicate_sent_by_role",
            "recent_filtered_by_role",
            "fifo_late_by_role",
            "covered_by_rank",
            "covered_by_role",
            "covered_with_min_lead_cycles",
            "queue_delay_cycles_sum",
            "queue_delay_cycles_max",
        ):
            result[field_name] = dict(sorted(getattr(self, field_name).items()))
        result["queue_delay_cycles_mean"] = {
            role: self.queue_delay_cycles_sum[role] / count
            for role, count in sorted(self.sent_by_role.items())
            if count
        }
        return result


class ConstrainedReplay:
    lead_thresholds = (0, 16, 32, 64, 128, 256)

    def __init__(
        self,
        config: ResourceConfig,
        clock_period_ticks: int,
        marker_tick: int,
        marker_seq: int,
    ):
        self.cfg = config
        self.clock_period_ticks = clock_period_ticks
        self.marker_tick = marker_tick
        self.marker_seq = marker_seq
        self.queues = {
            "primary": deque(),
            "extra": deque(),
        }
        self.capacities = {
            "primary": config.primary_fifo_size,
            "extra": config.extra_fifo_size,
        }
        self.schedule = (
            ["primary"] * config.primary_slots
            + ["extra"] * config.extra_slots
        )
        self.schedule_index = 0
        self.next_send_tick: Optional[int] = None
        self.outstanding: Dict[Tuple[int, bool, int], SentCandidate] = {}
        self.recent_filter_order: deque[Tuple[int, bool, int]] = deque()
        self.recent_filter_keys: set[Tuple[int, bool, int]] = set()
        self.metrics = ResourceMetrics()

    def _next_cycle_tick(self, tick: int) -> int:
        return (tick // self.clock_period_ticks + 1) * self.clock_period_ticks

    def _has_queued(self) -> bool:
        return bool(self.queues["primary"] or self.queues["extra"])

    def _dequeue(self) -> Optional[Candidate]:
        if self.cfg.strict_primary:
            role = "primary" if self.queues["primary"] else "extra"
            if not self.queues[role]:
                return None
            return self.queues[role].popleft()
        preferred = self.schedule[self.schedule_index]
        self.schedule_index = (self.schedule_index + 1) % len(self.schedule)
        alternate = "extra" if preferred == "primary" else "primary"
        role = preferred if self.queues[preferred] else alternate
        if not self.queues[role]:
            return None
        return self.queues[role].popleft()

    def _record_send(self, candidate: Candidate, send_tick: int) -> None:
        measuring = send_tick > self.marker_tick
        if measuring:
            self.metrics.sent_by_role[candidate.role] += 1
            delay_ticks = max(0, send_tick - candidate.tick)
            delay_cycles = (
                delay_ticks + self.clock_period_ticks - 1
            ) // self.clock_period_ticks
            self.metrics.queue_delay_cycles_sum[candidate.role] += delay_cycles
            self.metrics.queue_delay_cycles_max[candidate.role] = max(
                self.metrics.queue_delay_cycles_max[candidate.role],
                delay_cycles,
            )
        if candidate.key in self.outstanding:
            if measuring:
                self.metrics.duplicate_sent_count += 1
                self.metrics.duplicate_sent_by_role[candidate.role] += 1
            return
        self.outstanding[candidate.key] = SentCandidate(candidate, send_tick)

    def advance_before(self, tick: int) -> None:
        while self.next_send_tick is not None and self.next_send_tick < tick:
            send_tick = self.next_send_tick
            candidate = self._dequeue()
            if candidate is not None:
                self._record_send(candidate, send_tick)
            self.next_send_tick = (
                send_tick + self.clock_period_ticks
                if self._has_queued()
                else None
            )

    def observe_demand(self, access: Access, measuring: bool) -> None:
        sent = self.outstanding.pop(access.key, None)
        if not measuring:
            return
        self.metrics.demand_count += 1
        if sent is not None:
            candidate = sent.candidate
            self.metrics.covered_count += 1
            self.metrics.covered_by_rank[candidate.rank] += 1
            self.metrics.covered_by_role[candidate.role] += 1
            lead_cycles = max(
                0,
                (access.tick - sent.send_tick) // self.clock_period_ticks,
            )
            for threshold in self.lead_thresholds:
                if lead_cycles >= threshold:
                    self.metrics.covered_with_min_lead_cycles[threshold] += 1
            return

        late_roles = {
            role
            for role, queue in self.queues.items()
            if any(candidate.key == access.key for candidate in queue)
        }
        if late_roles:
            self.metrics.fifo_late_demand_count += 1
            for role in late_roles:
                self.metrics.fifo_late_by_role[role] += 1

    def _recent_filter_accepts(
        self,
        candidate: Candidate,
        measuring: bool,
    ) -> bool:
        capacity = self.cfg.recent_filter_entries
        if capacity == 0:
            return True
        if candidate.key in self.recent_filter_keys:
            if measuring:
                self.metrics.recent_filtered_by_role[candidate.role] += 1
            return False
        if len(self.recent_filter_order) >= capacity:
            evicted = self.recent_filter_order.popleft()
            self.recent_filter_keys.remove(evicted)
        self.recent_filter_order.append(candidate.key)
        self.recent_filter_keys.add(candidate.key)
        return True

    def _queue_role(self, candidate: Candidate) -> str:
        if self.cfg.role_mode == "rank":
            return candidate.role
        if candidate.bop_kind == "VBOP":
            return "primary"
        if candidate.bop_kind == "PBOP":
            return "extra"
        raise ValueError(
            f"cannot map BOP kind {candidate.bop_kind} to a resource role"
        )

    def enqueue(self, candidates: Sequence[Candidate], measuring: bool) -> None:
        for candidate in candidates:
            queue_role = self._queue_role(candidate)
            if queue_role != candidate.role:
                candidate = replace(candidate, role=queue_role)
            if measuring:
                self.metrics.generated_count += 1
            if not candidate.accepted:
                continue
            if measuring:
                self.metrics.accepted_count += 1
            if not self._recent_filter_accepts(candidate, measuring):
                continue
            queue = self.queues[candidate.role]
            if len(queue) >= self.capacities[candidate.role]:
                queue.popleft()
                if measuring:
                    self.metrics.dropped_by_role[candidate.role] += 1
            queue.append(candidate)
            if measuring:
                self.metrics.enqueued_by_role[candidate.role] += 1
            if self.next_send_tick is None:
                self.next_send_tick = self._next_cycle_tick(candidate.tick)


class SchemeTracker:
    def __init__(self, name: str, replays: Sequence[BopReplay]):
        self.name = name
        self.replays = list(replays)
        self.outstanding: Dict[Tuple[int, bool, int], Candidate] = {}
        self.metrics = CoverageMetrics()
        self.marker_stats: Optional[List[Counter]] = None
        self.resources: List[ConstrainedReplay] = []

    def attach_resources(
        self,
        configs: Sequence[ResourceConfig],
        clock_period_ticks: int,
        marker_tick: int,
        marker_seq: int,
    ) -> None:
        self.resources = [
            ConstrainedReplay(
                config,
                clock_period_ticks,
                marker_tick,
                marker_seq,
            )
            for config in configs
        ]

    def process(self, access: Access, measuring: bool) -> None:
        for resource in self.resources:
            resource.advance_before(access.tick)
            resource.observe_demand(access, measuring)
        candidate = self.outstanding.pop(access.key, None)
        if measuring:
            self.metrics.demand_count += 1
            if candidate is not None:
                self.metrics.covered_count += 1
                self.metrics.covered_by_rank[candidate.rank] += 1
                self.metrics.covered_by_bop[candidate.bop_kind] += 1
        candidates: List[Candidate] = []
        for replay in self.replays:
            candidates.extend(replay.step(access))
        for candidate in candidates:
            if measuring:
                self.metrics.generated_count += 1
                if candidate.accepted:
                    self.metrics.accepted_count += 1
            if not candidate.accepted:
                continue
            if candidate.key in self.outstanding:
                if measuring:
                    self.metrics.duplicate_count += 1
                continue
            self.outstanding[candidate.key] = candidate
        for resource in self.resources:
            resource.enqueue(candidates, measuring)

    def mark_boundary(self) -> None:
        self.marker_stats = [Counter(replay.stats) for replay in self.replays]

    def replay_stats(self) -> Dict:
        result = {}
        for index, replay in enumerate(self.replays):
            before = self.marker_stats[index] if self.marker_stats is not None else Counter()
            after = Counter(replay.stats)
            result[replay.cfg.name] = {
                "warmup": dict(before),
                "measurement": dict(after - before),
                "total": dict(after),
            }
        return result

    def resource_results(self) -> List[Dict]:
        return [
            {
                "name": resource.cfg.name,
                "config": asdict(resource.cfg),
                "metrics": resource.metrics.to_dict(),
            }
            for resource in self.resources
        ]


def read_clock_period(db_path: pathlib.Path, override: Optional[int]) -> int:
    if override is not None:
        return override
    config_path = db_path.parent / "config.ini"
    in_cpu_clock = False
    for line in config_path.read_text(errors="replace").splitlines():
        if line.startswith("[") and line.endswith("]"):
            in_cpu_clock = line == "[system.cpu_clk_domain]"
            continue
        if in_cpu_clock and line.startswith("clock="):
            try:
                return int(line.split("=", 1)[1])
            except ValueError:
                break
    raise ValueError(
        f"cannot read CPU clock period from {config_path}; "
        "pass --clock-period-ticks"
    )


def read_marker(connection: sqlite3.Connection) -> Tuple[int, int]:
    row = connection.execute(
        "SELECT Tick, AccessSeq FROM ArchDBTraceMarker "
        "WHERE Event='MeasurementStart' ORDER BY ID LIMIT 1"
    ).fetchone()
    if row is None:
        raise ValueError("MeasurementStart marker is missing")
    return int(row[0]), int(row[1])


def parse_resource_config(spec: str) -> ResourceConfig:
    parts = spec.split(":")
    if len(parts) not in (5, 6, 7, 8):
        raise argparse.ArgumentTypeError(
            "resource config must be NAME:PRIMARY_FIFO:EXTRA_FIFO:"
            "PRIMARY_SLOTS:EXTRA_SLOTS[:RECENT_FILTER_ENTRIES"
            "[:ROLE_MODE[:STRICT_PRIMARY]]]"
        )
    name = parts[0]
    if not name:
        raise argparse.ArgumentTypeError("resource config name must not be empty")
    try:
        primary_fifo_size = int(parts[1])
        extra_fifo_size = int(parts[2])
        primary_slots = int(parts[3])
        extra_slots = int(parts[4])
        recent_filter_entries = int(parts[5]) if len(parts) >= 6 else 0
        role_mode = parts[6] if len(parts) >= 7 else "rank"
        strict_primary_value = int(parts[7]) if len(parts) >= 8 else 0
        if strict_primary_value not in (0, 1):
            raise ValueError("STRICT_PRIMARY must be 0 or 1")
        return ResourceConfig(
            name,
            primary_fifo_size,
            extra_fifo_size,
            primary_slots,
            extra_slots,
            recent_filter_entries,
            role_mode,
            bool(strict_primary_value),
        )
    except ValueError as error:
        raise argparse.ArgumentTypeError(str(error)) from error


def iter_accesses(connection: sqlite3.Connection) -> Iterable[Access]:
    bop_columns = {
        row[1] for row in connection.execute("PRAGMA table_info(BOPInputTrace)")
    }
    has_rr_insert_count = "RRInsertCount" in bop_columns
    rr_columns = (
        "v.RRInsertCount, p.RRInsertCount"
        if has_rr_insert_count
        else "NULL, NULL"
    )
    query = f"""
        SELECT l.AccessSeq, l.Tick, v.TrainAddr,
               l.ContextId, l.HasContextId, l.Secure,
               l.CacheMiss, l.LateMSHR, l.LatePFQ,
               l.PfSource, l.PfFirstHit,
               {rr_columns}
        FROM L2AccessTrace AS l
        JOIN BOPInputTrace AS v ON v.AccessSeq = l.AccessSeq
            AND v.BOPKind = 'VBOP'
        JOIN BOPInputTrace AS p ON p.AccessSeq = l.AccessSeq
            AND p.BOPKind = 'PBOP'
        ORDER BY l.AccessSeq
    """
    for row in connection.execute(query):
        yield Access(
            seq=int(row[0]),
            tick=int(row[1]),
            addr=int(row[2]) & MASK64,
            context_id=int(row[3]),
            has_context_id=bool(row[4]),
            secure=bool(row[5]),
            cache_miss=bool(row[6]),
            late_mshr=bool(row[7]),
            late_pfq=bool(row[8]),
            pf_source=int(row[9]),
            pf_first_hit=bool(row[10]),
            vbop_rr_insert_count=(
                int(row[11]) if row[11] is not None else None
            ),
            pbop_rr_insert_count=(
                int(row[12]) if row[12] is not None else None
            ),
        )


def build_trackers(clock_period_ticks: int, matrix: bool) -> List[SchemeTracker]:
    trackers = [
        SchemeTracker(
            "dual_current",
            [
                BopReplay(vbop_config(clock_period_ticks), clock_period_ticks),
                BopReplay(pbop_config(clock_period_ticks), clock_period_ticks),
            ],
        )
    ]
    top_n_values = (1, 2) if matrix else (1,)
    thresholds = (0.0, 0.02, 0.03, 0.04, 0.05, 0.10) if matrix else (0.05,)
    for top_n in top_n_values:
        cfg = merged_config(1, "raw_rank", 0.0, top_n)
        trackers.append(
            SchemeTracker(
                f"merged_d1_top{top_n}",
                [BopReplay(cfg, clock_period_ticks)],
            )
        )
        for degree in (2, 3):
            for policy in ("raw_rank", "greedy_marginal"):
                for threshold in thresholds:
                    cfg = merged_config(degree, policy, threshold, top_n)
                    threshold_pct = int(round(threshold * 100))
                    name = f"merged_d{degree}_{policy}_th{threshold_pct}_top{top_n}"
                    trackers.append(
                        SchemeTracker(name, [BopReplay(cfg, clock_period_ticks)])
                    )
    return trackers


def parse_stats_blocks(path: pathlib.Path) -> List[Dict[str, float]]:
    if not path.exists():
        return []
    blocks: List[Dict[str, float]] = []
    current: Optional[Dict[str, float]] = None
    line_pattern = re.compile(r"^(\S+)\s+([-+]?\d+(?:\.\d+)?(?:[eE][-+]?\d+)?)\s+")
    for line in path.read_text(errors="replace").splitlines():
        if "Begin Simulation Statistics" in line:
            current = {}
            continue
        if "End Simulation Statistics" in line:
            if current is not None:
                blocks.append(current)
            current = None
            continue
        if current is None:
            continue
        match = line_pattern.match(line)
        if match:
            current[match.group(1)] = float(match.group(2))
    return blocks


def stats_validation(trackers: Sequence[SchemeTracker], stats_path: pathlib.Path) -> Dict:
    blocks = parse_stats_blocks(stats_path)
    dual = next(tracker for tracker in trackers if tracker.name == "dual_current")
    replay_stats = dual.replay_stats()
    fields = {
        "generated": "pfGenerated",
        "throttled": "throttledCount",
        "student_phases": "studentPhaseCount",
        "student_issue_phases": "studentIssueCount",
        "teacher_injected": "teacherInjectedCount",
    }
    output: Dict = {
        "stats_file": str(stats_path),
        "block_count": len(blocks),
        "comparisons": [],
        "distribution_mismatches": [],
    }
    if len(blocks) < 2:
        return output
    for block_index, interval in enumerate(("warmup", "measurement")):
        for bop_kind, stat_prefix in (("VBOP", "bop_large"), ("PBOP", "bop_small")):
            for replay_field, gem5_suffix in fields.items():
                gem5_name = f"system.l2_wrappers.prefetcher.{stat_prefix}.{gem5_suffix}"
                gem5_value = int(round(blocks[block_index].get(gem5_name, -1)))
                replay_value = int(replay_stats[bop_kind][interval].get(replay_field, 0))
                output["comparisons"].append(
                    {
                        "interval": interval,
                        "bop": bop_kind,
                        "field": replay_field,
                        "gem5": gem5_value,
                        "replay": replay_value,
                        "match": gem5_value == replay_value,
                    }
                )
            distribution_fields = {
                "issued_offset": ("issuedOffsetDist", "issued_offset::"),
                "student_selected_offset": (
                    "studentSelectedOffsetDist",
                    "student_selected_offset::",
                ),
                "teacher_injected_offset": (
                    "teacherInjectedOffsetDist",
                    "teacher_injected_offset::",
                ),
            }
            replay_interval = replay_stats[bop_kind][interval]
            for field_name, (gem5_dist, replay_prefix) in distribution_fields.items():
                gem5_prefix = (
                    f"system.l2_wrappers.prefetcher.{stat_prefix}."
                    f"{gem5_dist}::"
                )
                gem5_bins = {
                    int(name[len(gem5_prefix):]): int(round(value))
                    for name, value in blocks[block_index].items()
                    if name.startswith(gem5_prefix)
                    and re.fullmatch(r"-?\d+", name[len(gem5_prefix):])
                    and value
                }
                replay_bins = {
                    int(name[len(replay_prefix):]): int(value)
                    for name, value in replay_interval.items()
                    if name.startswith(replay_prefix) and value
                }
                for offset in sorted(set(gem5_bins) | set(replay_bins)):
                    gem5_value = gem5_bins.get(offset, 0)
                    replay_value = replay_bins.get(offset, 0)
                    if gem5_value != replay_value:
                        output["distribution_mismatches"].append(
                            {
                                "interval": interval,
                                "bop": bop_kind,
                                "field": field_name,
                                "offset": offset,
                                "gem5": gem5_value,
                                "replay": replay_value,
                            }
                        )
    output["scalar_all_match"] = all(
        item["match"] for item in output["comparisons"]
    )
    output["distribution_all_match"] = not output["distribution_mismatches"]
    output["all_match"] = (
        output["scalar_all_match"] and output["distribution_all_match"]
    )
    return output


def write_markdown(path: pathlib.Path, result: Dict) -> None:
    lines = [
        "# TS-BOP Degree Offline Replay",
        "",
        f"- DB: `{result['db']}`",
        f"- Measurement marker: tick `{result['marker']['tick']}`, access `{result['marker']['access_seq']}`",
        f"- L2 accesses: `{result['access_count']}`",
        f"- Actual HWP_BOP first-hit coverage: `{result['actual']['coverage']:.4%}`",
        "",
        "| scheme | ideal coverage | covered | demands | generated | accepted | duplicates |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for scheme in result["schemes"]:
        metrics = scheme["metrics"]
        lines.append(
            f"| {scheme['name']} | {metrics['coverage']:.4%} | "
            f"{metrics['covered_count']} | {metrics['demand_count']} | "
            f"{metrics['generated_count']} | {metrics['accepted_count']} | "
            f"{metrics['duplicate_count']} |"
        )
    resource_rows = [
        (scheme["name"], resource)
        for scheme in result["schemes"]
        for resource in scheme.get("resources", [])
    ]
    if resource_rows:
        lines.extend([
            "",
            "## FIFO And Arbitration Constraints",
            "",
            "Coverage here requires the candidate to leave its local FIFO before the demand.",
            "It still does not model memory latency, cache eviction, shared PFQ pressure, or closed-loop feedback.",
            "",
            (
                "| scheme | resource config | sent coverage | primary sent | "
                "extra sent | primary drop | extra drop | FIFO-late demands |"
            ),
            "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |",
        ])
        for scheme_name, resource in resource_rows:
            metrics = resource["metrics"]
            sent = metrics["sent_by_role"]
            dropped = metrics["dropped_by_role"]
            lines.append(
                f"| {scheme_name} | {resource['name']} | "
                f"{metrics['coverage']:.4%} | {sent.get('primary', 0)} | "
                f"{sent.get('extra', 0)} | {dropped.get('primary', 0)} | "
                f"{dropped.get('extra', 0)} | "
                f"{metrics['fifo_late_demand_count']} |"
            )
    validation = result.get("stats_validation", {})
    if validation:
        lines.extend([
            "",
            "## Replay Validation",
            "",
            f"All checked counters match: `{validation.get('all_match', False)}`",
            f"Offset distribution mismatches: `{len(validation.get('distribution_mismatches', []))}`",
            "",
            "| interval | BOP | field | gem5 | replay | match |",
            "| --- | --- | --- | ---: | ---: | --- |",
        ])
        for item in validation.get("comparisons", []):
            lines.append(
                f"| {item['interval']} | {item['bop']} | {item['field']} | "
                f"{item['gem5']} | {item['replay']} | {item['match']} |"
            )
        mismatches = validation.get("distribution_mismatches", [])
        if mismatches:
            lines.extend([
                "",
                "| interval | BOP | distribution | offset | gem5 | replay |",
                "| --- | --- | --- | ---: | ---: | ---: |",
            ])
            for item in mismatches:
                lines.append(
                    f"| {item['interval']} | {item['bop']} | "
                    f"{item['field']} | {item['offset']} | "
                    f"{item['gem5']} | {item['replay']} |"
                )
    path.write_text("\n".join(lines) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser(description="Replay TS-BOP degree schemes from ArchDB traces")
    parser.add_argument("--db", required=True, type=pathlib.Path)
    parser.add_argument("--out-json", required=True, type=pathlib.Path)
    parser.add_argument("--out-md", type=pathlib.Path)
    parser.add_argument("--stats", type=pathlib.Path)
    parser.add_argument("--clock-period-ticks", type=int)
    parser.add_argument("--matrix", action="store_true", help="run the full initial parameter matrix")
    parser.add_argument(
        "--scheme-regex",
        help="only replay matching schemes; dual_current is always retained for validation",
    )
    parser.add_argument(
        "--resource-config",
        action="append",
        default=[],
        type=parse_resource_config,
        metavar=(
            "NAME:PRIMARY_FIFO:EXTRA_FIFO:PRIMARY_SLOTS:EXTRA_SLOTS"
            "[:RECENT_FILTER_ENTRIES[:ROLE_MODE[:STRICT_PRIMARY]]]"
        ),
        help="attach a local FIFO/weighted-arbiter model; may be repeated",
    )
    parser.add_argument(
        "--resource-scheme-regex",
        default=r"^merged_",
        help="only attach resource models to matching scheme names",
    )
    args = parser.parse_args()

    clock_period_ticks = read_clock_period(args.db, args.clock_period_ticks)
    connection = sqlite3.connect(str(args.db))
    marker_tick, marker_seq = read_marker(connection)
    trackers = build_trackers(clock_period_ticks, args.matrix)
    if args.scheme_regex:
        scheme_pattern = re.compile(args.scheme_regex)
        matched_trackers = [
            tracker for tracker in trackers
            if scheme_pattern.search(tracker.name)
        ]
        if not matched_trackers:
            raise ValueError(f"scheme regex matched no schemes: {args.scheme_regex}")
        dual = next(
            tracker for tracker in trackers if tracker.name == "dual_current"
        )
        trackers = matched_trackers
        if dual not in trackers:
            trackers.insert(0, dual)
    if args.resource_config:
        resource_pattern = re.compile(args.resource_scheme_regex)
        matched = 0
        for tracker in trackers:
            if resource_pattern.search(tracker.name):
                tracker.attach_resources(
                    args.resource_config,
                    clock_period_ticks,
                    marker_tick,
                    marker_seq,
                )
                matched += 1
        if matched == 0:
            raise ValueError(
                f"resource scheme regex matched no schemes: {args.resource_scheme_regex}"
            )
    actual_demand_count = 0
    actual_covered_count = 0
    actual_late_count = 0
    access_count = 0
    for access in iter_accesses(connection):
        access_count += 1
        measuring = access.seq > marker_seq
        if measuring:
            actual_demand_count += 1
            if access.pf_first_hit and access.pf_source == HWP_BOP_SOURCE:
                actual_covered_count += 1
            if access.cache_miss and (access.late_mshr or access.late_pfq) and access.pf_source == HWP_BOP_SOURCE:
                actual_late_count += 1
        for tracker in trackers:
            tracker.process(access, measuring)
        if access.seq == marker_seq:
            for tracker in trackers:
                tracker.mark_boundary()
    connection.close()

    result = {
        "db": str(args.db),
        "clock_period_ticks": clock_period_ticks,
        "marker": {"tick": marker_tick, "access_seq": marker_seq},
        "access_count": access_count,
        "actual": {
            "demand_count": actual_demand_count,
            "covered_count": actual_covered_count,
            "late_count": actual_late_count,
            "coverage": actual_covered_count / actual_demand_count if actual_demand_count else 0.0,
        },
        "schemes": [
            {
                "name": tracker.name,
                "metrics": tracker.metrics.to_dict(),
                "replay_stats": tracker.replay_stats(),
                "resources": tracker.resource_results(),
            }
            for tracker in trackers
        ],
    }
    stats_path = args.stats or (args.db.parent / "stats.txt")
    result["stats_validation"] = stats_validation(trackers, stats_path)
    args.out_json.parent.mkdir(parents=True, exist_ok=True)
    args.out_json.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    if args.out_md:
        args.out_md.parent.mkdir(parents=True, exist_ok=True)
        write_markdown(args.out_md, result)
    print(json.dumps({
        "access_count": access_count,
        "actual_coverage": result["actual"]["coverage"],
        "schemes": {
            scheme["name"]: scheme["metrics"]["coverage"]
            for scheme in result["schemes"]
        },
        "stats_validation_all_match": result["stats_validation"].get("all_match"),
    }, indent=2, sort_keys=True))

if __name__ == "__main__":
    main()
