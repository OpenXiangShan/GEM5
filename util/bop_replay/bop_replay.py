#!/usr/bin/env python3
"""Replay the BOP admission controller against L2 demand-address oracle data.

The online trace deliberately ends before the BOP local prefetch filter.  This
tool therefore evaluates raw post-policy candidates only.  It does not model
filters, queues, MSHRs, fills, evictions, bandwidth, or cache residency.
"""

from __future__ import annotations

import argparse
import heapq
import json
import re
import resource
import sqlite3
import time
from collections import defaultdict, deque
from dataclasses import asdict, dataclass, field, replace
from pathlib import Path
from typing import Callable, Iterable, Iterator, Mapping, Sequence


SCHEMA_VERSION = 6
LEARNER_REPLAY_MIN_SCHEMA_VERSION = 3
LEARNER_REPLAY_CERTIFICATION_MIN_SCHEMA_VERSION = 5
UINT64_MASK = (1 << 64) - 1
PC_ASSOCIATIVITY = 4
GLOBAL_OUTCOME_WINDOW_SIZE = 512
GLOBAL_EWMA_SHIFT = 3
GLOBAL_IDLE_RESET_CHECKS = 4096
GLOBAL_UNUSED_EWMA_INITIAL = 255
PC_SHARED_CONFIG_FIELDS = (
    "pc_validation_entries",
    "pc_validation_tag_bits",
    "pc_validation_counter_bits",
    "pc_validation_initial",
    "pc_validation_medium_threshold",
    "pc_validation_high_threshold",
    "pc_validation_hit_increment",
    "pc_validation_medium_sample_period",
    "pc_validation_miss_decay_period",
    "pc_validation_low_entry_miss_streak_threshold",
    "pc_validation_epoch_bits",
    "pc_validation_offset_context_slots",
    "pc_validation_producer_consumer",
    "pc_validation_same_pc_hit_gate",
    "pc_validation_same_pc_hit_increment",
    "pc_validation_recovered_hit_increment",
    "pc_validation_recovered_same_pc_hit_gate",
    "pc_validation_recovered_probation",
    "pc_validation_recovered_admission",
    "pc_validation_recovered_producer_credit",
    "pc_validation_recovered_same_pc_credit",
    "pc_validation_recovered_cross_pc_credit",
    "global_coverage_guard",
    "global_bop_unused_threshold",
    "global_bop_min_resolved_coverage_shift",
)
LEARNER_REPORT_FIELDS = (
    "score_max",
    "round_max",
    "bad_score",
    "rr_entries",
    "tag_bits",
    "delay_queue_enabled",
    "delay_queue_size",
    "delay_ticks",
    "clock_period_ticks",
    "adapt_offset",
    "negative_offsets_enable",
    "auto_learning",
    "issue_validation",
    "pc_validation_confidence",
    "offsets",
    "student_cover_enabled",
    "student_pool_size",
    "student_conf_alpha",
    "student_cov_threshold",
    "student_teacher_top_n",
    "student_filter_entries",
    "student_hash_mode",
    "student_hash_count",
    "student_large_offset_priority",
    "student_large_offset_priority_coeff",
    "student_delay_queue_enabled",
    "student_delay_queue_size",
    "student_delay_ticks",
)
CONTROLLER_REPORT_FIELDS = (
    "pc_validation_entries",
    "pc_validation_tag_bits",
    "pc_validation_counter_bits",
    "pc_validation_initial",
    "pc_validation_medium_threshold",
    "pc_validation_high_threshold",
    "pc_validation_hit_increment",
    "pc_validation_medium_sample_period",
    "pc_validation_miss_decay_period",
    "pc_validation_low_entry_miss_streak_threshold",
    "pc_validation_epoch_bits",
    "pc_validation_offset_context_slots",
    "pc_validation_producer_consumer",
    "pc_validation_same_pc_hit_gate",
    "pc_validation_same_pc_hit_increment",
    "pc_validation_recovered_hit_increment",
    "pc_validation_recovered_same_pc_hit_gate",
    "pc_validation_recovered_probation",
    "pc_validation_recovered_admission",
    "pc_validation_recovered_producer_credit",
    "pc_validation_recovered_same_pc_credit",
    "pc_validation_recovered_cross_pc_credit",
    "global_coverage_guard",
    "global_bop_unused_threshold",
    "global_bop_min_resolved_coverage_shift",
)


@dataclass(frozen=True)
class Demand:
    access_seq: int
    tick: int
    addr: int
    phase_id: int = 0


@dataclass(frozen=True)
class ReplayEvent:
    access_seq: int
    order: int
    bop_name: str
    bop_kind: str
    tick: int
    trigger_addr: int
    trigger_pc: int
    trigger_has_pc: bool
    validation_hit: int
    best_offset_changed: bool
    issue_enabled: bool
    validation_enabled: bool
    pc_confidence_enabled: bool
    pc_sampled: bool
    raw_candidate_valid: bool
    raw_candidate_addr: int
    policy_candidate_valid: bool
    policy_candidate_addr: int
    late: bool = False
    best_offset_before: int = 0
    best_offset_after: int = 0
    teacher_issue_enabled: bool = False
    student_issue_enabled: bool = False
    student_selected_valid: bool = False
    student_selected_enable: bool = False
    student_selected_offset: int = 0
    selected_offset: int = 0
    best_score: int = 0
    round: int = 0
    online_generated: bool = False
    online_buffered: bool = False
    online_filtered: bool = False
    online_filter_passed: bool = False
    phase_id: int = 0
    replay_order: int = 0
    trigger_is_demand: bool = True
    trigger_is_read: bool = True


@dataclass(frozen=True)
class ReplayDelayAction:
    """One native BOP delay-queue state action, ordered per BOP instance."""

    bop_name: str
    replay_order: int
    action: str
    tick: int
    addr: int
    process_tick: int
    queue_size_after: int


@dataclass
class Candidate:
    candidate_id: int
    kind: str
    access_seq: int
    tick: int
    addr: int
    demand_index_at_issue: int
    phase_id: int = 0
    status: str = "pending"
    matched_demand_seq: int | None = None
    demand_distance: int | None = None
    tick_distance: int | None = None


@dataclass(frozen=True)
class ReplayPhase:
    phase_id: int
    name: str
    start_tick: int


@dataclass(frozen=True)
class EvaluationWindow:
    """A reporting window selected after full-trace state replay."""

    name: str = "full"
    source: str = "full"
    phase_id: int | None = None
    phase_name: str | None = None
    start_tick: int | None = None
    stats_path: str | None = None
    stats_block: int | None = None


@dataclass(frozen=True)
class BOPConfig:
    schema_version: int = SCHEMA_VERSION
    bop_name: str = "large"
    block_size: int = 64
    score_max: int = 20
    round_max: int = 50
    bad_score: int = 12
    rr_entries: int = 256
    tag_bits: int = 24
    delay_queue_enabled: bool = True
    delay_queue_size: int = 64
    delay_ticks: int = 0
    cross_page: bool = True
    adapt_offset: bool = True
    issue_validation: bool = False
    pc_validation_confidence: bool = False
    pc_validation_producer_consumer: bool = False
    # Offline-only P/C experiment knobs.  Defaults retain the online policy.
    pc_validation_same_pc_hit_gate: bool = False
    # Zero uses pc_validation_hit_increment for same-PC producer credit.
    pc_validation_same_pc_hit_increment: int = 0
    # Offline-only recovered-LRU evidence controls.  They have no effect for
    # native RR hits or the normal event-only controller replay.
    # Zero uses pc_validation_hit_increment for compatible recovered credit.
    pc_validation_recovered_hit_increment: int = 0
    pc_validation_recovered_same_pc_hit_gate: bool = False
    pc_validation_recovered_probation: bool = False
    # Offline-only recovered-LRU factor controls. They separate the direct
    # admission effect of recovered evidence from producer confidence credit.
    # Defaults retain the existing recovered-hit behavior.
    pc_validation_recovered_admission: bool = True
    pc_validation_recovered_producer_credit: bool = True
    pc_validation_recovered_same_pc_credit: bool = True
    pc_validation_recovered_cross_pc_credit: bool = True
    negative_offsets_enable: bool = False
    auto_learning: bool = False
    victim_offsets_list_size: int = 10
    restore_cycle: int = 250000
    clock_period_ticks: int = 0
    offsets: tuple[int, ...] = ()
    pc_validation_entries: int = 128
    pc_validation_tag_bits: int = 16
    pc_validation_counter_bits: int = 2
    pc_validation_initial: int = 0
    pc_validation_medium_threshold: int = 1
    pc_validation_high_threshold: int = 2
    pc_validation_hit_increment: int = 1
    pc_validation_medium_sample_period: int = 4
    pc_validation_miss_decay_period: int = 8
    pc_validation_low_entry_miss_streak_threshold: int = 0
    pc_validation_epoch_bits: int = 4
    # Zero selects exact legacy epoch semantics for pre-context V5 traces.
    pc_validation_offset_context_slots: int = 0
    global_coverage_guard: bool = False
    global_bop_unused_threshold: int = 38
    global_bop_min_resolved_coverage_shift: int = 3
    student_cover_enabled: bool = False
    student_pool_size: int = 0
    student_conf_alpha: float = 0.0
    student_cov_threshold: float = 0.0
    student_teacher_top_n: int = 1
    student_filter_entries: int = 0
    student_hash_mode: str = "splitmix"
    student_hash_count: int = 1
    student_large_offset_priority: bool = False
    student_large_offset_priority_coeff: float = 0.99
    student_delay_queue_enabled: bool = False
    student_delay_queue_size: int = 0
    student_delay_ticks: int = 0
    learner_configs: Mapping[str, "BOPConfig"] = field(default_factory=dict, compare=False)
    replay_delay_actions: Mapping[str, tuple[ReplayDelayAction, ...]] = field(
        default_factory=dict, compare=False
    )
    trace_delay_queue_enabled: bool | None = None
    trace_delay_queue_size: int | None = None
    trace_delay_ticks: int | None = None

    @classmethod
    def from_meta(cls, row: Mapping[str, object]) -> "BOPConfig":
        def value(name: str, default: object) -> object:
            try:
                return row[name]
            except (KeyError, IndexError):
                return default

        delay_queue_enabled = bool(value("DelayQueueEnabled", False))
        delay_queue_size = int(value("DelayQueueSize", 0))
        delay_ticks = int(value("DelayTicks", 0))
        return cls(
            schema_version=int(value("SchemaVersion", 1)),
            bop_name=str(value("BOPName", "large")),
            block_size=int(row["BlockSize"]),
            score_max=int(value("ScoreMax", 20)),
            round_max=int(value("RoundMax", 50)),
            bad_score=int(value("BadScore", 12)),
            rr_entries=int(value("RREntries", 256)),
            tag_bits=int(value("TagBits", 24)),
            delay_queue_enabled=delay_queue_enabled,
            delay_queue_size=delay_queue_size,
            delay_ticks=delay_ticks,
            cross_page=bool(value("CrossPage", True)),
            adapt_offset=bool(value("AdaptOffset", False)),
            issue_validation=bool(value("IssueValidation", False)),
            pc_validation_confidence=bool(
                value("PCValidationConfidence", False)
            ),
            pc_validation_producer_consumer=bool(
                value("PCValidationProducerConsumer", False)
            ),
            negative_offsets_enable=bool(value("NegativeOffsetsEnabled", False)),
            auto_learning=bool(value("AutoLearning", False)),
            victim_offsets_list_size=int(value("VictimOffsetsListSize", 10)),
            restore_cycle=int(value("RestoreCycle", 250000)),
            clock_period_ticks=int(value("ClockPeriodTicks", 0)),
            offsets=tuple(int(item) for item in str(value("Offsets", "")).split(",") if item),
            pc_validation_entries=int(row["PCValidationEntries"]),
            pc_validation_tag_bits=int(row["PCValidationTagBits"]),
            pc_validation_counter_bits=int(row["PCValidationCounterBits"]),
            pc_validation_initial=int(row["PCValidationInitial"]),
            pc_validation_medium_threshold=int(
                row["PCValidationMediumThreshold"]),
            pc_validation_high_threshold=int(row["PCValidationHighThreshold"]),
            pc_validation_hit_increment=int(row["PCValidationHitIncrement"]),
            pc_validation_medium_sample_period=int(
                row["PCValidationMediumSamplePeriod"]),
            pc_validation_miss_decay_period=int(row["PCValidationMissDecayPeriod"]),
            pc_validation_low_entry_miss_streak_threshold=int(
                row["PCValidationLowEntryMissStreakThreshold"]),
            pc_validation_epoch_bits=int(row["PCValidationEpochBits"]),
            pc_validation_offset_context_slots=int(
                value("PCValidationOffsetContextSlots", 0)
            ),
            global_coverage_guard=bool(row["GlobalCoverageGuard"]),
            global_bop_unused_threshold=int(row["GlobalBOPUnusedThreshold"]),
            global_bop_min_resolved_coverage_shift=int(
                row["GlobalBOPMinResolvedCoverageShift"]),
            student_cover_enabled=bool(value("StudentCoverEnabled", False)),
            student_pool_size=int(value("StudentPoolSize", 0)),
            student_conf_alpha=float(value("StudentConfAlpha", 0.0)),
            student_cov_threshold=float(value("StudentCovThreshold", 0.0)),
            student_teacher_top_n=int(value("StudentTeacherTopN", 1)),
            student_filter_entries=int(value("StudentFilterEntries", 0)),
            student_hash_mode=str(value("StudentHashMode", "splitmix")),
            student_hash_count=int(value("StudentHashCount", 1)),
            student_large_offset_priority=bool(
                value("StudentLargeOffsetPriority", False)),
            student_large_offset_priority_coeff=float(
                value("StudentLargeOffsetPriorityCoeff", 0.99)),
            student_delay_queue_enabled=bool(
                value("StudentDelayQueueEnabled", False)),
            student_delay_queue_size=int(value("StudentDelayQueueSize", 0)),
            student_delay_ticks=int(value("StudentDelayTicks", 0)),
            trace_delay_queue_enabled=delay_queue_enabled,
            trace_delay_queue_size=delay_queue_size,
            trace_delay_ticks=delay_ticks,
        )

    def for_kind(self, kind: str) -> "BOPConfig":
        direct = self.learner_configs.get(kind)
        if direct is not None:
            return direct
        for name, candidate in self.learner_configs.items():
            if name.lower().endswith(f"bop_{kind}") or name.lower().endswith(f"bop{kind}"):
                return candidate
        return self

    @property
    def uses_recorded_delay_actions(self) -> bool:
        """Use V5 event ordering only while queue parameters are unchanged."""
        return (
            self.schema_version >= LEARNER_REPLAY_CERTIFICATION_MIN_SCHEMA_VERSION
            and self.delay_queue_enabled
            and self.trace_delay_queue_enabled == self.delay_queue_enabled
            and self.trace_delay_queue_size == self.delay_queue_size
            and self.trace_delay_ticks == self.delay_ticks
            and bool(self.replay_delay_actions.get(self.bop_name))
        )


@dataclass
class _PCEntry:
    valid: bool = False
    tag: int = 0
    confidence: int = 0
    low_entry_miss_streak: int = 0
    recovered_probation: bool = False
    recovered_same_pc_credit_seen: bool = False
    recovered_cross_pc_credit_seen: bool = False
    epoch: int = 0


@dataclass
class _PCOffsetContext:
    valid: bool = False
    offset: int = 0
    confidence: int = 0
    low_entry_miss_streak: int = 0
    recovered_probation: bool = False
    recovered_same_pc_credit_seen: bool = False
    recovered_cross_pc_credit_seen: bool = False


@dataclass(frozen=True)
class _Lookup:
    index: int
    tag: int
    kind: str
    confidence: int
    state: str
    context_index: int = 0
    offset: int = 0
    context_hit: bool = False
    recovered_same_pc_credit_seen: bool = False
    recovered_cross_pc_credit_seen: bool = False


@dataclass(frozen=True)
class ControllerDecision:
    """One controller admission decision with its pre-update evidence.

    The normal replay consumes only the boolean result.  Offline attribution
    uses this snapshot to distinguish the current event's RR evidence from
    an earlier divergence in PC confidence or global-bypass state.
    """

    issued: bool
    reason: str
    validation_hit: bool
    validation_source: str
    owner_relation: str
    issuer_confidence: int | None
    issuer_state: str | None
    owner_confidence: int | None
    owner_state: str | None
    global_bypass_before: bool
    global_bypass_at_admission: bool
    issuer_recovered_same_pc_credit_seen: bool
    issuer_recovered_cross_pc_credit_seen: bool
    owner_recovered_same_pc_credit_seen: bool
    owner_recovered_cross_pc_credit_seen: bool


@dataclass
class _PendingUpdate:
    valid: bool = False
    offset_changed: bool = False
    validation_hit: bool = False
    hit_increment: int = 0
    kind: str = "generic"
    pc: int = 0
    trigger_line: int = 0
    index: int = 0
    tag: int = 0
    context_index: int = 0
    offset: int = 0
    participants: int = 0
    recovered_credit_relation: str = "none"


@dataclass(frozen=True)
class LearnerOutput:
    event: ReplayEvent
    kind: str
    best_offset_before: int
    best_offset_after: int
    best_score: int
    round: int
    best_offset_changed: bool
    issue_enabled: bool
    raw_candidate_valid: bool
    raw_candidate_addr: int
    validation_enabled: bool
    validation_hit: int
    validation_owner_pc: int = 0
    validation_owner_valid: bool = False
    selected_offset: int = 0


@dataclass(frozen=True)
class _RREntry:
    line: int = 0
    tag: int = 0
    owner_pc: int = 0
    owner_valid: bool = False


@dataclass
class _StudentOffset:
    offset: int
    confidence: float = 0.0
    last_phase_coverage: int = 0
    current_phase_coverage: int = 0


class BOPLearner:
    """Behavioral replay of the learner portion of gem5's BOP."""

    def __init__(
        self, config: BOPConfig, *, use_recorded_delay_actions: bool | None = None,
    ):
        if not config.offsets:
            raise ValueError(f"{config.bop_name}: offsets are empty")
        if config.rr_entries <= 0 or config.rr_entries & (config.rr_entries - 1):
            raise ValueError("RREntries must be a power of two")
        if config.delay_queue_enabled and config.clock_period_ticks <= 0:
            raise ValueError(
                "delay-queue learner replay requires ClockPeriodTicks from a V3 trace"
            )
        if config.auto_learning:
            raise ValueError(
                "auto-learning offsets require dynamic offset events, which are not traced"
            )
        self.config = config
        self.use_recorded_delay_actions = (
            config.uses_recorded_delay_actions
            if use_recorded_delay_actions is None
            else use_recorded_delay_actions
        )
        # The trace serializes originOffsets after C++ has expanded optional
        # negative offsets, so it is already the exact construction list.
        offsets = list(config.offsets)
        self.offsets = [[offset, 0, 1, 32] for offset in offsets if offset != 0]
        if not self.offsets:
            raise ValueError("offset list has no non-zero entries")
        self.max_offset_count = len(self.offsets)
        self.iterator = 0
        self.best_iterator = 0
        self.best_offset = self._calc(len(self.offsets) - 1)
        self.phase_best_offset = 0
        self.best_score = 0
        self.round = 0
        self.issue_enabled = False
        # gem5's RREntryDebug has no valid bit; value initialization leaves
        # every entry as {0, 0}, which intentionally aliases tag zero. BOP
        # checks both banks even though this XiangShan-aligned implementation
        # trains only the Left bank and notifyFill() leaves Right untouched.
        self.rr_left: list[_RREntry] = [_RREntry()] * config.rr_entries
        self.rr_right: list[_RREntry] = [_RREntry()] * config.rr_entries
        self.delay_queue: deque[tuple[_RREntry, int]] = deque()
        self.delay_event_tick: int | None = None
        self.student_pool: list[_StudentOffset] = []
        self.student_filter_bits: list[int] = []
        self.student_delay_queue: deque[tuple[int, tuple[int, ...], int]] = (
            deque()
        )
        self.student_selected_offset = 1
        self.student_selected_valid = False
        self.student_selected_enabled = False
        self.student_phase_train_count = 0
        if config.student_cover_enabled:
            if not 0 < config.student_pool_size <= 64:
                raise ValueError("student_pool_size must be in [1, 64]")
            if not _is_power_of_two(config.student_filter_entries):
                raise ValueError("student_filter_entries must be a power of two")
            if config.student_hash_count <= 0:
                raise ValueError("student_hash_count must be non-zero")
            if config.student_hash_mode not in {"lowbits", "bop_rr", "splitmix"}:
                raise ValueError("unsupported bounded student_hash_mode")
            if not 0.0 <= config.student_conf_alpha <= 1.0:
                raise ValueError("student_conf_alpha must be in [0, 1]")
            if not 0.0 <= config.student_cov_threshold <= 1.0:
                raise ValueError("student_cov_threshold must be in [0, 1]")
            if config.student_delay_queue_enabled and (
                config.student_delay_queue_size == 0
            ):
                raise ValueError("student_delay_queue_size must be non-zero")
            self.student_filter_bits = [0] * config.student_filter_entries

    @staticmethod
    def _u64(value: int) -> int:
        return value & UINT64_MASK

    def _line(self, addr: int) -> int:
        return self._u64(addr) & ~(self.config.block_size - 1)

    def _hash(self, addr: int) -> int:
        line = self._line(addr) // self.config.block_size
        bits = self.config.rr_entries.bit_length() - 1
        mask = self.config.rr_entries - 1
        return (line & mask) ^ ((line >> bits) & mask)

    def _tag(self, addr: int) -> int:
        line = self._line(addr) // self.config.block_size
        bits = self.config.rr_entries.bit_length() - 1
        return (line >> bits) & ((1 << self.config.tag_bits) - 1)

    @staticmethod
    def _splitmix64(value: int) -> int:
        value = (value + 0x9E3779B97F4A7C15) & UINT64_MASK
        value = ((value ^ (value >> 30)) * 0xBF58476D1CE4E5B9) & UINT64_MASK
        value = ((value ^ (value >> 27)) * 0x94D049BB133111EB) & UINT64_MASK
        return value ^ (value >> 31)

    def _student_hash_indexes(self, line_addr: int) -> tuple[int, ...]:
        if not self.config.student_cover_enabled:
            return ()
        mask = self.config.student_filter_entries - 1
        if self.config.student_hash_mode == "lowbits":
            base1 = line_addr
            base2 = ((line_addr >> 6) ^ (line_addr >> 12) ^ 0x9E37) | 1
        elif self.config.student_hash_mode == "bop_rr":
            bits = self.config.student_filter_entries.bit_length() - 1
            base1 = ((line_addr & mask) ^ ((line_addr >> bits) & mask)) & mask
            base2 = (((line_addr >> (2 * bits)) & mask) ^ line_addr ^ 0xC2B2) | 1
        else:
            base1 = self._splitmix64(line_addr)
            base2 = self._splitmix64(
                line_addr ^ 0x9E3779B97F4A7C15
            ) | 1
        return tuple(
            (base1 + index * base2) & mask
            for index in range(self.config.student_hash_count)
        )

    @staticmethod
    def _same_page(first: int, second: int) -> bool:
        return (first >> 12) == (second >> 12)

    def _student_all_same_sign(self) -> bool:
        if not self.student_pool:
            return False
        positive = self.student_pool[0].offset > 0
        return all(
            entry.offset > 0 if positive else entry.offset < 0
            for entry in self.student_pool
        )

    def _student_intermediate_offsets_match_slope(
        self, best_index: int, worst_index: int, best_coverage: int,
        worst_coverage: int,
    ) -> bool:
        if len(self.student_pool) <= 2:
            return True
        best_abs = abs(self.student_pool[best_index].offset)
        worst_abs = abs(self.student_pool[worst_index].offset)
        reference_distance = worst_abs - best_abs
        if reference_distance <= 0:
            return False
        reference_gap = best_coverage - worst_coverage
        coefficient = self.config.student_large_offset_priority_coeff
        for index, entry in enumerate(self.student_pool):
            if index in (best_index, worst_index):
                continue
            current_abs = abs(entry.offset)
            if current_abs <= best_abs or current_abs >= worst_abs:
                return False
            current_coverage = entry.current_phase_coverage
            if current_coverage > best_coverage or current_coverage < worst_coverage:
                return False
            current_distance = current_abs - best_abs
            current_gap = best_coverage - current_coverage
            if reference_gap == 0:
                if current_gap != 0:
                    return False
                continue
            lhs = current_gap * reference_distance
            rhs = reference_gap * current_distance
            if lhs < coefficient * rhs or coefficient * lhs > rhs:
                return False
        return True

    def _student_should_prefer_large_offset(
        self, best_index: int, worst_index: int, best_coverage: int,
        worst_coverage: int,
    ) -> bool:
        if (
            len(self.student_pool) < 2
            or self.student_phase_train_count == 0
            or not self._student_all_same_sign()
        ):
            return False
        by_abs = lambda entry: (abs(entry.offset), entry.offset)
        minimum = min(self.student_pool, key=by_abs)
        maximum = max(self.student_pool, key=by_abs)
        if (
            abs(self.student_pool[best_index].offset) != abs(minimum.offset)
            or abs(self.student_pool[worst_index].offset) != abs(maximum.offset)
        ):
            return False
        lhs = self.config.student_large_offset_priority_coeff * (
            worst_coverage - best_coverage
        )
        rhs = (abs(self.student_pool[worst_index].offset) -
               abs(self.student_pool[best_index].offset)) / self.student_phase_train_count
        return lhs <= rhs and self._student_intermediate_offsets_match_slope(
            best_index, worst_index, best_coverage, worst_coverage
        )

    def _student_insert_filter_mask(
        self, indexes: tuple[int, ...], mask: int,
    ) -> None:
        for index in indexes:
            self.student_filter_bits[index] |= mask

    def _student_drain_delay_queue(self, now: int) -> None:
        if not self.config.student_delay_queue_enabled:
            return
        while self.student_delay_queue and self.student_delay_queue[0][0] <= now:
            _, indexes, mask = self.student_delay_queue.popleft()
            self._student_insert_filter_mask(indexes, mask)

    def _student_enqueue_prediction(
        self, train_addr: int, bit_index: int, offset: int, tick: int,
    ) -> None:
        predicted = train_addr + offset * self.config.block_size
        if predicted < 0:
            return
        predicted &= UINT64_MASK
        if not self.config.cross_page and not self._same_page(train_addr, predicted):
            return
        indexes = self._student_hash_indexes(predicted // self.config.block_size)
        if not indexes:
            return
        mask = 1 << bit_index
        if not self.config.student_delay_queue_enabled:
            self._student_insert_filter_mask(indexes, mask)
            return
        if len(self.student_delay_queue) >= self.config.student_delay_queue_size:
            return
        self.student_delay_queue.append(
            (tick + self.config.student_delay_ticks, indexes, mask)
        )

    def _student_observe_train_addr(self, addr: int, tick: int) -> None:
        if not self.config.student_cover_enabled:
            return
        self.student_phase_train_count += 1
        if not self.student_pool:
            return
        self._student_drain_delay_queue(tick)
        hit_mask = UINT64_MASK
        for index in self._student_hash_indexes(addr // self.config.block_size):
            hit_mask &= self.student_filter_bits[index]
        for index, entry in enumerate(self.student_pool):
            if hit_mask & (1 << index):
                entry.current_phase_coverage += 1
        for index, entry in enumerate(self.student_pool):
            self._student_enqueue_prediction(addr, index, entry.offset, tick)

    def _student_pick_best_index(self) -> int:
        return max(
            range(len(self.student_pool)),
            key=lambda index: (
                self.student_pool[index].current_phase_coverage,
                self.student_pool[index].confidence,
                -abs(self.student_pool[index].offset),
                -self.student_pool[index].offset,
            ),
        )

    def _student_pick_worst_index(self) -> int:
        return min(
            range(len(self.student_pool)),
            key=lambda index: (
                self.student_pool[index].current_phase_coverage,
                self.student_pool[index].confidence,
                abs(self.student_pool[index].offset),
                self.student_pool[index].offset,
            ),
        )

    def _student_pick_evict_index(self) -> int:
        return min(
            range(len(self.student_pool)),
            key=lambda index: (
                self.student_pool[index].confidence,
                self.student_pool[index].last_phase_coverage,
                -abs(self.student_pool[index].offset),
                self.student_pool[index].offset,
            ),
        )

    def _student_insert_teacher_best(self, offset: int) -> None:
        if (
            not self.config.student_cover_enabled
            or offset == 0
            or self.config.student_teacher_top_n == 0
            or any(entry.offset == offset for entry in self.student_pool)
        ):
            return
        if len(self.student_pool) >= self.config.student_pool_size:
            del self.student_pool[self._student_pick_evict_index()]
        self.student_pool.append(_StudentOffset(offset))

    def _student_clear_phase_state(self) -> None:
        self.student_filter_bits[:] = [0] * len(self.student_filter_bits)
        self.student_delay_queue.clear()
        self.student_phase_train_count = 0
        for entry in self.student_pool:
            entry.current_phase_coverage = 0

    def _student_on_teacher_phase_end(self, teacher_best_offset: int) -> None:
        if not self.config.student_cover_enabled:
            return
        if self.student_pool:
            best_index = self._student_pick_best_index()
            worst_index = self._student_pick_worst_index()
            best_coverage = self.student_pool[best_index].current_phase_coverage
            worst_coverage = self.student_pool[worst_index].current_phase_coverage
            prefer_large = (
                self.config.student_large_offset_priority
                and self._student_should_prefer_large_offset(
                    best_index, worst_index, best_coverage, worst_coverage
                )
            )
            selected_index = worst_index if prefer_large else best_index
            selected_coverage = (
                worst_coverage if prefer_large else best_coverage
            )
            reward_index = selected_index
            punish_index = best_index if prefer_large else worst_index
            coverage = (
                selected_coverage / self.student_phase_train_count
                if self.student_phase_train_count else 0.0
            )
            self.student_selected_offset = self.student_pool[selected_index].offset
            self.student_selected_valid = True
            self.student_selected_enabled = (
                coverage >= self.config.student_cov_threshold
            )
            for index, entry in enumerate(self.student_pool):
                update = 1.0 if index == reward_index else (
                    -1.0 if index == punish_index else 0.0
                )
                entry.confidence = (
                    entry.confidence * self.config.student_conf_alpha
                    + update * (1.0 - self.config.student_conf_alpha)
                )
                entry.last_phase_coverage = entry.current_phase_coverage
        else:
            self.student_selected_valid = False
            self.student_selected_enabled = False
        self._student_insert_teacher_best(teacher_best_offset)
        self._student_clear_phase_state()

    def _insert_rr(
        self, addr: int, owner_pc: int = 0, owner_valid: bool = False,
    ) -> None:
        entry = _RREntry(
            self._line(addr), self._tag(addr), owner_pc, owner_valid,
        )
        self._insert_rr_entry(entry)

    def _insert_rr_entry(self, entry: _RREntry) -> None:
        self.rr_left[self._hash(entry.line)] = entry

    def _insert_rr_action(
        self, action: ReplayDelayAction, entry: _RREntry,
    ) -> None:
        if entry.line != self._line(action.addr):
            raise ValueError(
                f"{self.config.bop_name}: delay action address mismatch"
            )
        self._insert_rr_entry(entry)

    def _trigger_rr_entry(self, event: ReplayEvent) -> _RREntry:
        return _RREntry(
            self._line(event.trigger_addr), self._tag(event.trigger_addr),
            event.trigger_pc,
            event.trigger_has_pc and event.trigger_is_demand
            and event.trigger_is_read,
        )

    def _next_cycle(self, tick: int) -> int:
        period = self.config.clock_period_ticks
        return ((tick + period - 1) // period + 1) * period

    def _run_delay_events_before(self, tick: int) -> None:
        """Replay callbacks strictly before the next BOP trigger.

        A delay callback scheduled at the same tick as a cache-trigger event
        runs after the trigger: gem5 stores same-priority events in LIFO
        order, and the trigger event is scheduled later than the already
        pending delay callback.  Leaving equality pending also naturally
        preserves that state for a later trace event at the same tick.
        """
        while self.delay_event_tick is not None and self.delay_event_tick < tick:
            callback_tick = self.delay_event_tick
            self.delay_event_tick = None
            if self.delay_queue and self.delay_queue[0][1] <= callback_tick:
                entry, _ = self.delay_queue.popleft()
                self._insert_rr_entry(entry)
            if not self.delay_queue:
                continue
            if self.delay_queue[0][1] <= callback_tick:
                self.delay_event_tick = self._next_cycle(callback_tick)
            else:
                self.delay_event_tick = self.delay_queue[0][1]

    def _insert_training(self, entry: _RREntry | int, tick: int) -> None:
        if isinstance(entry, int):
            entry = _RREntry(self._line(entry), self._tag(entry))
        if not self.config.delay_queue_enabled:
            self._insert_rr_entry(entry)
            return
        if self.config.delay_queue_size == 0 or len(self.delay_queue) >= self.config.delay_queue_size:
            return
        process_tick = tick + self.config.delay_ticks
        self.delay_queue.append((entry, process_tick))
        if self.delay_event_tick is None:
            self.delay_event_tick = process_tick

    def apply_delay_action(self, action: ReplayDelayAction) -> None:
        """Apply a V5 native delay-queue transition before its trigger.

        The online trace is intentionally limited to BOP's intrinsic learner
        queue.  It records queue actions because the global gem5 event queue
        can interleave callbacks and triggers at the same tick in a way that
        trigger timestamps alone cannot reproduce.  Queue capacity and order
        are checked here; score learning still runs independently below.
        """
        if action.action != "dequeue_to_rr":
            raise ValueError(
                f"{self.config.bop_name}: unexpected replay action "
                f"{action.action!r} before learner trigger"
            )
        if not self.delay_queue:
            raise ValueError(
                f"{self.config.bop_name}: dequeue action on an empty delay queue"
            )
        entry, process_tick = self.delay_queue[0]
        if entry.line != self._line(action.addr) or process_tick != action.process_tick:
            raise ValueError(
                f"{self.config.bop_name}: delay action does not match queue head"
            )
        self.delay_queue.popleft()
        self._insert_rr_action(action, entry)
        if len(self.delay_queue) != action.queue_size_after:
            raise ValueError(
                f"{self.config.bop_name}: dequeue queue size mismatch"
            )

    def apply_trigger_delay_action(
        self, event: ReplayEvent, action: ReplayDelayAction | None,
    ) -> None:
        """Apply the V5 enqueue/drop decision that belongs to one trigger."""
        entry = self._trigger_rr_entry(event)
        if not self.config.delay_queue_enabled:
            self._insert_rr_entry(entry)
            return
        if action is None:
            raise ValueError(
                f"{self.config.bop_name}: missing delay action for replay order "
                f"{event.replay_order}"
            )
        if action.addr != entry.line:
            raise ValueError(
                f"{self.config.bop_name}: trigger delay action address mismatch"
            )
        if action.action == "enqueue":
            self.delay_queue.append((entry, action.process_tick))
        elif action.action == "drop_full":
            if len(self.delay_queue) != self.config.delay_queue_size:
                raise ValueError(
                    f"{self.config.bop_name}: drop_full without a full delay queue"
                )
        else:
            raise ValueError(
                f"{self.config.bop_name}: unexpected trigger replay action "
                f"{action.action!r}"
            )
        if len(self.delay_queue) != action.queue_size_after:
            raise ValueError(
                f"{self.config.bop_name}: trigger queue size mismatch"
            )

    def _test_rr_entry(self, addr: int) -> _RREntry | None:
        index = self._hash(addr)
        tag = self._tag(addr)
        if self.rr_left[index].tag == tag:
            return self.rr_left[index]
        if self.rr_right[index].tag == tag:
            return self.rr_right[index]
        return None

    def _test_rr(self, addr: int) -> bool:
        return self._test_rr_entry(addr) is not None

    def _calc(self, index: int) -> int:
        item = self.offsets[index]
        return item[0] * item[2]

    def _best_offset_index(self) -> int | None:
        """Match BOP::getBestOffsetIter's base-offset comparison exactly."""
        for index, item in enumerate(self.offsets):
            if item[0] == self.best_offset:
                return index
        return None

    def process(
        self, event: ReplayEvent, trigger_delay_action: ReplayDelayAction | None = None,
    ) -> LearnerOutput:
        if self.use_recorded_delay_actions:
            self.apply_trigger_delay_action(event, trigger_delay_action)
        else:
            self._run_delay_events_before(event.tick)
            self._insert_training(self._trigger_rr_entry(event), event.tick)
        addr = self._line(event.trigger_addr)
        before = self.best_offset
        item = self.offsets[self.iterator]
        offset = self._calc(self.iterator)
        lookup_addr = self._u64(addr - offset * self.config.block_size)
        if self._test_rr(lookup_addr):
            item[1] = (item[1] + 1) & 0xFF
            if self.config.adapt_offset and item[1] >= self.round // 2:
                if event.late:
                    item[3] = min(63, item[3] + 2)
                else:
                    item[3] = max(0, item[3] - 1)
                update_depth = False
                if item[3] > 42:
                    item[2] += 1
                    item[3] = 32
                    update_depth = True
                elif item[3] < 4:
                    item[2] = max(1, item[2] - 1)
                    item[3] = 32
                    update_depth = True
                if update_depth and self._best_offset_index() == self.iterator:
                    self.best_offset = self._calc(self.iterator)
            if item[1] > self.best_score:
                self.best_iterator = self.iterator
                self.best_score = item[1]
                self.phase_best_offset = self._calc(self.iterator)
        self.iterator += 1
        teacher_phase_end = False
        if self.iterator == len(self.offsets):
            self.iterator = 0
            self.round += 1
            if self.best_score >= self.config.score_max or self.round == self.config.round_max:
                self.issue_enabled = self.best_score > self.config.bad_score
                self.best_offset = self.phase_best_offset
                self.round = 0
                self.best_score = 0
                self.phase_best_offset = 0
                for offset_item in self.offsets:
                    offset_item[1] = 0
                teacher_phase_end = True
        after = self.best_offset
        self._student_observe_train_addr(addr, event.tick)
        if teacher_phase_end:
            self._student_on_teacher_phase_end(after)
        teacher_issue = self.issue_enabled
        student_issue = (
            self.config.student_cover_enabled
            and self.student_selected_valid
            and self.student_selected_enabled
        )
        selected_offset = self.student_selected_offset if student_issue else after
        algorithm_issue = (teacher_issue or student_issue) and selected_offset != 0
        raw_valid = algorithm_issue
        raw_addr = (
            self._line(addr + selected_offset * self.config.block_size)
            if raw_valid else 0
        )
        validation_enabled = (
            self.config.issue_validation or self.config.pc_validation_confidence
        )
        validation_addr = (
            self._u64(addr - selected_offset * self.config.block_size)
            if selected_offset != 0 else 0
        )
        validation_entry = (
            self._test_rr_entry(validation_addr)
            if raw_valid and validation_enabled
            else None
        )
        validation_hit = int(validation_entry is not None) \
            if raw_valid and validation_enabled else -1
        return LearnerOutput(
            event, event.bop_kind, before, after, self.best_score, self.round,
            before != after, algorithm_issue, raw_valid, raw_addr,
            validation_enabled, validation_hit,
            validation_entry.owner_pc if validation_entry else 0,
            validation_entry.owner_valid if validation_entry else False,
            selected_offset,
        )


def replay_learner(
    events: Sequence[ReplayEvent], config: BOPConfig,
) -> list[LearnerOutput]:
    learners: dict[str, BOPLearner] = {}
    action_streams: dict[str, tuple[ReplayDelayAction, ...]] = {}
    action_positions: dict[str, int] = defaultdict(int)
    outputs: list[LearnerOutput] = []
    for event in sorted(events, key=lambda item: (item.access_seq, item.order)):
        learner = learners.get(event.bop_kind)
        if learner is None:
            learner = BOPLearner(config.for_kind(event.bop_kind))
            learners[event.bop_kind] = learner
        learner_config = learner.config
        if learner_config.uses_recorded_delay_actions:
            actions = action_streams.get(event.bop_name)
            if actions is None:
                actions = tuple(sorted(
                    learner_config.replay_delay_actions[event.bop_name],
                    key=lambda action: action.replay_order,
                ))
                action_streams[event.bop_name] = actions
            position = action_positions[event.bop_name]
            while (position < len(actions) and
                   actions[position].replay_order < event.replay_order):
                action = actions[position]
                if action.action != "dequeue_to_rr":
                    raise ValueError(
                        f"{event.bop_name}: unexpected {action.action!r} "
                        "before learner trigger"
                    )
                learner.apply_delay_action(action)
                position += 1
            trigger_action = None
            if position < len(actions) and (
                actions[position].replay_order == event.replay_order
            ):
                trigger_action = actions[position]
                position += 1
            outputs.append(learner.process(event, trigger_action))
            action_positions[event.bop_name] = position
        else:
            outputs.append(learner.process(event))
    return outputs


def compare_online_learner(
    events: Sequence[ReplayEvent], outputs: Sequence[LearnerOutput]
) -> dict[str, object]:
    fields = (
        "best_offset_before", "best_offset_after", "best_score", "round",
        "best_offset_changed", "selected_offset", "issue_enabled",
        "raw_candidate_valid", "raw_candidate_addr",
    )
    ordered_events = sorted(events, key=lambda item: (item.access_seq, item.order))
    mismatches = []
    for event, output in zip(ordered_events, outputs):
        if (
            event.access_seq != output.event.access_seq
            or event.bop_name != output.event.bop_name
        ):
            mismatches.append({
                "access_seq": event.access_seq,
                "bop_name": event.bop_name,
                "field": "event_identity",
                "online": [event.access_seq, event.bop_name],
                "offline": [output.event.access_seq, output.event.bop_name],
            })
            continue
        expected = {
            "best_offset_before": event.best_offset_before,
            "best_offset_after": event.best_offset_after,
            "best_score": event.best_score,
            "round": event.round,
            "best_offset_changed": event.best_offset_changed,
            "selected_offset": event.selected_offset or event.best_offset_after,
            "issue_enabled": event.issue_enabled,
            "raw_candidate_valid": event.raw_candidate_valid,
            "raw_candidate_addr": event.raw_candidate_addr,
        }
        actual = {field: getattr(output, field) for field in fields}
        for field in fields:
            if expected[field] != actual[field]:
                mismatches.append({
                    "access_seq": event.access_seq,
                    "bop_name": event.bop_name,
                    "field": field,
                    "online": expected[field],
                    "offline": actual[field],
                })
                break
    for event in ordered_events[len(outputs):]:
        mismatches.append({
            "access_seq": event.access_seq,
            "bop_name": event.bop_name,
            "field": "missing_offline_output",
            "online": True,
            "offline": False,
        })
    for output in outputs[len(ordered_events):]:
        mismatches.append({
            "access_seq": output.event.access_seq,
            "bop_name": output.event.bop_name,
            "field": "unexpected_offline_output",
            "online": False,
            "offline": True,
        })
    return {
        "total_events": len(ordered_events),
        "matched_events": len(ordered_events) - len(mismatches),
        "mismatched_events": len(mismatches),
        "first_mismatch": mismatches[0] if mismatches else None,
        "pass": not mismatches,
    }


def _kind_index(kind: str) -> int:
    try:
        return {"generic": 0, "large": 1, "small": 2}[kind]
    except KeyError as error:
        raise ValueError(f"unsupported BOP kind: {kind!r}") from error


def _is_power_of_two(value: int) -> bool:
    return value > 0 and (value & (value - 1)) == 0


def _trunc_div(numerator: int, denominator: int) -> int:
    return numerator // denominator if numerator >= 0 else -((-numerator) // denominator)


class PCValidationController:
    """Exact Python model of BOP's PC-confidence/global-bypass controller.

    Best-offset learning and RR validation are fixed trace inputs in this
    phase.  The table and global feedback state are replayed so controller
    parameter changes can be ranked without cache/filter behavior.
    """

    def __init__(self, config: BOPConfig):
        if not _is_power_of_two(config.pc_validation_entries):
            raise ValueError("PCValidationEntries must be a power of two")
        if config.pc_validation_entries < PC_ASSOCIATIVITY:
            raise ValueError("PCValidationEntries must be at least four")
        if not _is_power_of_two(config.pc_validation_medium_sample_period):
            raise ValueError("PCValidationMediumSamplePeriod must be a power of two")
        if not _is_power_of_two(config.pc_validation_miss_decay_period):
            raise ValueError("PCValidationMissDecayPeriod must be a power of two")
        if config.pc_validation_offset_context_slots not in (0, 1, 2, 4):
            raise ValueError(
                "PCValidationOffsetContextSlots must be zero, one, two, or four"
            )

        self.config = config
        self.sets = config.pc_validation_entries // PC_ASSOCIATIVITY
        self.set_bits = self.sets.bit_length() - 1
        self.tag_mask = (1 << config.pc_validation_tag_bits) - 1
        self.counter_max = (1 << config.pc_validation_counter_bits) - 1
        self.epoch_mask = (1 << config.pc_validation_epoch_bits) - 1
        self.uses_offset_contexts = config.pc_validation_offset_context_slots != 0
        self.offset_context_slots = config.pc_validation_offset_context_slots
        self.current_epoch = {"generic": 0, "large": 0, "small": 0}
        self.table = [_PCEntry() for _ in range(config.pc_validation_entries)]
        self.plru_state = [0] * self.sets
        self.offset_contexts = [
            [_PCOffsetContext() for _ in range(self.offset_context_slots)]
            for _ in range(config.pc_validation_entries)
        ]
        self.offset_plru_state = [0] * config.pc_validation_entries
        self.pending = {
            kind: [_PendingUpdate(), _PendingUpdate()]
            for kind in ("generic", "large", "small")
        }
        self.global_resolved = 0
        self.global_unused = 0
        self.global_issued = 0
        self.global_unused_ewma = GLOBAL_UNUSED_EWMA_INITIAL
        self.global_checks_since_outcome = 0
        self.global_bypass = False
        self.table_lookups = 0
        self.table_hits = 0
        self.table_misses = 0
        self.table_replacements = 0
        self.offset_context_hits = 0
        self.offset_context_misses = 0
        self.offset_context_replacements = 0
        self.epoch_resets = 0
        self.offset_epoch_changes = 0
        self.rr_owner_valid_hits = 0
        self.rr_owner_invalid_hits = 0
        self.rr_owner_same_pc_hits = 0
        self.rr_owner_cross_pc_hits = 0
        self.producer_hit_updates = 0
        self.consumer_miss_updates = 0
        self.same_pc_hit_gate_checks = 0
        self.same_pc_hit_gate_issued = 0
        self.same_pc_hit_gate_suppressed = 0
        self.recovered_compatible_hits = 0
        self.recovered_producer_credits = 0
        self.recovered_producer_credit_suppressed = 0
        self.recovered_probation_armed = 0
        self.recovered_probation_confirmed = 0
        self.last_decision: ControllerDecision | None = None

    def _signature(self, pc: int, kind: str) -> int:
        signature = (pc & UINT64_MASK) >> 1
        signature ^= signature >> 7
        signature ^= signature >> 13
        signature ^= signature >> 27
        signature ^= _kind_index(kind) * 0x9E3779B97F4A7C15
        signature &= UINT64_MASK
        signature ^= signature >> 11
        signature ^= signature >> 23
        return signature & UINT64_MASK

    def _pc_key(self, pc: int, kind: str) -> tuple[int, int]:
        signature = self._signature(pc, kind)
        return (
            signature & (self.sets - 1),
            (signature >> self.set_bits) & self.tag_mask,
        )

    def _sample(
        self, pc: int, kind: str, line: int, period: int, salt: int,
        best_offset: int = 0,
    ) -> bool:
        signature = self._signature(pc, kind)
        signature ^= line & UINT64_MASK
        signature ^= salt
        if self.uses_offset_contexts:
            signature ^= (best_offset & UINT64_MASK) * 0x9E3779B97F4A7C15
        else:
            signature ^= self.current_epoch[kind]
        signature ^= signature >> 9
        signature ^= signature >> 17
        signature ^= signature >> 29
        return (signature & (period - 1)) == 0

    def _entry_index(self, set_index: int, way: int) -> int:
        return set_index * PC_ASSOCIATIVITY + way

    def _plru_victim(self, set_index: int) -> int:
        state = self.plru_state[set_index] & 0x7
        if (state & 0x1) == 0:
            return 0 if (state & 0x2) == 0 else 1
        return 2 if (state & 0x4) == 0 else 3

    def _touch_plru(self, set_index: int, way: int) -> None:
        state = self.plru_state[set_index]
        if way == 0:
            state |= 0x1
            state |= 0x2
        elif way == 1:
            state |= 0x1
            state &= ~0x2
        elif way == 2:
            state &= ~0x1
            state |= 0x4
        elif way == 3:
            state &= ~0x1
            state &= ~0x4
        else:
            raise ValueError(f"invalid PLRU way: {way}")
        self.plru_state[set_index] = state

    def _offset_context_victim(self, entry_index: int) -> int:
        if self.offset_context_slots == 1:
            return 0
        state = self.offset_plru_state[entry_index]
        if self.offset_context_slots == 2:
            return state & 0x1
        if (state & 0x1) == 0:
            return 0 if (state & 0x2) == 0 else 1
        return 2 if (state & 0x4) == 0 else 3

    def _touch_offset_context(self, entry_index: int, context_index: int) -> None:
        if self.offset_context_slots == 1:
            return
        if self.offset_context_slots == 2:
            self.offset_plru_state[entry_index] = 1 if context_index == 0 else 0
            return
        state = self.offset_plru_state[entry_index]
        if context_index == 0:
            state |= 0x1
            state |= 0x2
        elif context_index == 1:
            state |= 0x1
            state &= ~0x2
        elif context_index == 2:
            state &= ~0x1
            state |= 0x4
        elif context_index == 3:
            state &= ~0x1
            state &= ~0x4
        else:
            raise ValueError(f"invalid offset context way: {context_index}")
        self.offset_plru_state[entry_index] = state

    def lookup(self, pc: int, kind: str, best_offset: int = 0) -> _Lookup:
        self.table_lookups += 1
        set_index, tag = self._pc_key(pc, kind)
        way = None
        root_hit = False
        for candidate_way in range(PC_ASSOCIATIVITY):
            entry = self.table[self._entry_index(set_index, candidate_way)]
            if entry.valid and entry.tag == tag:
                way = candidate_way
                root_hit = True
                break
        if way is None:
            for candidate_way in range(PC_ASSOCIATIVITY):
                entry = self.table[self._entry_index(set_index, candidate_way)]
                if not entry.valid:
                    way = candidate_way
                    break
        if way is None:
            way = self._plru_victim(set_index)

        index = self._entry_index(set_index, way)
        entry = self.table[index]
        if root_hit:
            self.table_hits += 1
        else:
            self.table_misses += 1
            if entry.valid:
                self.table_replacements += 1
        if not root_hit:
            entry.valid = True
            entry.tag = tag
            entry.confidence = self.config.pc_validation_initial
            entry.low_entry_miss_streak = 0
            entry.recovered_probation = False
            entry.recovered_same_pc_credit_seen = False
            entry.recovered_cross_pc_credit_seen = False
            entry.epoch = self.current_epoch[kind]
            if self.uses_offset_contexts:
                self.offset_contexts[index] = [
                    _PCOffsetContext() for _ in range(self.offset_context_slots)
                ]
                self.offset_plru_state[index] = 0
        elif not self.uses_offset_contexts and entry.epoch != self.current_epoch[kind]:
            entry.confidence = self.config.pc_validation_initial
            entry.low_entry_miss_streak = 0
            entry.recovered_probation = False
            entry.recovered_same_pc_credit_seen = False
            entry.recovered_cross_pc_credit_seen = False
            entry.epoch = self.current_epoch[kind]
            self.epoch_resets += 1
        self._touch_plru(set_index, way)

        if not self.uses_offset_contexts:
            confidence = entry.confidence
            low_streak = entry.low_entry_miss_streak
            context_index = 0
            context_hit = False
            recovered_same_pc_credit_seen = entry.recovered_same_pc_credit_seen
            recovered_cross_pc_credit_seen = entry.recovered_cross_pc_credit_seen
        else:
            contexts = self.offset_contexts[index]
            context_index = next(
                (
                    candidate
                    for candidate, context in enumerate(contexts)
                    if context.valid and context.offset == best_offset
                ),
                None,
            )
            context_hit = context_index is not None
            if context_hit:
                self.offset_context_hits += 1
            else:
                self.offset_context_misses += 1
            if context_index is None:
                context_index = next(
                    (
                        candidate
                        for candidate, context in enumerate(contexts)
                        if not context.valid
                    ),
                    None,
                )
            if context_index is None:
                context_index = self._offset_context_victim(index)
            context = contexts[context_index]
            if not context_hit:
                if context.valid:
                    self.offset_context_replacements += 1
                context.valid = True
                context.offset = best_offset
                context.confidence = self.config.pc_validation_initial
                context.low_entry_miss_streak = 0
                context.recovered_probation = False
                context.recovered_same_pc_credit_seen = False
                context.recovered_cross_pc_credit_seen = False
            self._touch_offset_context(index, context_index)
            confidence = context.confidence
            low_streak = context.low_entry_miss_streak
            recovered_same_pc_credit_seen = context.recovered_same_pc_credit_seen
            recovered_cross_pc_credit_seen = context.recovered_cross_pc_credit_seen

        if confidence >= self.config.pc_validation_high_threshold:
            state = "high"
        elif confidence >= self.config.pc_validation_medium_threshold:
            state = "medium"
        else:
            state = "low"
        return _Lookup(
            index, tag, kind, confidence, state, context_index, best_offset,
            context_hit, recovered_same_pc_credit_seen,
            recovered_cross_pc_credit_seen,
        )

    def note_offset_change(self, kind: str) -> None:
        update = self.pending[kind][0]
        update.kind = kind
        update.offset_changed = True
        self.offset_epoch_changes += 1

    def stats(self) -> dict[str, int]:
        return {
            "offset_context_slots": self.offset_context_slots,
            "table_lookups": self.table_lookups,
            "table_hits": self.table_hits,
            "table_misses": self.table_misses,
            "table_replacements": self.table_replacements,
            "offset_context_hits": self.offset_context_hits,
            "offset_context_misses": self.offset_context_misses,
            "offset_context_replacements": self.offset_context_replacements,
            "epoch_resets": self.epoch_resets,
            "offset_epoch_changes": self.offset_epoch_changes,
            "rr_owner_valid_hits": self.rr_owner_valid_hits,
            "rr_owner_invalid_hits": self.rr_owner_invalid_hits,
            "rr_owner_same_pc_hits": self.rr_owner_same_pc_hits,
            "rr_owner_cross_pc_hits": self.rr_owner_cross_pc_hits,
            "producer_hit_updates": self.producer_hit_updates,
            "consumer_miss_updates": self.consumer_miss_updates,
            "same_pc_hit_gate_checks": self.same_pc_hit_gate_checks,
            "same_pc_hit_gate_issued": self.same_pc_hit_gate_issued,
            "same_pc_hit_gate_suppressed": self.same_pc_hit_gate_suppressed,
            "recovered_compatible_hits": self.recovered_compatible_hits,
            "recovered_producer_credits": self.recovered_producer_credits,
            "recovered_producer_credit_suppressed": (
                self.recovered_producer_credit_suppressed
            ),
            "recovered_probation_armed": self.recovered_probation_armed,
            "recovered_probation_confirmed": self.recovered_probation_confirmed,
        }

    def _state_for_lookup(self, lookup: _Lookup) -> _PCEntry | _PCOffsetContext:
        if self.uses_offset_contexts:
            return self.offset_contexts[lookup.index][lookup.context_index]
        return self.table[lookup.index]

    def _clear_recovered_probation(self, lookup: _Lookup) -> None:
        self._state_for_lookup(lookup).recovered_probation = False

    def _accept_recovered_credit(self, lookup: _Lookup) -> bool:
        """Require two compatible recovered hits when probation is enabled."""
        if not self.config.pc_validation_recovered_probation:
            return True
        state = self._state_for_lookup(lookup)
        if state.recovered_probation:
            state.recovered_probation = False
            self.recovered_probation_confirmed += 1
            return True
        state.recovered_probation = True
        self.recovered_probation_armed += 1
        return False

    def _recovered_hit_increment(self) -> int:
        return (
            self.config.pc_validation_recovered_hit_increment
            or self.config.pc_validation_hit_increment
        )

    def _recovered_producer_credit_allowed(self, relation: str) -> bool:
        """Return whether this recovered owner may update a producer context."""
        if not self.config.pc_validation_recovered_producer_credit:
            return False
        if relation == "same_pc":
            return self.config.pc_validation_recovered_same_pc_credit
        if relation == "cross_pc":
            return self.config.pc_validation_recovered_cross_pc_credit
        return (
            self.config.pc_validation_recovered_same_pc_credit
            or self.config.pc_validation_recovered_cross_pc_credit
        )

    def submit_validation(
        self, lookup: _Lookup, pc: int, trigger_line: int, validation_hit: bool,
        hit_increment: int = 0, recovered_credit_relation: str = "none",
    ) -> None:
        updates = self.pending[lookup.kind]
        free_update = None
        for update in updates:
            if not update.valid:
                if free_update is None:
                    free_update = update
                continue
            if (
                update.index == lookup.index
                and update.tag == lookup.tag
                and update.context_index == lookup.context_index
                and update.offset == lookup.offset
                and update.kind == lookup.kind
            ):
                if validation_hit:
                    # A same AccessSeq can reach one context through more
                    # than one fixed producer/consumer path. A miss carries
                    # no positive increment, so a later recovered producer
                    # credit must retain its requested strength rather than
                    # inheriting the normal native-hit increment.
                    update.hit_increment = max(
                        update.hit_increment if update.validation_hit else 0,
                        hit_increment or self.config.pc_validation_hit_increment,
                    )
                    if recovered_credit_relation != "none":
                        update.recovered_credit_relation = recovered_credit_relation
                update.validation_hit = update.validation_hit or validation_hit
                update.participants += 1
                return
        if free_update is None:
            raise ValueError("PC validation exceeded per-demand update capacity")
        free_update.valid = True
        free_update.kind = lookup.kind
        free_update.pc = pc
        free_update.trigger_line = trigger_line
        free_update.index = lookup.index
        free_update.tag = lookup.tag
        free_update.context_index = lookup.context_index
        free_update.offset = lookup.offset
        free_update.validation_hit = validation_hit
        free_update.hit_increment = hit_increment
        free_update.recovered_credit_relation = recovered_credit_relation
        free_update.participants = 1

    def _commit_one(self, kind: str, update_index: int) -> None:
        update = self.pending[kind][update_index]
        if not update.valid and not update.offset_changed:
            return
        if update.valid and not update.offset_changed:
            entry = (
                self.offset_contexts[update.index][update.context_index]
                if self.uses_offset_contexts
                else self.table[update.index]
            )
            if update.validation_hit:
                hit_increment = (
                    update.hit_increment
                    or self.config.pc_validation_hit_increment
                )
                entry.confidence = min(
                    self.counter_max,
                    entry.confidence + hit_increment,
                )
                entry.low_entry_miss_streak = 0
                if update.recovered_credit_relation == "same_pc":
                    entry.recovered_same_pc_credit_seen = True
                elif update.recovered_credit_relation == "cross_pc":
                    entry.recovered_cross_pc_credit_seen = True
            elif self._sample(
                update.pc,
                kind,
                update.trigger_line,
                self.config.pc_validation_miss_decay_period,
                0x7F4A,
                update.offset,
            ):
                if (
                    self.config.pc_validation_low_entry_miss_streak_threshold != 0
                    and entry.confidence
                    == self.config.pc_validation_medium_threshold
                ):
                    entry.low_entry_miss_streak = min(
                        self.config.pc_validation_low_entry_miss_streak_threshold,
                        entry.low_entry_miss_streak + 1,
                    )
                    if (
                        entry.low_entry_miss_streak
                        == self.config.pc_validation_low_entry_miss_streak_threshold
                    ):
                        entry.confidence = max(0, entry.confidence - 1)
                        entry.low_entry_miss_streak = 0
                else:
                    entry.confidence = max(0, entry.confidence - 1)
                    entry.low_entry_miss_streak = 0
        if not self.uses_offset_contexts and update.offset_changed:
            self.current_epoch[kind] = (self.current_epoch[kind] + 1) & self.epoch_mask
        self.pending[kind][update_index] = _PendingUpdate()

    def commit(self) -> None:
        for kind in ("generic", "large", "small"):
            for index in range(len(self.pending[kind])):
                self._commit_one(kind, index)

    def note_pc_validation_miss(self) -> None:
        if not self.config.global_coverage_guard or not self.global_bypass:
            return
        self.global_checks_since_outcome += 1
        if self.global_checks_since_outcome >= GLOBAL_IDLE_RESET_CHECKS:
            self._reset_global_policy()

    def _reset_global_policy(self) -> None:
        self.global_resolved = 0
        self.global_unused = 0
        self.global_issued = 0
        self.global_unused_ewma = GLOBAL_UNUSED_EWMA_INITIAL
        self.global_checks_since_outcome = 0
        self.global_bypass = False

    def note_issued(self) -> None:
        if self.config.global_coverage_guard:
            self.global_issued += 1

    def note_outcome(self, useful: bool) -> None:
        if not self.config.global_coverage_guard:
            return
        self.global_checks_since_outcome = 0
        self.global_resolved += 1
        if not useful:
            self.global_unused += 1
        if self.global_resolved != GLOBAL_OUTCOME_WINDOW_SIZE:
            return

        coverage_good = (
            self.config.global_bop_min_resolved_coverage_shift == 0
            or self.global_issued == 0
            or (
                GLOBAL_OUTCOME_WINDOW_SIZE
                << self.config.global_bop_min_resolved_coverage_shift
            )
            >= self.global_issued
        )
        unused_q08 = (self.global_unused * 255) >> 9
        delta = unused_q08 - self.global_unused_ewma
        step = _trunc_div(delta, 1 << GLOBAL_EWMA_SHIFT)
        if step == 0 and delta != 0:
            step = 1 if delta > 0 else -1
        self.global_unused_ewma = min(255, max(0, self.global_unused_ewma + step))
        self.global_resolved = 0
        self.global_unused = 0
        self.global_issued = 0
        self.global_bypass = (
            self.global_unused_ewma <= self.config.global_bop_unused_threshold
            and coverage_good
        )

    def _admit_by_confidence(
        self, lookup: _Lookup, pc: int, kind: str, trigger_line: int,
        best_offset: int, *, note_pc_validation_miss: bool,
    ) -> tuple[bool, str]:
        if note_pc_validation_miss:
            self.note_pc_validation_miss()
        if self.global_bypass:
            return True, "global_bypass"
        if lookup.state == "high":
            return True, "high_confidence"
        if lookup.state == "medium":
            issued = self._sample(
                pc, kind, trigger_line,
                self.config.pc_validation_medium_sample_period, 0x9E37,
                best_offset,
            )
            return issued, "medium_sampled" if issued else "medium_unsampled"
        return False, "low_confidence"

    def _consumer_admission(
        self, lookup: _Lookup, pc: int, kind: str, trigger_line: int,
        best_offset: int,
    ) -> tuple[bool, str]:
        return self._admit_by_confidence(
            lookup, pc, kind, trigger_line, best_offset,
            note_pc_validation_miss=True,
        )

    def _record_decision(
        self, issued: bool, reason: str, validation_hit: bool,
        validation_source: str, owner_relation: str,
        global_bypass_before: bool, issuer_lookup: _Lookup | None = None,
        owner_lookup: _Lookup | None = None,
    ) -> bool:
        self.last_decision = ControllerDecision(
            issued=issued,
            reason=reason,
            validation_hit=validation_hit,
            validation_source=validation_source,
            owner_relation=owner_relation,
            issuer_confidence=(
                issuer_lookup.confidence if issuer_lookup is not None else None
            ),
            issuer_state=(issuer_lookup.state if issuer_lookup is not None else None),
            owner_confidence=(
                owner_lookup.confidence if owner_lookup is not None else None
            ),
            owner_state=(owner_lookup.state if owner_lookup is not None else None),
            global_bypass_before=global_bypass_before,
            global_bypass_at_admission=self.global_bypass,
            issuer_recovered_same_pc_credit_seen=(
                issuer_lookup.recovered_same_pc_credit_seen
                if issuer_lookup is not None else False
            ),
            issuer_recovered_cross_pc_credit_seen=(
                issuer_lookup.recovered_cross_pc_credit_seen
                if issuer_lookup is not None else False
            ),
            owner_recovered_same_pc_credit_seen=(
                owner_lookup.recovered_same_pc_credit_seen
                if owner_lookup is not None else False
            ),
            owner_recovered_cross_pc_credit_seen=(
                owner_lookup.recovered_cross_pc_credit_seen
                if owner_lookup is not None else False
            ),
        )
        return issued

    def policy_candidate_values(
        self, *, bop_kind: str, best_offset: int, best_offset_changed: bool,
        raw_candidate_valid: bool, pc_confidence_enabled: bool,
        validation_enabled: bool, validation_hit: int, trigger_addr: int,
        trigger_pc: int, trigger_has_pc: bool,
        validation_owner_pc: int = 0, validation_owner_valid: bool = False,
        validation_source: str = "native",
    ) -> bool:
        """Apply the controller to a recorded or learner-derived input."""
        if validation_source not in ("native", "recovered"):
            raise ValueError(f"unsupported validation source: {validation_source!r}")
        global_bypass_before = self.global_bypass
        if (best_offset_changed and pc_confidence_enabled
                and not self.uses_offset_contexts):
            self.note_offset_change(bop_kind)
        if not raw_candidate_valid:
            return self._record_decision(
                False, "raw_candidate_invalid", validation_hit == 1,
                validation_source, "none", global_bypass_before,
            )
        if not pc_confidence_enabled:
            issued = not validation_enabled or validation_hit == 1
            return self._record_decision(
                issued,
                "pc_confidence_disabled_validation_hit" if issued
                else "pc_confidence_disabled_validation_miss",
                validation_hit == 1, validation_source, "none",
                global_bypass_before,
            )

        validation_hit_bool = validation_hit == 1
        recovered_hit = validation_hit_bool and validation_source == "recovered"
        recovered_admission = (
            recovered_hit and self.config.pc_validation_recovered_admission
        )
        admission_hit = validation_hit_bool and (
            not recovered_hit or recovered_admission
        )
        trigger_line = trigger_addr // self.config.block_size

        def submit_producer_credit(
            lookup: _Lookup, pc: int, relation: str = "unknown",
        ) -> None:
            if not recovered_hit:
                self._clear_recovered_probation(lookup)
                self.submit_validation(lookup, pc, trigger_line, True)
                self.producer_hit_updates += 1
                return
            if not self._recovered_producer_credit_allowed(relation):
                self.recovered_producer_credit_suppressed += 1
                return
            if not self._accept_recovered_credit(lookup):
                return
            self.submit_validation(
                lookup, pc, trigger_line, True,
                self._recovered_hit_increment(),
                recovered_credit_relation=relation,
            )
            self.producer_hit_updates += 1
            self.recovered_producer_credits += 1

        if (self.config.pc_validation_producer_consumer
                and validation_hit_bool):
            if recovered_hit:
                self.recovered_compatible_hits += 1
            if not validation_owner_valid:
                self.rr_owner_invalid_hits += 1
                if recovered_hit and not recovered_admission:
                    if trigger_has_pc:
                        current_lookup = self.lookup(
                            trigger_pc, bop_kind, best_offset
                        )
                        issue, admission_reason = self._consumer_admission(
                            current_lookup, trigger_pc, bop_kind, trigger_line,
                            best_offset,
                        )
                        self.submit_validation(
                            current_lookup, trigger_pc, trigger_line, False
                        )
                        self.consumer_miss_updates += 1
                        return self._record_decision(
                            issue, "invalid_owner_" + admission_reason, True,
                            validation_source, "invalid_owner",
                            global_bypass_before, issuer_lookup=current_lookup,
                        )
                    self.note_pc_validation_miss()
                    issue = self.global_bypass
                    return self._record_decision(
                        issue,
                        "invalid_owner_no_pc_global_bypass" if issue
                        else "invalid_owner_no_pc_suppressed",
                        True, validation_source, "invalid_owner",
                        global_bypass_before,
                    )
                return self._record_decision(
                    True, "validation_hit_invalid_owner", True,
                    validation_source, "invalid_owner",
                    global_bypass_before,
                )

            self.rr_owner_valid_hits += 1
            if not trigger_has_pc:
                owner_lookup = self.lookup(
                    validation_owner_pc, bop_kind, best_offset
                )
                submit_producer_credit(owner_lookup, 0, "unknown")
                if recovered_hit and not recovered_admission:
                    self.note_pc_validation_miss()
                    issue = self.global_bypass
                    return self._record_decision(
                        issue,
                        "no_trigger_pc_global_bypass" if issue
                        else "no_trigger_pc_suppressed",
                        True, validation_source, "no_trigger_pc",
                        global_bypass_before, owner_lookup=owner_lookup,
                    )
                return self._record_decision(
                    True, "validation_hit_no_trigger_pc", True,
                    validation_source, "no_trigger_pc",
                    global_bypass_before, owner_lookup=owner_lookup,
                )

            if (self._pc_key(validation_owner_pc, bop_kind)
                    == self._pc_key(trigger_pc, bop_kind)):
                self.rr_owner_same_pc_hits += 1
                current_lookup = self.lookup(
                    trigger_pc, bop_kind, best_offset
                )
                issue = True
                admission_reason = "validation_hit"
                if not admission_hit:
                    issue, admission_reason = self._consumer_admission(
                        current_lookup, trigger_pc, bop_kind, trigger_line,
                        best_offset,
                    )
                    self.submit_validation(
                        current_lookup, trigger_pc, trigger_line, False
                    )
                    self.consumer_miss_updates += 1
                elif (
                    self.config.pc_validation_same_pc_hit_gate
                    or (
                        recovered_hit
                        and self.config.pc_validation_recovered_same_pc_hit_gate
                    )
                ):
                    self.same_pc_hit_gate_checks += 1
                    issue, admission_reason = self._admit_by_confidence(
                        current_lookup, trigger_pc, bop_kind, trigger_line,
                        best_offset, note_pc_validation_miss=False,
                    )
                    if issue:
                        self.same_pc_hit_gate_issued += 1
                    else:
                        self.same_pc_hit_gate_suppressed += 1
                if recovered_hit:
                    submit_producer_credit(current_lookup, trigger_pc, "same_pc")
                else:
                    self._clear_recovered_probation(current_lookup)
                    self.submit_validation(
                        current_lookup, trigger_pc, trigger_line, True,
                        self.config.pc_validation_same_pc_hit_increment,
                    )
                    self.producer_hit_updates += 1
                relation = (
                    "same_pc" if validation_owner_pc == trigger_pc
                    else "pc_key_alias"
                )
                return self._record_decision(
                    issue, "same_pc_" + admission_reason, True,
                    validation_source, relation, global_bypass_before,
                    issuer_lookup=current_lookup, owner_lookup=current_lookup,
                )

            self.rr_owner_cross_pc_hits += 1
            current_lookup = self.lookup(trigger_pc, bop_kind, best_offset)
            issue, admission_reason = self._consumer_admission(
                current_lookup, trigger_pc, bop_kind, trigger_line,
                best_offset,
            )
            self.submit_validation(
                current_lookup, trigger_pc, trigger_line, False
            )
            self.consumer_miss_updates += 1

            owner_lookup = self.lookup(
                validation_owner_pc, bop_kind, best_offset
            )
            submit_producer_credit(owner_lookup, 0, "cross_pc")
            return self._record_decision(
                issue, "cross_pc_" + admission_reason, True,
                validation_source, "cross_pc", global_bypass_before,
                issuer_lookup=current_lookup, owner_lookup=owner_lookup,
            )

        issue = True
        if trigger_has_pc:
            lookup = self.lookup(trigger_pc, bop_kind, best_offset)
            if not admission_hit:
                issue, admission_reason = self._consumer_admission(
                    lookup, trigger_pc, bop_kind, trigger_line, best_offset
                )
            else:
                admission_reason = "validation_hit"
            credit_hit = validation_hit_bool
            if recovered_hit and not self._recovered_producer_credit_allowed(
                    "same_pc"):
                credit_hit = False
                self.recovered_producer_credit_suppressed += 1
            self.submit_validation(
                lookup,
                trigger_pc,
                trigger_line,
                credit_hit,
                self._recovered_hit_increment() if recovered_hit and credit_hit else 0,
            )
            if (self.config.pc_validation_producer_consumer
                    and not admission_hit):
                self.consumer_miss_updates += 1
            return self._record_decision(
                issue, admission_reason, validation_hit_bool, validation_source,
                "none", global_bypass_before, issuer_lookup=lookup,
            )
        elif not admission_hit:
            self.note_pc_validation_miss()
            issue = self.global_bypass
            return self._record_decision(
                issue, "no_pc_global_bypass" if issue else "no_pc_suppressed",
                False, validation_source, "no_trigger_pc",
                global_bypass_before,
            )
        return self._record_decision(
            issue, "no_pc_validation_hit", validation_hit_bool,
            validation_source, "no_trigger_pc", global_bypass_before,
        )

    def policy_candidate(self, event: ReplayEvent) -> bool:
        return self.policy_candidate_values(
            bop_kind=event.bop_kind,
            best_offset=event.selected_offset or event.best_offset_after,
            best_offset_changed=event.best_offset_changed,
            raw_candidate_valid=event.raw_candidate_valid,
            pc_confidence_enabled=event.pc_confidence_enabled,
            validation_enabled=event.validation_enabled,
            validation_hit=event.validation_hit,
            trigger_addr=event.trigger_addr,
            trigger_pc=event.trigger_pc,
            trigger_has_pc=event.trigger_has_pc,
            validation_owner_pc=0,
            validation_owner_valid=False,
        )

    def policy_candidate_output(
        self, output: LearnerOutput, config: BOPConfig,
    ) -> bool:
        event = output.event
        return self.policy_candidate_values(
            bop_kind=output.kind,
            best_offset=output.selected_offset or output.best_offset_after,
            best_offset_changed=output.best_offset_changed,
            raw_candidate_valid=output.raw_candidate_valid,
            pc_confidence_enabled=config.pc_validation_confidence,
            validation_enabled=output.validation_enabled,
            validation_hit=output.validation_hit,
            trigger_addr=event.trigger_addr,
            trigger_pc=event.trigger_pc,
            trigger_has_pc=event.trigger_has_pc,
            validation_owner_pc=output.validation_owner_pc,
            validation_owner_valid=output.validation_owner_valid,
        )


class DemandOracle:
    """Labels emitted candidates using later L2 demand reads only."""

    def __init__(
        self,
        horizon: int,
        on_outcome: Callable[[bool], None] | None = None,
        *,
        keep_candidates: bool = True,
        on_resolve: Callable[[Candidate], None] | None = None,
    ):
        if horizon <= 0:
            raise ValueError("demand horizon must be positive")
        self.horizon = horizon
        self.on_outcome = on_outcome
        self.on_resolve = on_resolve
        self.keep_candidates = keep_candidates
        self.demand_index = 0
        self._next_candidate_id = 0
        self._all_candidates: list[Candidate] = []
        self._pending_by_line: dict[int, deque[Candidate]] = defaultdict(deque)
        self._expiry_heap: list[tuple[int, int, Candidate]] = []

    @property
    def candidates(self) -> Sequence[Candidate]:
        return self._all_candidates

    def emit(
        self, kind: str, access_seq: int, tick: int, addr: int,
        phase_id: int = 0,
    ) -> Candidate:
        candidate = Candidate(
            candidate_id=self._next_candidate_id,
            kind=kind,
            access_seq=access_seq,
            tick=tick,
            addr=addr,
            demand_index_at_issue=self.demand_index,
            phase_id=phase_id,
        )
        self._next_candidate_id += 1
        if self.keep_candidates:
            self._all_candidates.append(candidate)
        self._pending_by_line[addr].append(candidate)
        heapq.heappush(
            self._expiry_heap,
            (self.demand_index + self.horizon, candidate.candidate_id, candidate),
        )
        return candidate

    def _resolve(self, candidate: Candidate, status: str, demand: Demand | None) -> None:
        if candidate.status != "pending":
            return
        candidate.status = status
        if demand is not None:
            candidate.matched_demand_seq = demand.access_seq
            candidate.demand_distance = self.demand_index - candidate.demand_index_at_issue
            candidate.tick_distance = demand.tick - candidate.tick
        if self.on_outcome is not None and status != "censored":
            self.on_outcome(status == "useful")
        if self.on_resolve is not None:
            self.on_resolve(candidate)
        if not self.keep_candidates:
            self._discard_resolved_prefix(candidate.addr)

    def _discard_resolved_prefix(self, addr: int) -> None:
        """Bound no-retention replay memory by removing resolved line entries."""
        pending = self._pending_by_line.get(addr)
        if pending is None:
            return
        while pending and pending[0].status != "pending":
            pending.popleft()
        if not pending:
            self._pending_by_line.pop(addr, None)

    def observe_demand(self, demand: Demand) -> None:
        self.demand_index += 1
        while self._expiry_heap and self._expiry_heap[0][0] < self.demand_index:
            _, _, candidate = heapq.heappop(self._expiry_heap)
            self._resolve(candidate, "unused", None)

        pending = self._pending_by_line.get(demand.addr)
        if pending is None:
            return
        while pending and pending[0].status != "pending":
            pending.popleft()
        if not pending:
            return
        self._resolve(pending.popleft(), "useful", demand)
        while pending:
            self._resolve(pending.popleft(), "redundant", demand)

    def finish(self) -> Sequence[Candidate]:
        if self.keep_candidates:
            for candidate in self._all_candidates:
                self._resolve(candidate, "censored", None)
        else:
            while self._expiry_heap:
                _, _, candidate = heapq.heappop(self._expiry_heap)
                self._resolve(candidate, "censored", None)
        return self._all_candidates


@dataclass(frozen=True)
class QualityMetrics:
    candidates: int
    useful: int
    unused: int
    redundant: int
    censored: int
    eligible_demands: int
    covered_demands: int
    accuracy: float | None
    coverage: float | None
    mean_demand_distance: float | None
    mean_tick_distance: float | None


def evaluate_candidates(
    demands: Iterable[Demand], candidates: Iterable[Candidate], horizon: int
) -> QualityMetrics:
    oracle = DemandOracle(horizon)
    by_access: dict[int, list[Candidate]] = defaultdict(list)
    for candidate in candidates:
        by_access[candidate.access_seq].append(candidate)
    demand_list = sorted(demands, key=lambda demand: demand.access_seq)
    demands_by_access = {demand.access_seq: demand for demand in demand_list}
    for access_seq in sorted(set(demands_by_access) | set(by_access)):
        demand = demands_by_access.get(access_seq)
        if demand is not None:
            oracle.observe_demand(demand)
        for candidate in by_access.pop(access_seq, []):
            oracle.emit(candidate.kind, candidate.access_seq, candidate.tick, candidate.addr)
    resolved = oracle.finish()
    return _metrics(resolved, len(demand_list))


def _in_evaluation_window(
    item: Demand | ReplayEvent | Candidate, window: EvaluationWindow,
) -> bool:
    return _in_evaluation_values(item.phase_id, item.tick, window)


def _in_evaluation_values(
    phase_id: int, tick: int, window: EvaluationWindow,
) -> bool:
    if window.phase_id is not None:
        return phase_id == window.phase_id
    if window.start_tick is not None:
        return tick >= window.start_tick
    return True


def evaluate_window(
    demands: Sequence[Demand], candidates: Sequence[Candidate], horizon: int,
    window: EvaluationWindow | None = None,
) -> dict[str, QualityMetrics]:
    """Report a phase/tick window without resetting replay state.

    Candidate quality is intentionally closed over the selected demand stream:
    a candidate issued in the stable phase can only be labeled by a later
    stable-phase demand, and coverage uses stable-phase demand count.
    """
    selected_window = window or EvaluationWindow()
    selected_demands = [
        demand for demand in demands
        if _in_evaluation_window(demand, selected_window)
    ]
    selected_candidates = [
        candidate for candidate in candidates
        if _in_evaluation_window(candidate, selected_window)
    ]
    return {
        "large": evaluate_candidates(
            selected_demands,
            (item for item in selected_candidates if item.kind == "large"),
            horizon,
        ),
        "small": evaluate_candidates(
            selected_demands,
            (item for item in selected_candidates if item.kind == "small"),
            horizon,
        ),
        "combined": evaluate_candidates(
            selected_demands, selected_candidates, horizon
        ),
    }


def _metrics(candidates: Sequence[Candidate], eligible_demands: int) -> QualityMetrics:
    counts = {status: 0 for status in ("useful", "unused", "redundant", "censored")}
    useful_distances: list[int] = []
    useful_ticks: list[int] = []
    for candidate in candidates:
        counts[candidate.status] += 1
        if candidate.status == "useful":
            assert candidate.demand_distance is not None
            assert candidate.tick_distance is not None
            useful_distances.append(candidate.demand_distance)
            useful_ticks.append(candidate.tick_distance)
    denominator = counts["useful"] + counts["unused"] + counts["redundant"]
    return QualityMetrics(
        candidates=len(candidates),
        useful=counts["useful"],
        unused=counts["unused"],
        redundant=counts["redundant"],
        censored=counts["censored"],
        eligible_demands=eligible_demands,
        covered_demands=counts["useful"],
        accuracy=(counts["useful"] / denominator) if denominator else None,
        coverage=(counts["useful"] / eligible_demands) if eligible_demands else None,
        mean_demand_distance=(sum(useful_distances) / len(useful_distances))
        if useful_distances
        else None,
        mean_tick_distance=(sum(useful_ticks) / len(useful_ticks))
        if useful_ticks
        else None,
    )


class _StreamingQuality:
    """Accumulate one quality view without retaining its resolved candidates."""

    def __init__(self, horizon: int):
        self.eligible_demands = 0
        self.counts = {
            status: 0 for status in ("useful", "unused", "redundant", "censored")
        }
        self.useful_demand_distance_sum = 0
        self.useful_tick_distance_sum = 0
        self.oracle = DemandOracle(
            horizon, keep_candidates=False, on_resolve=self._record_resolution
        )

    def observe_demand(self, demand: Demand) -> None:
        self.eligible_demands += 1
        self.oracle.observe_demand(demand)

    def emit(
        self, kind: str, access_seq: int, tick: int, addr: int, phase_id: int,
    ) -> None:
        self.oracle.emit(kind, access_seq, tick, addr, phase_id)

    def _record_resolution(self, candidate: Candidate) -> None:
        self.counts[candidate.status] += 1
        if candidate.status == "useful":
            assert candidate.demand_distance is not None
            assert candidate.tick_distance is not None
            self.useful_demand_distance_sum += candidate.demand_distance
            self.useful_tick_distance_sum += candidate.tick_distance

    def finish(self) -> QualityMetrics:
        self.oracle.finish()
        denominator = (
            self.counts["useful"]
            + self.counts["unused"]
            + self.counts["redundant"]
        )
        return QualityMetrics(
            candidates=sum(self.counts.values()),
            useful=self.counts["useful"],
            unused=self.counts["unused"],
            redundant=self.counts["redundant"],
            censored=self.counts["censored"],
            eligible_demands=self.eligible_demands,
            covered_demands=self.counts["useful"],
            accuracy=(self.counts["useful"] / denominator) if denominator else None,
            coverage=(self.counts["useful"] / self.eligible_demands)
            if self.eligible_demands
            else None,
            mean_demand_distance=(
                self.useful_demand_distance_sum / self.counts["useful"]
            )
            if self.counts["useful"]
            else None,
            mean_tick_distance=(
                self.useful_tick_distance_sum / self.counts["useful"]
            )
            if self.counts["useful"]
            else None,
        )


def _candidate_from_event(event: ReplayEvent, mode: str) -> tuple[bool, int]:
    if mode == "recorded":
        return event.policy_candidate_valid, event.policy_candidate_addr
    if mode == "raw":
        return event.raw_candidate_valid, event.raw_candidate_addr
    raise ValueError(f"unsupported direct candidate mode: {mode}")


def _event_candidates(
    events: Sequence[ReplayEvent], mode: str,
) -> list[Candidate]:
    candidates = []
    for event in events:
        valid, address = _candidate_from_event(event, mode)
        if valid:
            candidates.append(
                Candidate(
                    candidate_id=len(candidates),
                    kind=event.bop_kind,
                    access_seq=event.access_seq,
                    tick=event.tick,
                    addr=address,
                    demand_index_at_issue=0,
                    phase_id=event.phase_id,
                )
            )
    return candidates


def _learner_raw_candidates(
    outputs: Sequence[LearnerOutput],
) -> list[Candidate]:
    return [
        Candidate(
            candidate_id=index,
            kind=output.kind,
            access_seq=output.event.access_seq,
            tick=output.event.tick,
            addr=output.raw_candidate_addr,
            demand_index_at_issue=0,
            phase_id=output.event.phase_id,
        )
        for index, output in enumerate(outputs)
        if output.raw_candidate_valid
    ]


def replay_controller(
    demands: Sequence[Demand], events: Sequence[ReplayEvent], config: BOPConfig, horizon: int
) -> list[Candidate]:
    controller = PCValidationController(config)
    oracle = DemandOracle(horizon, controller.note_outcome)
    demands_by_seq = {demand.access_seq: demand for demand in demands}
    events_by_seq: dict[int, list[ReplayEvent]] = defaultdict(list)
    for event in events:
        events_by_seq[event.access_seq].append(event)

    for access_seq in sorted(set(demands_by_seq) | set(events_by_seq)):
        demand = demands_by_seq.get(access_seq)
        if demand is not None:
            oracle.observe_demand(demand)
        for event in sorted(events_by_seq[access_seq], key=lambda item: item.order):
            if controller.policy_candidate(event):
                oracle.emit(
                    event.bop_kind, event.access_seq, event.tick,
                    event.raw_candidate_addr, event.phase_id,
                )
                controller.note_issued()
        controller.commit()
    return list(oracle.finish())


def _table_columns(connection: sqlite3.Connection, table_name: str) -> set[str]:
    return {
        str(row[1])
        for row in connection.execute(f"PRAGMA table_info({table_name})")
    }


def _sqlite_u64(value: object) -> int:
    """Recover gem5 Addr bits from SQLite's signed INT64 representation."""
    return int(value) & UINT64_MASK


def _load_trace_connection(
    connection: sqlite3.Connection,
) -> tuple[list[Demand], list[ReplayEvent], BOPConfig, list[ReplayPhase]]:
    connection.row_factory = sqlite3.Row
    meta_rows = connection.execute("SELECT * FROM BOPReplayMeta ORDER BY BOPName").fetchall()
    if not meta_rows:
        raise ValueError("BOPReplayMeta is empty; enable --dump-bop-replay-trace")
    schema_versions = {int(row["SchemaVersion"]) for row in meta_rows}
    if not schema_versions.issubset({1, 2, 3, 4, 5, SCHEMA_VERSION}):
        raise ValueError(f"unsupported replay schema version(s): {sorted(schema_versions)}")
    parsed_configs = {
        str(row["BOPName"]): BOPConfig.from_meta(row) for row in meta_rows
    }
    schema_version = next(iter(schema_versions))

    demand_columns = _table_columns(connection, "L2DemandTrace")
    event_columns = _table_columns(connection, "BOPReplayEvent")
    demand_phase_column = "PhaseId" if "PhaseId" in demand_columns else "0"
    event_phase_column = "PhaseId" if "PhaseId" in event_columns else "0"
    demands = [
        Demand(
            int(row["AccessSeq"]), int(row["Tick"]), _sqlite_u64(row["Addr"]),
            int(row["PhaseId"]),
        )
        for row in connection.execute(
            "SELECT AccessSeq,Tick,Addr,"
            f"{demand_phase_column} AS PhaseId "
            "FROM L2DemandTrace ORDER BY AccessSeq"
        )
    ]
    event_rows = connection.execute(
        "SELECT rowid AS TraceOrder,*,"
        f"{event_phase_column} AS LoadedPhaseId "
        "FROM BOPReplayEvent ORDER BY AccessSeq,rowid"
    ).fetchall()

    phases = []
    if "BOPReplayPhase" in {
        str(row[0])
        for row in connection.execute(
            "SELECT name FROM sqlite_master WHERE type='table'"
        )
    }:
        phases = [
            ReplayPhase(
                int(row["PhaseId"]), str(row["PhaseName"]),
                int(row["StartTick"]),
            )
            for row in connection.execute(
                "SELECT PhaseId,PhaseName,StartTick FROM BOPReplayPhase "
                "ORDER BY PhaseId"
            )
        ]

    def event_value(row: sqlite3.Row, name: str, default: object) -> object:
        try:
            return row[name]
        except (KeyError, IndexError):
            return default

    actions_by_bop: dict[str, tuple[ReplayDelayAction, ...]] = {}
    table_names = {
        str(row[0])
        for row in connection.execute(
            "SELECT name FROM sqlite_master WHERE type='table'"
        )
    }
    if schema_version >= LEARNER_REPLAY_CERTIFICATION_MIN_SCHEMA_VERSION:
        if "BOPReplayDelayAction" not in table_names:
            raise ValueError(
                "schema V5 trace is missing BOPReplayDelayAction"
            )
        actions_by_bop = {
            bop_name: tuple(
                ReplayDelayAction(
                    bop_name=str(row["BOPName"]),
                    replay_order=int(row["ReplayOrder"]),
                    action=str(row["Action"]),
                    tick=int(row["Tick"]),
                    addr=_sqlite_u64(row["Addr"]),
                    process_tick=int(row["ProcessTick"]),
                    queue_size_after=int(row["QueueSizeAfter"]),
                )
                for row in connection.execute(
                    "SELECT BOPName,ReplayOrder,Action,Tick,Addr,ProcessTick,"
                    "QueueSizeAfter FROM BOPReplayDelayAction "
                    "WHERE BOPName=? ORDER BY ReplayOrder",
                    (bop_name,),
                )
            )
            for bop_name in parsed_configs
        }
        missing = [
            bop_name for bop_name, learner_config in parsed_configs.items()
            if learner_config.delay_queue_enabled and not actions_by_bop[bop_name]
        ]
        if missing:
            raise ValueError(
                "schema V5 trace is missing delay actions for: "
                + ", ".join(missing)
            )
        parsed_configs = {
            bop_name: replace(
                learner_config, replay_delay_actions=actions_by_bop,
            )
            for bop_name, learner_config in parsed_configs.items()
        }
    config = replace(
        parsed_configs[str(meta_rows[0]["BOPName"])],
        learner_configs=parsed_configs,
        replay_delay_actions=actions_by_bop,
    )

    events = [
        ReplayEvent(
            access_seq=int(event_value(row, "AccessSeq", 0)),
            order=int(row["TraceOrder"]),
            bop_name=str(event_value(row, "BOPName", "large")),
            bop_kind=str(event_value(row, "BOPKind", "large")),
            tick=int(event_value(row, "Tick", 0)),
            trigger_addr=_sqlite_u64(event_value(row, "TriggerAddr", 0)),
            trigger_pc=_sqlite_u64(event_value(row, "TriggerPC", 0)),
            trigger_has_pc=bool(event_value(row, "TriggerHasPC", False)),
            validation_hit=int(event_value(row, "ValidationHit", -1)),
            best_offset_changed=bool(event_value(row, "BestOffsetChanged", False)),
            issue_enabled=bool(event_value(row, "IssueEnabled", False)),
            validation_enabled=bool(event_value(row, "ValidationEnabled", False)),
            pc_confidence_enabled=bool(event_value(row, "PCConfidenceEnabled", False)),
            pc_sampled=bool(event_value(row, "PCSampled", False)),
            raw_candidate_valid=bool(event_value(row, "RawCandidateValid", False)),
            raw_candidate_addr=_sqlite_u64(
                event_value(row, "RawCandidateAddr", 0)
            ),
            policy_candidate_valid=bool(event_value(row, "PolicyCandidateValid", False)),
            policy_candidate_addr=_sqlite_u64(
                event_value(row, "PolicyCandidateAddr", 0)
            ),
            late=bool(event_value(row, "Late", False)),
            best_offset_before=int(event_value(row, "BestOffsetBefore", 0)),
            best_offset_after=int(event_value(row, "BestOffsetAfter", 0)),
            teacher_issue_enabled=bool(
                event_value(row, "TeacherIssueEnabled", False)),
            student_issue_enabled=bool(
                event_value(row, "StudentIssueEnabled", False)),
            student_selected_valid=bool(
                event_value(row, "StudentSelectedValid", False)),
            student_selected_enable=bool(
                event_value(row, "StudentSelectedEnable", False)),
            student_selected_offset=int(
                event_value(row, "StudentSelectedOffset", 0)),
            selected_offset=int(event_value(row, "SelectedOffset", 0)),
            best_score=int(event_value(row, "BestScore", 0)),
            round=int(event_value(row, "Round", 0)),
            online_generated=bool(event_value(row, "OnlineGenerated", False)),
            online_buffered=bool(event_value(row, "OnlineBuffered", False)),
            online_filtered=bool(event_value(row, "OnlineFiltered", False)),
            online_filter_passed=bool(event_value(row, "OnlineFilterPassed", False)),
            phase_id=int(event_value(row, "LoadedPhaseId", 0)),
            replay_order=int(event_value(row, "ReplayOrder", row["TraceOrder"])),
            trigger_is_demand=bool(event_value(row, "TriggerIsDemand", True)),
            trigger_is_read=bool(event_value(row, "TriggerIsRead", True)),
        )
        for row in event_rows
    ]
    if not events:
        raise ValueError("BOPReplayEvent is empty")
    return demands, events, config, phases


def load_trace_connection(
    connection: sqlite3.Connection,
) -> tuple[list[Demand], list[ReplayEvent], BOPConfig]:
    demands, events, config, _ = _load_trace_connection(connection)
    return demands, events, config


def load_trace(
    path: Path,
) -> tuple[list[Demand], list[ReplayEvent], BOPConfig]:
    with sqlite3.connect(path) as connection:
        return load_trace_connection(connection)


def load_trace_with_phases(
    path: Path,
) -> tuple[list[Demand], list[ReplayEvent], BOPConfig, list[ReplayPhase]]:
    with sqlite3.connect(path) as connection:
        return _load_trace_connection(connection)


def _stats_block_final_tick(path: Path, block_number: int) -> int:
    if block_number <= 0:
        raise ValueError("stats block numbers start at 1")
    begin_marker = "---------- Begin Simulation Statistics ----------"
    end_marker = "---------- End Simulation Statistics   ----------"
    blocks: list[int] = []
    in_block = False
    final_tick = None
    for line in path.read_text().splitlines():
        if line == begin_marker:
            in_block = True
            final_tick = None
            continue
        if in_block and line == end_marker:
            if final_tick is None:
                raise ValueError(
                    f"stats block {len(blocks) + 1} has no finalTick"
                )
            blocks.append(final_tick)
            in_block = False
            continue
        if in_block:
            match = re.match(r"^finalTick\s+(\d+)\b", line)
            if match:
                final_tick = int(match.group(1))
    if in_block:
        raise ValueError(f"unterminated statistics block in {path}")
    if block_number > len(blocks):
        raise ValueError(
            f"stats block {block_number} is unavailable in {path}; "
            f"found {len(blocks)} block(s)"
        )
    return blocks[block_number - 1]


def resolve_evaluation_window(
    *, phase_name: str, start_tick: int | None, stats_path: Path | None,
    stats_block: int, phases: Sequence[ReplayPhase],
) -> EvaluationWindow:
    selector_count = sum((
        phase_name != "full",
        start_tick is not None,
        stats_path is not None,
    ))
    if selector_count > 1:
        raise ValueError(
            "choose only one evaluation selector: phase, start tick, or stats"
        )
    if start_tick is not None:
        if start_tick < 0:
            raise ValueError("evaluation start tick must be non-negative")
        return EvaluationWindow(
            name=f"tick_{start_tick}", source="explicit_tick",
            start_tick=start_tick,
        )
    if stats_path is not None:
        final_tick = _stats_block_final_tick(stats_path, stats_block)
        return EvaluationWindow(
            name=f"stats_block_{stats_block}", source="stats_file",
            start_tick=final_tick, stats_path=str(stats_path),
            stats_block=stats_block,
        )
    if phase_name == "full":
        return EvaluationWindow()
    phase = next((item for item in phases if item.name == phase_name), None)
    if phase is None:
        available = ", ".join(item.name for item in phases) or "none"
        raise ValueError(
            f"evaluation phase {phase_name!r} is unavailable; "
            f"available trace phases: {available}"
        )
    return EvaluationWindow(
        name=phase.name, source="trace_phase", phase_id=phase.phase_id,
        phase_name=phase.name, start_tick=phase.start_tick,
    )


def evaluation_window_report(
    window: EvaluationWindow, demands: Sequence[Demand],
    events: Sequence[ReplayEvent],
) -> dict[str, object]:
    return {
        "name": window.name,
        "source": window.source,
        "phase_id": window.phase_id,
        "phase_name": window.phase_name,
        "start_tick": window.start_tick,
        "stats_path": window.stats_path,
        "stats_block": window.stats_block,
        "demands": sum(
            _in_evaluation_window(demand, window) for demand in demands
        ),
        "events": sum(
            _in_evaluation_window(event, window) for event in events
        ),
        "state_replay": "full_trace",
        "demand_oracle": "selected_window_only",
    }


def run_quality(
    demands: Sequence[Demand],
    events: Sequence[ReplayEvent],
    config: BOPConfig,
    horizon: int,
    mode: str,
) -> dict[str, QualityMetrics]:
    if mode == "replay-controller":
        candidates = replay_controller(demands, events, config, horizon)
    else:
        candidates = _event_candidates(events, mode)
    return evaluate_window(demands, candidates, horizon)


def run_learner_quality(
    demands: Sequence[Demand], events: Sequence[ReplayEvent], config: BOPConfig,
    horizon: int, candidate_stage: str = "raw",
) -> tuple[dict[str, QualityMetrics], list[LearnerOutput]]:
    outputs = replay_learner(events, config)
    return (
        learner_quality_from_outputs(
            demands, outputs, config, horizon, candidate_stage
        ),
        outputs,
    )


def learner_quality_from_outputs(
    demands: Sequence[Demand], outputs: Sequence[LearnerOutput], config: BOPConfig,
    horizon: int, candidate_stage: str = "raw",
    window: EvaluationWindow | None = None,
) -> dict[str, QualityMetrics]:
    """Evaluate one horizon from a previously replayed learner stream.

    Learner state only depends on the recorded trigger stream.  The optional
    policy stage additionally uses the horizon-specific demand oracle, so
    candidate admission and metrics remain evaluated independently here.
    """
    if candidate_stage == "raw":
        candidates = _learner_raw_candidates(outputs)
    elif candidate_stage == "policy":
        return replay_learner_policy_metrics(demands, outputs, config, horizon, window)
    else:
        raise ValueError(f"unsupported learner candidate stage: {candidate_stage!r}")
    return evaluate_window(demands, candidates, horizon, window)


def _learner_policy_event(output: LearnerOutput, config: BOPConfig) -> ReplayEvent:
    """Materialize learner-derived BOP validation state for controller replay."""
    return replace(
        output.event,
        best_offset_changed=output.best_offset_changed,
        issue_enabled=output.issue_enabled,
        validation_enabled=output.validation_enabled,
        validation_hit=output.validation_hit,
        pc_confidence_enabled=config.pc_validation_confidence,
        raw_candidate_valid=output.raw_candidate_valid,
        raw_candidate_addr=output.raw_candidate_addr,
    )


def replay_learner_controller(
    demands: Sequence[Demand], outputs: Sequence[LearnerOutput], config: BOPConfig,
    horizon: int,
) -> list[Candidate]:
    """Replay BOP learner plus PC/global control with demand-oracle outcomes.

    The global bypass input intentionally uses the demand oracle rather than
    online cache outcomes. This is the defined filter-free offline policy
    model, so its policy candidate stream is not a baseline-golden field.
    """
    large_config = config.for_kind("large")
    small_config = config.for_kind("small")
    mismatched = [
        field
        for field in PC_SHARED_CONFIG_FIELDS
        if getattr(large_config, field) != getattr(small_config, field)
    ]
    if mismatched:
        raise ValueError(
            "large/small PC validation configurations must match for the "
            f"shared online controller: {', '.join(mismatched)}"
        )
    controller = PCValidationController(large_config)
    oracle = DemandOracle(horizon, controller.note_outcome)
    demands_by_seq = {demand.access_seq: demand for demand in demands}
    outputs_by_seq: dict[int, list[LearnerOutput]] = defaultdict(list)
    for output in outputs:
        outputs_by_seq[output.event.access_seq].append(output)

    for access_seq in sorted(set(demands_by_seq) | set(outputs_by_seq)):
        demand = demands_by_seq.get(access_seq)
        if demand is not None:
            oracle.observe_demand(demand)
        for output in sorted(
            outputs_by_seq[access_seq], key=lambda item: item.event.order
        ):
            if controller.policy_candidate_output(
                output, config.for_kind(output.kind)
            ):
                oracle.emit(
                    output.kind, output.event.access_seq, output.event.tick,
                    output.raw_candidate_addr, output.event.phase_id,
                )
                # With local filtering intentionally absent, every policy
                # candidate is treated as admitted into the global window.
                controller.note_issued()
        controller.commit()
    return list(oracle.finish())


def replay_learner_policy_metrics(
    demands: Sequence[Demand], outputs: Sequence[LearnerOutput], config: BOPConfig,
    horizon: int, window: EvaluationWindow | None = None,
) -> dict[str, QualityMetrics]:
    """Evaluate policy candidates while retaining only unresolved candidates.

    The controller's full-trace demand oracle remains the source of global
    bypass feedback.  The three stable-window quality views receive the same
    ordered demand/candidate stream that ``evaluate_window`` would consume,
    but release candidates as soon as their labels resolve.
    """
    large_config = config.for_kind("large")
    small_config = config.for_kind("small")
    mismatched = [
        field
        for field in PC_SHARED_CONFIG_FIELDS
        if getattr(large_config, field) != getattr(small_config, field)
    ]
    if mismatched:
        raise ValueError(
            "large/small PC validation configurations must match for the "
            f"shared online controller: {', '.join(mismatched)}"
        )

    selected_window = window or EvaluationWindow()
    controller = PCValidationController(large_config)
    outcome_oracle = DemandOracle(
        horizon, controller.note_outcome, keep_candidates=False
    )
    quality = {
        "large": _StreamingQuality(horizon),
        "small": _StreamingQuality(horizon),
        "combined": _StreamingQuality(horizon),
    }
    demands_by_seq = {demand.access_seq: demand for demand in demands}
    outputs_by_seq: dict[int, list[LearnerOutput]] = defaultdict(list)
    for output in outputs:
        outputs_by_seq[output.event.access_seq].append(output)

    for access_seq in sorted(set(demands_by_seq) | set(outputs_by_seq)):
        demand = demands_by_seq.get(access_seq)
        if demand is not None:
            outcome_oracle.observe_demand(demand)
            if _in_evaluation_window(demand, selected_window):
                for accumulator in quality.values():
                    accumulator.observe_demand(demand)
        for output in sorted(
            outputs_by_seq[access_seq], key=lambda item: item.event.order
        ):
            event = output.event
            if controller.policy_candidate_output(
                output, config.for_kind(output.kind)
            ):
                outcome_oracle.emit(
                    output.kind, event.access_seq, event.tick,
                    output.raw_candidate_addr, event.phase_id,
                )
                # With local filtering intentionally absent, every policy
                # candidate is treated as admitted into the global window.
                controller.note_issued()
                if _in_evaluation_window(event, selected_window):
                    quality["combined"].emit(
                        output.kind, event.access_seq, event.tick,
                        output.raw_candidate_addr, event.phase_id,
                    )
                    quality[output.kind].emit(
                        output.kind, event.access_seq, event.tick,
                        output.raw_candidate_addr, event.phase_id,
                    )
        controller.commit()

    outcome_oracle.finish()
    return {kind: accumulator.finish() for kind, accumulator in quality.items()}


@dataclass(frozen=True)
class _StreamingReplayResult:
    quality_by_horizon: Mapping[int, Mapping[str, QualityMetrics]]
    demands: int
    events: int
    window_demands: int
    window_events: int
    verification: Mapping[str, object] | None = None
    controller_stats: Mapping[int, Mapping[str, int]] | None = None


def _streaming_metadata(
    connection: sqlite3.Connection,
) -> tuple[BOPConfig, list[ReplayPhase]]:
    """Read only the small trace metadata needed before streaming rows."""
    connection.row_factory = sqlite3.Row
    meta_rows = connection.execute(
        "SELECT * FROM BOPReplayMeta ORDER BY BOPName"
    ).fetchall()
    if not meta_rows:
        raise ValueError("BOPReplayMeta is empty; enable --dump-bop-replay-trace")
    schema_versions = {int(row["SchemaVersion"]) for row in meta_rows}
    if schema_versions != {SCHEMA_VERSION}:
        raise ValueError(
            "streaming replay requires the current trace schema; use materialized replay "
            f"for schema version(s) {sorted(schema_versions)}"
        )
    parsed_configs = {
        str(row["BOPName"]): BOPConfig.from_meta(row) for row in meta_rows
    }
    phases = [
        ReplayPhase(
            int(row["PhaseId"]), str(row["PhaseName"]), int(row["StartTick"])
        )
        for row in connection.execute(
            "SELECT PhaseId,PhaseName,StartTick FROM BOPReplayPhase "
            "ORDER BY PhaseId"
        )
    ]
    config = replace(
        parsed_configs[str(meta_rows[0]["BOPName"])],
        learner_configs=parsed_configs,
    )
    return config, phases


def _stream_event_from_row(row: sqlite3.Row) -> ReplayEvent:
    return ReplayEvent(
        access_seq=int(row["AccessSeq"]),
        order=int(row["TraceOrder"]),
        bop_name=str(row["BOPName"]),
        bop_kind=str(row["BOPKind"]),
        tick=int(row["Tick"]),
        trigger_addr=_sqlite_u64(row["TriggerAddr"]),
        trigger_pc=_sqlite_u64(row["TriggerPC"]),
        trigger_has_pc=bool(row["TriggerHasPC"]),
        validation_hit=int(row["ValidationHit"]),
        best_offset_changed=bool(row["BestOffsetChanged"]),
        issue_enabled=bool(row["IssueEnabled"]),
        validation_enabled=bool(row["ValidationEnabled"]),
        pc_confidence_enabled=bool(row["PCConfidenceEnabled"]),
        pc_sampled=bool(row["PCSampled"]),
        raw_candidate_valid=bool(row["RawCandidateValid"]),
        raw_candidate_addr=_sqlite_u64(row["RawCandidateAddr"]),
        policy_candidate_valid=bool(row["PolicyCandidateValid"]),
        policy_candidate_addr=_sqlite_u64(row["PolicyCandidateAddr"]),
        late=bool(row["Late"]),
        best_offset_before=int(row["BestOffsetBefore"]),
        best_offset_after=int(row["BestOffsetAfter"]),
        teacher_issue_enabled=bool(row["TeacherIssueEnabled"]),
        student_issue_enabled=bool(row["StudentIssueEnabled"]),
        student_selected_valid=bool(row["StudentSelectedValid"]),
        student_selected_enable=bool(row["StudentSelectedEnable"]),
        student_selected_offset=int(row["StudentSelectedOffset"]),
        selected_offset=int(row["SelectedOffset"]),
        best_score=int(row["BestScore"]),
        round=int(row["Round"]),
        phase_id=int(row["PhaseId"]),
        replay_order=int(row["ReplayOrder"]),
        trigger_is_demand=bool(row["TriggerIsDemand"]),
        trigger_is_read=bool(row["TriggerIsRead"]),
    )


def _stream_delay_action_from_row(row: sqlite3.Row) -> ReplayDelayAction:
    return ReplayDelayAction(
        bop_name=str(row["BOPName"]),
        replay_order=int(row["ReplayOrder"]),
        action=str(row["Action"]),
        tick=int(row["Tick"]),
        addr=_sqlite_u64(row["Addr"]),
        process_tick=int(row["ProcessTick"]),
        queue_size_after=int(row["QueueSizeAfter"]),
    )


def _recorded_delay_actions_compatible(config: BOPConfig) -> bool:
    return (
        config.schema_version >= LEARNER_REPLAY_CERTIFICATION_MIN_SCHEMA_VERSION
        and config.delay_queue_enabled
        and config.trace_delay_queue_enabled == config.delay_queue_enabled
        and config.trace_delay_queue_size == config.delay_queue_size
        and config.trace_delay_ticks == config.delay_ticks
    )


class _DelayActionCursors:
    """Bounded V5 delay-action cursors, one ordered stream per BOP."""

    def __init__(self, connection: sqlite3.Connection, bop_names: Iterable[str]):
        self.cursors: dict[str, Iterator[sqlite3.Row]] = {}
        self.next_rows: dict[str, sqlite3.Row | None] = {}
        for bop_name in bop_names:
            cursor = iter(connection.execute(
                "SELECT BOPName,ReplayOrder,Action,Tick,Addr,ProcessTick,"
                "QueueSizeAfter FROM BOPReplayDelayAction WHERE BOPName=? "
                "ORDER BY ReplayOrder",
                (bop_name,),
            ))
            self.cursors[bop_name] = cursor
            self.next_rows[bop_name] = next(cursor, None)
            if self.next_rows[bop_name] is None:
                raise ValueError(
                    f"V5 trace is missing BOPReplayDelayAction for {bop_name}"
                )

    def _advance(self, bop_name: str) -> ReplayDelayAction:
        row = self.next_rows[bop_name]
        if row is None:
            raise ValueError(
                f"{bop_name}: delay-action stream ended before learner event"
            )
        self.next_rows[bop_name] = next(self.cursors[bop_name], None)
        return _stream_delay_action_from_row(row)

    def trigger_action(
        self, event: ReplayEvent, learner: BOPLearner,
    ) -> ReplayDelayAction | None:
        row = self.next_rows[event.bop_name]
        while row is not None and int(row["ReplayOrder"]) < event.replay_order:
            action = self._advance(event.bop_name)
            if action.action != "dequeue_to_rr":
                raise ValueError(
                    f"{event.bop_name}: unexpected {action.action!r} before "
                    "learner trigger"
                )
            learner.apply_delay_action(action)
            row = self.next_rows[event.bop_name]
        if row is not None and int(row["ReplayOrder"]) == event.replay_order:
            return self._advance(event.bop_name)
        return None


class _StreamingMetricSet:
    """Maintain one bounded quality accumulator per horizon and BOP view."""

    def __init__(self, horizons: Sequence[int]):
        self.quality = {
            horizon: {
                "large": _StreamingQuality(horizon),
                "small": _StreamingQuality(horizon),
                "combined": _StreamingQuality(horizon),
            }
            for horizon in horizons
        }

    def observe_demand(self, demand: Demand, selected: bool) -> None:
        if not selected:
            return
        for quality in self.quality.values():
            for accumulator in quality.values():
                accumulator.observe_demand(demand)

    def emit(
        self, kind: str, access_seq: int, tick: int, addr: int,
        phase_id: int, selected: bool,
    ) -> None:
        if not selected:
            return
        for quality in self.quality.values():
            quality["combined"].emit(kind, access_seq, tick, addr, phase_id)
            quality[kind].emit(kind, access_seq, tick, addr, phase_id)

    def finish(self) -> dict[int, dict[str, QualityMetrics]]:
        return {
            horizon: {
                kind: accumulator.finish()
                for kind, accumulator in quality.items()
            }
            for horizon, quality in self.quality.items()
        }


def _shared_controller_config(config: BOPConfig) -> BOPConfig:
    large_config = config.for_kind("large")
    small_config = config.for_kind("small")
    mismatched = [
        field for field in PC_SHARED_CONFIG_FIELDS
        if getattr(large_config, field) != getattr(small_config, field)
    ]
    if mismatched:
        raise ValueError(
            "large/small PC validation configurations must match for the "
            f"shared online controller: {', '.join(mismatched)}"
        )
    return large_config


class _StreamingPolicyReplay:
    """One horizon's controller state and selected-window quality stream."""

    def __init__(
        self, config: BOPConfig, horizon: int, window: EvaluationWindow,
        *, on_candidate_resolve: Callable[[Candidate], None] | None = None,
    ):
        self.config = config
        self.window = window
        self.controller = PCValidationController(_shared_controller_config(config))
        self.outcome_oracle = DemandOracle(
            horizon, self.controller.note_outcome, keep_candidates=False,
            on_resolve=on_candidate_resolve,
        )
        self.quality = _StreamingMetricSet((horizon,))

    def observe_demand(self, demand: Demand) -> None:
        self.outcome_oracle.observe_demand(demand)
        self.quality.observe_demand(
            demand, _in_evaluation_window(demand, self.window)
        )

    def emit_event(self, event: ReplayEvent) -> Candidate | None:
        if not self.controller.policy_candidate(event):
            return None
        return self._emit(
            event.bop_kind, event.access_seq, event.tick,
            event.raw_candidate_addr, event.phase_id,
            _in_evaluation_window(event, self.window),
        )

    def emit_values(
        self, *, bop_kind: str, best_offset: int, best_offset_changed: bool,
        raw_candidate_valid: bool, raw_candidate_addr: int,
        pc_confidence_enabled: bool, validation_enabled: bool,
        validation_hit: int, trigger_addr: int, trigger_pc: int,
        trigger_has_pc: bool, access_seq: int, tick: int, phase_id: int,
        selected: bool, validation_owner_pc: int = 0,
        validation_owner_valid: bool = False,
        validation_source: str = "native",
    ) -> Candidate | None:
        """Replay a recorded raw candidate without a ReplayEvent allocation."""
        if not self.controller.policy_candidate_values(
            bop_kind=bop_kind,
            best_offset=best_offset,
            best_offset_changed=best_offset_changed,
            raw_candidate_valid=raw_candidate_valid,
            pc_confidence_enabled=pc_confidence_enabled,
            validation_enabled=validation_enabled,
            validation_hit=validation_hit,
            trigger_addr=trigger_addr,
            trigger_pc=trigger_pc,
            trigger_has_pc=trigger_has_pc,
            validation_owner_pc=validation_owner_pc,
            validation_owner_valid=validation_owner_valid,
            validation_source=validation_source,
        ):
            return None
        return self._emit(
            bop_kind, access_seq, tick, raw_candidate_addr, phase_id, selected,
        )

    def emit_output(self, output: LearnerOutput) -> Candidate | None:
        event = output.event
        if not self.controller.policy_candidate_output(
            output, self.config.for_kind(output.kind)
        ):
            return None
        return self._emit(
            output.kind, event.access_seq, event.tick,
            output.raw_candidate_addr, event.phase_id,
            _in_evaluation_window(event, self.window),
        )

    def _emit(
        self, kind: str, access_seq: int, tick: int, addr: int,
        phase_id: int, selected: bool,
    ) -> Candidate:
        candidate = self.outcome_oracle.emit(kind, access_seq, tick, addr, phase_id)
        self.controller.note_issued()
        self.quality.emit(kind, access_seq, tick, addr, phase_id, selected)
        return candidate

    def commit(self) -> None:
        self.controller.commit()

    def finish(self) -> dict[str, QualityMetrics]:
        self.outcome_oracle.finish()
        return self.quality.finish()[next(iter(self.quality.quality))]


class _OnlineVerifier:
    _FIELDS = (
        "best_offset_before", "best_offset_after", "best_score", "round",
        "best_offset_changed", "selected_offset", "issue_enabled",
        "raw_candidate_valid", "raw_candidate_addr",
    )

    def __init__(self):
        self.total_events = 0
        self.mismatched_events = 0
        self.first_mismatch: dict[str, object] | None = None

    def observe(self, event: ReplayEvent, output: LearnerOutput) -> None:
        self.total_events += 1
        if event.access_seq != output.event.access_seq or event.bop_name != output.event.bop_name:
            self._record({
                "access_seq": event.access_seq,
                "bop_name": event.bop_name,
                "field": "event_identity",
                "online": [event.access_seq, event.bop_name],
                "offline": [output.event.access_seq, output.event.bop_name],
            })
            return
        for field_name in self._FIELDS:
            expected = getattr(event, field_name)
            if field_name == "selected_offset" and expected == 0:
                expected = event.best_offset_after
            actual = getattr(output, field_name)
            if expected != actual:
                self._record({
                    "access_seq": event.access_seq,
                    "bop_name": event.bop_name,
                    "field": field_name,
                    "online": expected,
                    "offline": actual,
                })
                return

    def _record(self, mismatch: dict[str, object]) -> None:
        self.mismatched_events += 1
        if self.first_mismatch is None:
            self.first_mismatch = mismatch

    def report(self) -> dict[str, object]:
        return {
            "total_events": self.total_events,
            "matched_events": self.total_events - self.mismatched_events,
            "mismatched_events": self.mismatched_events,
            "first_mismatch": self.first_mismatch,
            "pass": self.mismatched_events == 0,
        }


def _stream_trace_rows(
    connection: sqlite3.Connection, on_demand: Callable[[Demand], None],
    on_event: Callable[[ReplayEvent], None], on_access_end: Callable[[], None],
) -> tuple[int, int]:
    event_columns = _table_columns(connection, "BOPReplayEvent")
    trigger_is_demand_column = (
        "TriggerIsDemand" if "TriggerIsDemand" in event_columns else "1"
    )
    trigger_is_read_column = (
        "TriggerIsRead" if "TriggerIsRead" in event_columns else "1"
    )
    demand_rows = iter(connection.execute(
        "SELECT AccessSeq,PhaseId,Tick,Addr FROM L2DemandTrace ORDER BY AccessSeq"
    ))
    event_rows = iter(connection.execute(
        "SELECT rowid AS TraceOrder,AccessSeq,BOPName,BOPKind,ReplayOrder,PhaseId,"
        "Tick,TriggerAddr,TriggerPC,TriggerHasPC,ValidationHit,BestOffsetChanged,"
        "IssueEnabled,ValidationEnabled,PCConfidenceEnabled,PCSampled,"
        "RawCandidateValid,RawCandidateAddr,PolicyCandidateValid,"
        "PolicyCandidateAddr,Late,BestOffsetBefore,BestOffsetAfter,BestScore,Round,"
        "TeacherIssueEnabled,StudentIssueEnabled,StudentSelectedValid,"
        "StudentSelectedEnable,StudentSelectedOffset,SelectedOffset,"
        f"{trigger_is_demand_column} AS TriggerIsDemand,"
        f"{trigger_is_read_column} AS TriggerIsRead "
        "FROM BOPReplayEvent ORDER BY AccessSeq,rowid"
    ))
    demand_row = next(demand_rows, None)
    event_row = next(event_rows, None)
    demand_count = 0
    event_count = 0
    while demand_row is not None or event_row is not None:
        demand_seq = int(demand_row["AccessSeq"]) if demand_row is not None else None
        event_seq = int(event_row["AccessSeq"]) if event_row is not None else None
        access_seq = event_seq if demand_seq is None else demand_seq
        if event_seq is not None and event_seq < access_seq:
            access_seq = event_seq
        if demand_row is not None and demand_seq == access_seq:
            on_demand(Demand(
                access_seq, int(demand_row["Tick"]),
                _sqlite_u64(demand_row["Addr"]), int(demand_row["PhaseId"]),
            ))
            demand_count += 1
            demand_row = next(demand_rows, None)
        while event_row is not None and int(event_row["AccessSeq"]) == access_seq:
            on_event(_stream_event_from_row(event_row))
            event_count += 1
            event_row = next(event_rows, None)
        on_access_end()
    return demand_count, event_count


def stream_controller_only_replay(
    connection: sqlite3.Connection, config: BOPConfig, horizons: Sequence[int],
    window: EvaluationWindow,
) -> _StreamingReplayResult:
    """Replay fixed raw BOP candidates using tuple rows and bounded state.

    This path is deliberately narrower than ``_stream_trace_rows``: current
    controller experiments need no learner snapshots or online-output fields.
    Keep demand-before-event ordering and commit exactly once per AccessSeq so
    its state transitions remain identical to the materialized reference.
    """
    event_columns = _table_columns(connection, "BOPReplayEvent")
    best_offset_after_column = (
        "BestOffsetAfter" if "BestOffsetAfter" in event_columns else "0"
    )
    connection.row_factory = None
    policies = {
        horizon: _StreamingPolicyReplay(config, horizon, window)
        for horizon in horizons
    }
    demand_rows = iter(connection.execute(
        "SELECT AccessSeq,PhaseId,Tick,Addr FROM L2DemandTrace ORDER BY AccessSeq"
    ))
    event_rows = iter(connection.execute(
        "SELECT rowid,AccessSeq,BOPKind,PhaseId,Tick,TriggerAddr,TriggerPC,"
        "TriggerHasPC,ValidationHit,BestOffsetChanged,ValidationEnabled,"
        "PCConfidenceEnabled,RawCandidateValid,RawCandidateAddr,"
        f"{best_offset_after_column} AS BestOffsetAfter "
        "FROM BOPReplayEvent ORDER BY AccessSeq,rowid"
    ))
    demand_row = next(demand_rows, None)
    event_row = next(event_rows, None)
    demand_count = 0
    event_count = 0
    window_demands = 0
    window_events = 0

    while demand_row is not None or event_row is not None:
        demand_seq = int(demand_row[0]) if demand_row is not None else None
        event_seq = int(event_row[1]) if event_row is not None else None
        access_seq = event_seq if demand_seq is None else demand_seq
        if event_seq is not None and event_seq < access_seq:
            access_seq = event_seq

        if demand_row is not None and demand_seq == access_seq:
            phase_id = int(demand_row[1])
            tick = int(demand_row[2])
            selected = _in_evaluation_values(phase_id, tick, window)
            if selected:
                window_demands += 1
            demand = Demand(
                access_seq, tick, _sqlite_u64(demand_row[3]), phase_id,
            )
            for replay in policies.values():
                replay.observe_demand(demand)
            demand_count += 1
            demand_row = next(demand_rows, None)

        while event_row is not None and int(event_row[1]) == access_seq:
            phase_id = int(event_row[3])
            tick = int(event_row[4])
            selected = _in_evaluation_values(phase_id, tick, window)
            if selected:
                window_events += 1
            for replay in policies.values():
                replay.emit_values(
                    bop_kind=str(event_row[2]),
                    best_offset=int(event_row[14]),
                    best_offset_changed=bool(event_row[9]),
                    raw_candidate_valid=bool(event_row[12]),
                    raw_candidate_addr=_sqlite_u64(event_row[13]),
                    pc_confidence_enabled=bool(event_row[11]),
                    validation_enabled=bool(event_row[10]),
                    validation_hit=int(event_row[8]),
                    trigger_addr=_sqlite_u64(event_row[5]),
                    trigger_pc=_sqlite_u64(event_row[6]),
                    trigger_has_pc=bool(event_row[7]),
                    access_seq=access_seq,
                    tick=tick,
                    phase_id=phase_id,
                    selected=selected,
                )
            event_count += 1
            event_row = next(event_rows, None)

        for replay in policies.values():
            replay.commit()

    return _StreamingReplayResult(
        {horizon: replay.finish() for horizon, replay in policies.items()},
        demand_count,
        event_count,
        window_demands,
        window_events,
        controller_stats={
            horizon: replay.controller.stats()
            for horizon, replay in policies.items()
        },
    )


def stream_direct_replay(
    connection: sqlite3.Connection, config: BOPConfig, horizons: Sequence[int],
    mode: str, window: EvaluationWindow,
) -> _StreamingReplayResult:
    if mode == "replay-controller":
        if config.pc_validation_producer_consumer:
            return stream_learner_replay(
                connection, config, horizons, "policy", window, False
            )
        return stream_controller_only_replay(connection, config, horizons, window)

    policies = None
    metrics = _StreamingMetricSet(horizons)
    window_demands = 0
    window_events = 0

    def on_demand(demand: Demand) -> None:
        nonlocal window_demands
        if _in_evaluation_window(demand, window):
            window_demands += 1
        metrics.observe_demand(demand, _in_evaluation_window(demand, window))

    def on_event(event: ReplayEvent) -> None:
        nonlocal window_events
        selected = _in_evaluation_window(event, window)
        if selected:
            window_events += 1
        valid, addr = _candidate_from_event(event, mode)
        if valid:
            metrics.emit(
                event.bop_kind, event.access_seq, event.tick, addr,
                event.phase_id, selected,
            )

    def on_access_end() -> None:
        return None

    demands, events = _stream_trace_rows(connection, on_demand, on_event, on_access_end)
    quality_by_horizon = metrics.finish()
    return _StreamingReplayResult(
        quality_by_horizon, demands, events, window_demands, window_events,
    )


def stream_learner_replay(
    connection: sqlite3.Connection, config: BOPConfig, horizons: Sequence[int],
    candidate_stage: str, window: EvaluationWindow, verify_online: bool,
) -> _StreamingReplayResult:
    learner_configs = {
        kind: config.for_kind(kind) for kind in ("large", "small")
    }
    recorded_actions = {
        learner_config.bop_name
        for learner_config in learner_configs.values()
        if _recorded_delay_actions_compatible(learner_config)
    }
    delay_actions = _DelayActionCursors(connection, recorded_actions)
    learners: dict[str, BOPLearner] = {}
    verifier = _OnlineVerifier() if verify_online else None
    policies = (
        {
            horizon: _StreamingPolicyReplay(config, horizon, window)
            for horizon in horizons
        }
        if candidate_stage == "policy"
        else None
    )
    metrics = _StreamingMetricSet(horizons) if candidate_stage == "raw" else None
    window_demands = 0
    window_events = 0

    def on_demand(demand: Demand) -> None:
        nonlocal window_demands
        selected = _in_evaluation_window(demand, window)
        if selected:
            window_demands += 1
        if policies is not None:
            for replay in policies.values():
                replay.observe_demand(demand)
        else:
            assert metrics is not None
            metrics.observe_demand(demand, selected)

    def on_event(event: ReplayEvent) -> None:
        nonlocal window_events
        learner = learners.get(event.bop_kind)
        if learner is None:
            learner_config = learner_configs[event.bop_kind]
            learner = BOPLearner(
                learner_config,
                use_recorded_delay_actions=(
                    learner_config.bop_name in recorded_actions
                ),
            )
            learners[event.bop_kind] = learner
        trigger_action = (
            delay_actions.trigger_action(event, learner)
            if learner.use_recorded_delay_actions else None
        )
        output = learner.process(event, trigger_action)
        if verifier is not None:
            verifier.observe(event, output)
        selected = _in_evaluation_window(event, window)
        if selected:
            window_events += 1
        if policies is not None:
            for replay in policies.values():
                replay.emit_output(output)
        elif output.raw_candidate_valid:
            assert metrics is not None
            metrics.emit(
                output.kind, event.access_seq, event.tick,
                output.raw_candidate_addr, event.phase_id, selected,
            )

    def on_access_end() -> None:
        if policies is not None:
            for replay in policies.values():
                replay.commit()

    demands, events = _stream_trace_rows(connection, on_demand, on_event, on_access_end)
    quality_by_horizon = (
        {horizon: replay.finish() for horizon, replay in policies.items()}
        if policies is not None
        else metrics.finish()
    )
    return _StreamingReplayResult(
        quality_by_horizon, demands, events, window_demands, window_events,
        verifier.report() if verifier is not None else None,
        controller_stats=(
            {horizon: replay.controller.stats()
             for horizon, replay in policies.items()}
            if policies is not None else None
        ),
    )


def _apply_learner_overrides(
    payload: Mapping[str, object], config: BOPConfig
) -> BOPConfig:
    learner_configs = dict(config.learner_configs)
    for kind in ("large", "small"):
        values = payload.get(kind, {})
        if not isinstance(values, Mapping):
            raise ValueError(f"{kind} learner override must be a JSON object")
        target_name = next(
            (
                name
                for name in learner_configs
                if name.lower().endswith(f"bop_{kind}")
                or name.lower().endswith(f"bop{kind}")
            ),
            None,
        )
        if target_name is not None and values:
            allowed = {
                key: value for key, value in values.items()
                if key in BOPConfig.__dataclass_fields__
                and key not in {"schema_version", "bop_name", "learner_configs"}
            }
            if "offsets" in allowed:
                allowed["offsets"] = tuple(int(item) for item in allowed["offsets"])
            learner_configs[target_name] = replace(
                learner_configs[target_name], **allowed
            )
    return replace(config, learner_configs=learner_configs)


def _load_learner_overrides(path: Path, config: BOPConfig) -> BOPConfig:
    return _apply_learner_overrides(json.loads(path.read_text()), config)


def _apply_controller_overrides(
    payload: Mapping[str, object], config: BOPConfig,
) -> BOPConfig:
    """Apply one shared PC/global-controller configuration to both BOPs.

    The online L2 composite prefetcher shares exactly one PC validation table
    between Large and Small BOP.  A controller sweep must therefore never
    produce per-kind values for these fields.  Keep the root configuration in
    sync as well because direct ``replay-controller`` mode uses it directly.
    """
    unknown = sorted(set(payload) - set(PC_SHARED_CONFIG_FIELDS))
    if unknown:
        raise ValueError(
            "unsupported shared controller override(s): " + ", ".join(unknown)
        )
    if not payload:
        return config

    values = {
        field: payload[field]
        for field in PC_SHARED_CONFIG_FIELDS
        if field in payload
    }
    learner_configs = dict(config.learner_configs)
    targets: list[str] = []
    for kind in ("large", "small"):
        target_name = next(
            (
                name
                for name in learner_configs
                if name.lower().endswith(f"bop_{kind}")
                or name.lower().endswith(f"bop{kind}")
            ),
            None,
        )
        if target_name is not None:
            learner_configs[target_name] = replace(
                learner_configs[target_name], **values
            )
            targets.append(target_name)

    if targets and len(targets) != 2:
        raise ValueError(
            "controller overrides require both Large and Small BOP metadata"
        )
    return replace(config, **values, learner_configs=learner_configs)


def _load_controller_overrides(path: Path, config: BOPConfig) -> BOPConfig:
    payload = json.loads(path.read_text())
    if not isinstance(payload, Mapping):
        raise ValueError("controller override must be a JSON object")
    return _apply_controller_overrides(payload, config)


def _parse_horizons(value: str) -> list[int]:
    horizons = [int(item) for item in value.split(",") if item]
    if not horizons or any(horizon <= 0 for horizon in horizons):
        raise argparse.ArgumentTypeError("horizons must be positive comma-separated integers")
    return horizons


def _learner_parameter_report(config: BOPConfig) -> dict[str, dict[str, object]]:
    return {
        kind: {
            field: list(value) if isinstance(value := getattr(config.for_kind(kind), field), tuple)
            else value
            for field in LEARNER_REPORT_FIELDS
        }
        for kind in ("large", "small")
    }


def _controller_parameter_report(config: BOPConfig) -> dict[str, object]:
    return {
        field: getattr(config.for_kind("large"), field)
        for field in CONTROLLER_REPORT_FIELDS
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("database", type=Path, help="SQLite DB from --dump-bop-replay-trace")
    parser.add_argument(
        "--mode",
        choices=("recorded", "raw", "replay-controller", "learner-replay"),
        default="replay-controller",
        help="recorded policy, ungated raw BOP, or replayed PC/global controller",
    )
    parser.add_argument(
        "--learner-config", type=Path, default=None,
        help="JSON overrides for offline large/small learner parameters",
    )
    parser.add_argument(
        "--controller-config", type=Path, default=None,
        help="JSON overrides for shared PC-validation/global-controller parameters",
    )
    parser.add_argument(
        "--candidate-stage",
        choices=("raw", "policy"),
        default="raw",
        help="raw learner candidates or learner plus filter-free BOP policy replay",
    )
    parser.add_argument(
        "--replay-engine",
        choices=("materialized", "streaming"),
        default="streaming",
        help="trace execution engine; V1-V4 traces automatically use materialized",
    )
    parser.add_argument(
        "--verify-online", action="store_true",
        help="compare learner-replay state and raw candidates with online golden fields",
    )
    parser.add_argument(
        "--horizons",
        type=_parse_horizons,
        default=[128, 512, 2048, 8192],
        help="future demand-read horizons, for example 128,512,2048,8192",
    )
    parser.add_argument(
        "--evaluation-phase",
        default="full",
        help="V5 trace phase to report, for example stable; default: full",
    )
    parser.add_argument(
        "--evaluation-start-tick",
        type=int,
        default=None,
        help="report candidates/demands at or after this tick",
    )
    parser.add_argument(
        "--evaluation-start-stats",
        type=Path,
        default=None,
        help="derive the reporting start tick from a gem5 stats.txt block",
    )
    parser.add_argument(
        "--evaluation-stats-block",
        type=int,
        default=1,
        help="1-based stats block used with --evaluation-start-stats",
    )
    parser.add_argument(
        "--global-unused-threshold",
        type=int,
        default=None,
        help="override the metadata global unused EWMA threshold",
    )
    parser.add_argument(
        "--global-min-resolved-coverage-shift",
        type=int,
        default=None,
        help="override the metadata resolved-coverage gate shift",
    )
    parser.add_argument("--output", type=Path, default=None, help="write JSON summary to this path")
    args = parser.parse_args()

    with sqlite3.connect(args.database) as schema_connection:
        schema_row = schema_connection.execute(
            "SELECT SchemaVersion FROM BOPReplayMeta LIMIT 1"
        ).fetchone()
    if schema_row is None:
        raise ValueError("BOPReplayMeta is empty; enable --dump-bop-replay-trace")
    replay_engine = args.replay_engine
    if replay_engine == "streaming" and int(schema_row[0]) != SCHEMA_VERSION:
        replay_engine = "materialized"

    if replay_engine == "streaming":
        start_time = time.perf_counter()
        with sqlite3.connect(args.database) as connection:
            config, phases = _streaming_metadata(connection)
            window = resolve_evaluation_window(
                phase_name=args.evaluation_phase,
                start_tick=args.evaluation_start_tick,
                stats_path=args.evaluation_start_stats,
                stats_block=args.evaluation_stats_block,
                phases=phases,
            )
            controller_overrides = {}
            if args.global_unused_threshold is not None:
                controller_overrides["global_bop_unused_threshold"] = (
                    args.global_unused_threshold
                )
            if args.global_min_resolved_coverage_shift is not None:
                controller_overrides["global_bop_min_resolved_coverage_shift"] = (
                    args.global_min_resolved_coverage_shift
                )
            if controller_overrides:
                config = _apply_controller_overrides(controller_overrides, config)
            if args.controller_config is not None:
                config = _load_controller_overrides(args.controller_config, config)
            if args.learner_config is not None:
                config = _load_learner_overrides(args.learner_config, config)

            if args.mode == "learner-replay":
                result = stream_learner_replay(
                    connection, config, args.horizons, args.candidate_stage,
                    window, args.verify_online,
                )
            else:
                result = stream_direct_replay(
                    connection, config, args.horizons, args.mode, window,
                )

        execution = {
            "engine": "streaming",
            "wall_seconds": time.perf_counter() - start_time,
            "peak_rss_kib": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,
        }
        evaluation_report = {
            "name": window.name,
            "source": window.source,
            "phase_id": window.phase_id,
            "phase_name": window.phase_name,
            "start_tick": window.start_tick,
            "stats_path": window.stats_path,
            "stats_block": window.stats_block,
            "demands": result.window_demands,
            "events": result.window_events,
            "state_replay": "full_trace",
            "demand_oracle": "selected_window_only",
        }
        horizons = {
            str(horizon): {
                name: asdict(metrics)
                for name, metrics in quality.items()
            }
            for horizon, quality in result.quality_by_horizon.items()
        }
        report = {
            "schema_version": config.schema_version,
            "database": str(args.database),
            "mode": args.mode,
            "candidate_stage": args.candidate_stage if args.mode == "learner-replay" else None,
            "replay_engine": execution,
            "demands": result.demands,
            "events": result.events,
            "evaluation_window": evaluation_report,
            "horizons": horizons,
        }
        if (args.mode == "replay-controller"
                and config.pc_validation_producer_consumer):
            report["policy_reconstruction"] = "learner_replay"
            report["learner_parameters"] = _learner_parameter_report(config)
            report["controller_parameters"] = _controller_parameter_report(
                config
            )
        if result.controller_stats is not None:
            report["controller_stats"] = {
                str(horizon): dict(stats)
                for horizon, stats in result.controller_stats.items()
            }
        if args.mode == "learner-replay":
            report.update({
                "learner_config_file": (
                    str(args.learner_config)
                    if args.learner_config is not None else None
                ),
                "learner_parameters": _learner_parameter_report(config),
                "controller_parameters": _controller_parameter_report(config),
                "verification": result.verification,
            })
        encoded = json.dumps(report, indent=2, sort_keys=True)
        if args.output is None:
            print(encoded)
        else:
            args.output.write_text(encoded + "\n")
        return (
            0 if result.verification is None or result.verification["pass"] else 2
        )

    demands, events, config, phases = load_trace_with_phases(args.database)
    window = resolve_evaluation_window(
        phase_name=args.evaluation_phase,
        start_tick=args.evaluation_start_tick,
        stats_path=args.evaluation_start_stats,
        stats_block=args.evaluation_stats_block,
        phases=phases,
    )
    controller_overrides = {}
    if args.global_unused_threshold is not None:
        controller_overrides["global_bop_unused_threshold"] = (
            args.global_unused_threshold
        )
    if args.global_min_resolved_coverage_shift is not None:
        controller_overrides["global_bop_min_resolved_coverage_shift"] = (
            args.global_min_resolved_coverage_shift
        )
    if controller_overrides:
        config = _apply_controller_overrides(controller_overrides, config)

    if args.controller_config is not None:
        config = _load_controller_overrides(args.controller_config, config)

    if args.learner_config is not None:
        config = _load_learner_overrides(args.learner_config, config)

    verification = None
    if args.mode == "learner-replay":
        if config.schema_version < LEARNER_REPLAY_MIN_SCHEMA_VERSION:
            raise ValueError(
                "learner-replay requires schema V3 or newer with ClockPeriodTicks; "
                "rerun GEM5 with --dump-bop-replay-trace"
            )
        if (args.verify_online and
                config.schema_version < LEARNER_REPLAY_CERTIFICATION_MIN_SCHEMA_VERSION):
            raise ValueError(
                "strict --verify-online requires schema V5 with native "
                "BOPReplayDelayAction ordering; rerun GEM5 with "
                "--dump-bop-replay-trace"
            )
        outputs = replay_learner(events, config)
        quality_by_horizon = {}
        for horizon in args.horizons:
            metrics = learner_quality_from_outputs(
                demands, outputs, config, horizon, args.candidate_stage, window
            )
            quality_by_horizon[str(horizon)] = {
                name: asdict(value) for name, value in metrics.items()
            }
        if args.verify_online:
            verification = compare_online_learner(events, outputs)
        report = {
            "schema_version": config.schema_version,
            "database": str(args.database),
            "mode": args.mode,
            "candidate_stage": args.candidate_stage,
            "learner_config_file": (
                str(args.learner_config) if args.learner_config is not None else None
            ),
            "learner_parameters": _learner_parameter_report(config),
            "controller_parameters": _controller_parameter_report(config),
            "demands": len(demands),
            "events": len(events),
            "evaluation_window": evaluation_window_report(window, demands, events),
            "verification": verification,
            "horizons": quality_by_horizon,
        }
        encoded = json.dumps(report, indent=2, sort_keys=True)
        if args.output is None:
            print(encoded)
        else:
            args.output.write_text(encoded + "\n")
        return 0 if verification is None or verification["pass"] else 2

    if (args.mode == "replay-controller"
            and config.pc_validation_producer_consumer):
        # Event-only controller inputs have no RR producer owner. Rebuild it
        # from the native learner and delay-queue state as streaming does.
        outputs = replay_learner(events, config)
        report = {
            "schema_version": config.schema_version,
            "database": str(args.database),
            "mode": args.mode,
            "policy_reconstruction": "learner_replay",
            "learner_parameters": _learner_parameter_report(config),
            "controller_parameters": _controller_parameter_report(config),
            "demands": len(demands),
            "events": len(events),
            "evaluation_window": evaluation_window_report(window, demands, events),
            "horizons": {
                str(horizon): {
                    name: asdict(metrics)
                    for name, metrics in learner_quality_from_outputs(
                        demands, outputs, config, horizon, "policy", window
                    ).items()
                }
                for horizon in args.horizons
            },
        }
        encoded = json.dumps(report, indent=2, sort_keys=True)
        if args.output is None:
            print(encoded)
        else:
            args.output.write_text(encoded + "\n")
        return 0

    report = {
        "schema_version": config.schema_version,
        "database": str(args.database),
        "mode": args.mode,
        "demands": len(demands),
        "events": len(events),
        "evaluation_window": evaluation_window_report(window, demands, events),
        "horizons": {
            str(horizon): {
                name: asdict(metrics)
                for name, metrics in evaluate_window(
                    demands,
                    replay_controller(demands, events, config, horizon)
                    if args.mode == "replay-controller"
                    else _event_candidates(events, args.mode),
                    horizon,
                    window,
                ).items()
            }
            for horizon in args.horizons
        },
    }
    encoded = json.dumps(report, indent=2, sort_keys=True)
    if args.output is None:
        print(encoded)
    else:
        args.output.write_text(encoded + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
