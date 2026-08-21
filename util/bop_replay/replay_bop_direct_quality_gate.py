#!/usr/bin/env python3
"""Causal sampled direct-quality admission for recorded raw BOP candidates.

The existing PC-validation controller treats predecessor RR evidence as a
proxy for candidate value.  This offline-only experiment instead learns from
the future L2 demand outcome of a bounded deterministic sample of candidates
that were actually issued earlier in the same replay.  Admission never reads
an unresolved or future label.

The design deliberately has two fixed-size structures:

* a set-associative ``PC x BOP-kind`` table holding useful/unused evidence and
  the state ``OBSERVE``, ``OPEN``, ``OPEN_AUDIT``, or ``BLOCK``;
* a set-associative sampled-feedback table holding candidate-line ownership
  until L2 demand use or a Horizon-based expiry.

The quality table blocks only after sufficient direct evidence proves
``unused >= unused_per_useful * useful + guard``.  Feedback-table conflicts
are dropped labels, never negative samples.  This models a practical bounded
controller, not the non-causal complete-window PC oracle.

The trace is replayed in original access order.  Only candidates in the
selected reporting window participate in quality metrics and direct feedback;
warmup always remains state-only so warmup-issued samples cannot acquire a
label from a stable demand.
"""

from __future__ import annotations

import argparse
import heapq
import json
import sqlite3
import time
from collections import Counter, defaultdict, deque
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Callable, Iterable, Mapping

import bop_replay as replay


HORIZON = 2048
STATES = ("observe", "open", "audit", "block", "recover")
OUTCOMES = ("useful", "unused", "redundant", "censored")


@dataclass(frozen=True)
class DirectQualityConfig:
    """Offline parameters for the sampled direct-quality controller."""

    quality_entries: int = 128
    quality_ways: int = 4
    quality_tag_bits: int = 10
    # One slot preserves the original PC x BOP-kind quality state. Larger
    # values split that root state by recent native BOP best offsets.
    offset_context_slots: int = 1
    feedback_entries: int = 256
    feedback_ways: int = 4
    feedback_tag_bits: int = 24
    horizon: int = HORIZON
    observe_sample_period: int = 16
    # False retains the initial probe-only baseline. True keeps OBSERVE
    # fail-open while sampling it at observe_sample_period for training.
    observe_issue_all: bool = False
    open_sample_period: int = 16
    # A strict BLOCK retains the original sparse recovery rate. A BLOCK which
    # has failed the main 10:1 test but not this stricter test may probe at a
    # separate rate. Defaults collapse both classes to the legacy behavior.
    block_probe_period: int = 64
    borderline_block_probe_period: int = 64
    min_samples: int = 64
    unused_per_useful: int = 10
    block_guard: int = 4
    strict_unused_per_useful: int = 10
    strict_block_guard: int = 4
    reopen_unused_per_useful: int = 8
    reopen_guard: int = 4
    # A positive probe can otherwise move BLOCK directly to unthrottled OPEN.
    # Keep the default at zero so existing profiles retain that behavior.
    reopen_confirm_samples: int = 0
    reopen_probe_period: int = 64
    # When enabled, a context that would normally reopen enters a fail-open
    # audit episode first. It issues every raw candidate but samples one in
    # audit_sample_period candidates until audit_samples direct labels from
    # that exact episode arrive. Defaults disable the state entirely.
    audit_samples: int = 0
    audit_sample_period: int = 4
    audit_unused_per_useful: int = 20
    audit_block_guard: int = 4
    # "line" retains address-hash sampling. "global_counter" uses one shared
    # bounded decision counter to avoid a spatially correlated sample subset.
    sample_source: str = "line"
    sample_counter_bits: int = 16
    decay_period: int = 64

    def __post_init__(self) -> None:
        if self.quality_entries <= 0 or not _is_power_of_two(self.quality_entries):
            raise ValueError("quality_entries must be a positive power of two")
        if self.quality_ways <= 0 or self.quality_entries % self.quality_ways:
            raise ValueError("quality_entries must be divisible by quality_ways")
        if self.quality_ways not in (1, 2, 4):
            raise ValueError("quality_ways must be one, two, or four")
        if self.offset_context_slots not in (1, 2, 4):
            raise ValueError("offset_context_slots must be one, two, or four")
        if self.feedback_entries <= 0 or not _is_power_of_two(self.feedback_entries):
            raise ValueError("feedback_entries must be a positive power of two")
        if self.feedback_ways <= 0 or self.feedback_entries % self.feedback_ways:
            raise ValueError("feedback_entries must be divisible by feedback_ways")
        if self.horizon <= 0:
            raise ValueError("horizon must be positive")
        for name, period in (
            ("observe_sample_period", self.observe_sample_period),
            ("open_sample_period", self.open_sample_period),
            ("block_probe_period", self.block_probe_period),
            ("borderline_block_probe_period", self.borderline_block_probe_period),
            ("reopen_probe_period", self.reopen_probe_period),
            ("audit_sample_period", self.audit_sample_period),
        ):
            if not _is_power_of_two(period):
                raise ValueError(f"{name} must be a positive power of two")
        if self.min_samples <= 0:
            raise ValueError("min_samples must be positive")
        if self.unused_per_useful <= 0:
            raise ValueError("unused_per_useful must be positive")
        if self.strict_unused_per_useful < self.unused_per_useful:
            raise ValueError(
                "strict_unused_per_useful must be at least unused_per_useful"
            )
        if self.block_guard < 0 or self.strict_block_guard < 0:
            raise ValueError("block guards must be non-negative")
        if self.reopen_unused_per_useful < 0:
            raise ValueError("reopen_unused_per_useful must be non-negative")
        if self.reopen_confirm_samples < 0:
            raise ValueError("reopen_confirm_samples must be non-negative")
        if self.audit_samples < 0:
            raise ValueError("audit_samples must be non-negative")
        if self.audit_unused_per_useful <= 0:
            raise ValueError("audit_unused_per_useful must be positive")
        if self.audit_block_guard < 0:
            raise ValueError("audit_block_guard must be non-negative")
        if self.sample_source not in ("line", "global_counter"):
            raise ValueError("sample_source must be 'line' or 'global_counter'")
        if not 1 <= self.sample_counter_bits <= 64:
            raise ValueError("sample_counter_bits must be in [1, 64]")
        if self.decay_period < 0:
            raise ValueError("decay_period must be non-negative")
        if self.decay_period and not _is_power_of_two(self.decay_period):
            raise ValueError("decay_period must be zero or a power of two")

    @property
    def quality_sets(self) -> int:
        return self.quality_entries // self.quality_ways

    @property
    def feedback_sets(self) -> int:
        return self.feedback_entries // self.feedback_ways


@dataclass
class _QualityEntry:
    valid: bool = False
    tag: int = 0
    generation: int = 0


@dataclass
class _QualityOffsetContext:
    valid: bool = False
    offset: int = 0
    useful: int = 0
    unused: int = 0
    state: str = "observe"
    trained: bool = False
    resolved_since_decay: int = 0
    recovery_samples: int = 0
    recovery_generation: int = 0
    audit_samples: int = 0
    audit_useful: int = 0
    audit_unused: int = 0
    audit_generation: int = 0
    generation: int = 0


@dataclass
class _FeedbackEntry:
    valid: bool = False
    line: int = 0
    tag: int = 0
    quality_index: int = 0
    quality_generation: int = 0
    offset_context_index: int = 0
    offset_context_generation: int = 0
    recovery_generation: int = 0
    audit_generation: int = 0
    demand_index_at_issue: int = 0
    candidate_id: int = 0
    last_touch: int = 0


@dataclass(frozen=True)
class _QualityLookup:
    index: int
    context_index: int
    offset: int
    state: str
    useful: int
    unused: int
    generation: int
    context_generation: int
    recovery_generation: int
    audit_generation: int


@dataclass(frozen=True)
class _IssuedCandidate:
    candidate_id: int
    kind: str
    access_seq: int
    tick: int
    addr: int
    demand_index_at_issue: int
    phase_id: int
    context: tuple[int, str] | None = None


@dataclass
class _ContextCounts:
    raw_candidates: int = 0
    issued_candidates: int = 0
    suppressed_candidates: int = 0
    raw_useful: int = 0
    raw_unused: int = 0
    raw_redundant: int = 0
    raw_censored: int = 0
    useful: int = 0
    unused: int = 0
    redundant: int = 0
    censored: int = 0
    samples_inserted: int = 0
    samples_useful: int = 0
    samples_unused: int = 0
    samples_redundant: int = 0
    samples_dropped: int = 0
    samples_censored: int = 0
    state_observe: int = 0
    state_open: int = 0
    state_audit: int = 0
    state_block: int = 0
    state_recover: int = 0
    audit_entries: int = 0
    audit_passes: int = 0
    audit_failures: int = 0

    def resolve_quality(self, status: str) -> None:
        if status not in OUTCOMES:
            raise ValueError(f"invalid candidate outcome {status!r}")
        setattr(self, status, getattr(self, status) + 1)

    def resolve_raw_quality(self, status: str) -> None:
        if status not in OUTCOMES:
            raise ValueError(f"invalid candidate outcome {status!r}")
        field = f"raw_{status}"
        setattr(self, field, getattr(self, field) + 1)


def _is_power_of_two(value: int) -> bool:
    return value > 0 and (value & (value - 1)) == 0


def _kind_value(kind: str) -> int:
    if kind == "large":
        return 1
    if kind == "small":
        return 2
    raise ValueError(f"unsupported BOP kind {kind!r}")


def _mix64(value: int) -> int:
    value &= replay.UINT64_MASK
    value ^= value >> 30
    value = (value * 0xBF58476D1CE4E5B9) & replay.UINT64_MASK
    value ^= value >> 27
    value = (value * 0x94D049BB133111EB) & replay.UINT64_MASK
    value ^= value >> 31
    return value & replay.UINT64_MASK


class DirectQualityController:
    """Bounded PC-kind quality roots with optional recent-offset contexts."""

    def __init__(self, config: DirectQualityConfig):
        self.config = config
        self.entries = [_QualityEntry() for _ in range(config.quality_entries)]
        self.offset_contexts = [
            [_QualityOffsetContext() for _ in range(config.offset_context_slots)]
            for _ in range(config.quality_entries)
        ]
        # Match BOP's existing four-way PC table: one three-bit tree-PLRU
        # state per set protects recently used hot roots without adding a
        # per-entry recency counter to the hardware-facing model.
        self.plru_state = [0] * config.quality_sets
        self.offset_plru_state = [0] * config.quality_entries
        self.sample_counter = 0
        self.sample_counter_mask = (1 << config.sample_counter_bits) - 1
        self.stats: Counter[str] = Counter()

    def _signature(self, pc: int, kind: str) -> int:
        return _mix64((pc >> 1) ^ (_kind_value(kind) * 0x9E3779B97F4A7C15))

    def _key(self, pc: int, kind: str) -> tuple[int, int]:
        signature = self._signature(pc, kind)
        set_bits = self.config.quality_sets.bit_length() - 1
        return (
            signature & (self.config.quality_sets - 1),
            (signature >> set_bits) & ((1 << self.config.quality_tag_bits) - 1),
        )

    def _plru_victim(self, set_index: int) -> int:
        if self.config.quality_ways == 1:
            return 0
        if self.config.quality_ways == 2:
            return self.plru_state[set_index] & 0x1
        state = self.plru_state[set_index] & 0x7
        if (state & 0x1) == 0:
            return 0 if (state & 0x2) == 0 else 1
        return 2 if (state & 0x4) == 0 else 3

    def _touch_plru(self, set_index: int, way: int) -> None:
        if self.config.quality_ways == 1:
            if way != 0:
                raise ValueError(f"invalid direct-quality way {way}")
            return
        if self.config.quality_ways == 2:
            if way not in (0, 1):
                raise ValueError(f"invalid direct-quality way {way}")
            self.plru_state[set_index] = 1 if way == 0 else 0
            return
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
            raise ValueError(f"invalid four-way PLRU index {way}")
        self.plru_state[set_index] = state

    def _offset_context_victim(self, entry_index: int) -> int:
        if self.config.offset_context_slots == 1:
            return 0
        if self.config.offset_context_slots == 2:
            return self.offset_plru_state[entry_index] & 0x1
        state = self.offset_plru_state[entry_index] & 0x7
        if (state & 0x1) == 0:
            return 0 if (state & 0x2) == 0 else 1
        return 2 if (state & 0x4) == 0 else 3

    def _touch_offset_context(self, entry_index: int, context_index: int) -> None:
        if self.config.offset_context_slots == 1:
            if context_index != 0:
                raise ValueError(f"invalid direct-quality offset context {context_index}")
            return
        if self.config.offset_context_slots == 2:
            if context_index not in (0, 1):
                raise ValueError(f"invalid direct-quality offset context {context_index}")
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
            raise ValueError(f"invalid direct-quality offset context {context_index}")
        self.offset_plru_state[entry_index] = state

    def _reset_root(self, index: int, tag: int) -> None:
        entry = self.entries[index]
        entry.valid = True
        entry.tag = tag
        entry.generation += 1
        self.offset_contexts[index] = [
            _QualityOffsetContext()
            for _ in range(self.config.offset_context_slots)
        ]
        self.offset_plru_state[index] = 0

    @staticmethod
    def _reset_offset_context(context: _QualityOffsetContext, offset: int) -> None:
        context.valid = True
        context.offset = offset
        context.useful = 0
        context.unused = 0
        context.state = "observe"
        context.trained = False
        context.resolved_since_decay = 0
        context.recovery_samples = 0
        context.recovery_generation = 0
        context.audit_samples = 0
        context.audit_useful = 0
        context.audit_unused = 0
        context.audit_generation = 0
        context.generation += 1

    def _lookup_offset_context(
        self, entry_index: int, best_offset: int,
    ) -> tuple[int, _QualityOffsetContext]:
        contexts = self.offset_contexts[entry_index]
        if self.config.offset_context_slots == 1:
            context = contexts[0]
            if not context.valid:
                self._reset_offset_context(context, best_offset)
            return 0, context

        context_index = next(
            (
                candidate
                for candidate, context in enumerate(contexts)
                if context.valid and context.offset == best_offset
            ),
            None,
        )
        if context_index is not None:
            self.stats["offset_context_hits"] += 1
            self._touch_offset_context(entry_index, context_index)
            return context_index, contexts[context_index]

        self.stats["offset_context_misses"] += 1
        context_index = next(
            (
                candidate
                for candidate, context in enumerate(contexts)
                if not context.valid
            ),
            None,
        )
        if context_index is None:
            context_index = self._offset_context_victim(entry_index)
            self.stats["offset_context_replacements"] += 1
        context = contexts[context_index]
        self._reset_offset_context(context, best_offset)
        self._touch_offset_context(entry_index, context_index)
        return context_index, context

    def lookup(
        self, pc: int, kind: str, best_offset: int = 0,
    ) -> _QualityLookup:
        set_index, tag = self._key(pc, kind)
        base = set_index * self.config.quality_ways
        self.stats["quality_lookups"] += 1
        for way in range(self.config.quality_ways):
            index = base + way
            entry = self.entries[index]
            if entry.valid and entry.tag == tag:
                self.stats["quality_hits"] += 1
                self._touch_plru(set_index, way)
                context_index, context = self._lookup_offset_context(
                    index, best_offset,
                )
                return _QualityLookup(
                    index, context_index, best_offset, context.state,
                    context.useful, context.unused, entry.generation,
                    context.generation, context.recovery_generation,
                    context.audit_generation,
                )

        self.stats["quality_misses"] += 1
        victim = next(
            (base + way for way in range(self.config.quality_ways)
             if not self.entries[base + way].valid),
            None,
        )
        if victim is None:
            victim_way = self._plru_victim(set_index)
            victim = base + victim_way
            self.stats["quality_replacements"] += 1
        self._reset_root(victim, tag)
        entry = self.entries[victim]
        self._touch_plru(set_index, victim - base)
        context_index, context = self._lookup_offset_context(victim, best_offset)
        return _QualityLookup(
            victim, context_index, best_offset, context.state, context.useful,
            context.unused, entry.generation, context.generation,
            context.recovery_generation, context.audit_generation,
        )

    def should_issue(
        self, lookup: _QualityLookup, pc: int, kind: str, line: int,
    ) -> tuple[bool, bool]:
        """Return ``issued, sampled`` using only current causal state."""
        state = lookup.state
        sample_counter = self.sample_counter
        self.sample_counter = (self.sample_counter + 1) & self.sample_counter_mask
        self.stats[f"admission_checks:{state}"] += 1
        if state == "open":
            period = self.config.open_sample_period
            issued = True
            sampled = self._sample(
                pc, kind, line, period, 0x5A6D, lookup.offset, sample_counter,
            )
        elif state == "audit":
            issued = True
            sampled = self._sample(
                pc, kind, line, self.config.audit_sample_period, 0xA0D1,
                lookup.offset, sample_counter,
            )
            self.stats["audit_checks"] += 1
            if sampled:
                self.stats["audit_sampled_candidates"] += 1
            else:
                self.stats["audit_unsampled_issued"] += 1
        elif state == "recover":
            period = self.config.reopen_probe_period
            issued = self._sample(
                pc, kind, line, period, 0x5EC0, lookup.offset, sample_counter,
            )
            sampled = issued
            self.stats["recovery_checks"] += 1
            if issued:
                self.stats["recovery_probes"] += 1
            else:
                self.stats["recovery_suppressions"] += 1
        elif state == "block":
            block_class = self._block_class(lookup)
            period = (
                self.config.block_probe_period
                if block_class == "strict"
                else self.config.borderline_block_probe_period
            )
            issued = self._sample(
                pc, kind, line, period, 0xB10C, lookup.offset, sample_counter,
            )
            sampled = issued
            self.stats[f"block_checks:{block_class}"] += 1
            if issued:
                self.stats["block_recovery_probes"] += 1
                self.stats[f"block_recovery_probes:{block_class}"] += 1
            else:
                self.stats["block_suppressions"] += 1
                self.stats[f"block_suppressions:{block_class}"] += 1
        else:
            period = self.config.observe_sample_period
            sampled = self._sample(
                pc, kind, line, period, 0x0B5E, lookup.offset, sample_counter,
            )
            issued = self.config.observe_issue_all or sampled
            if self.config.observe_issue_all:
                self.stats["observe_fail_open_issued"] += 1
                if sampled:
                    self.stats["observe_fail_open_samples"] += 1
            elif issued:
                self.stats["observe_probes"] += 1
            else:
                self.stats["observe_suppressions"] += 1
        if issued and not sampled:
            # OPEN/AUDIT traffic is sampled independently; OBSERVE/BLOCK
            # traffic is already probe-limited and always yields feedback.
            self.stats["issued_not_sampled"] += 1
        if issued:
            self.stats[f"issued:{state}"] += 1
        return issued, sampled

    def _block_class(self, lookup: _QualityLookup) -> str:
        """Classify an already-BLOCK context using current causal evidence."""
        strict_limit = (
            self.config.strict_unused_per_useful * lookup.useful
            + self.config.strict_block_guard
        )
        return "strict" if lookup.unused >= strict_limit else "borderline"

    def _sample(
        self, pc: int, kind: str, line: int, period: int, salt: int,
        best_offset: int, sample_counter: int,
    ) -> bool:
        signature = self._signature(pc, kind)
        if self.config.sample_source == "line":
            signature ^= line & replay.UINT64_MASK
        else:
            signature ^= (
                sample_counter * 0x9E3779B97F4A7C15
            ) & replay.UINT64_MASK
        signature ^= salt
        if self.config.offset_context_slots > 1:
            signature ^= (best_offset & replay.UINT64_MASK) * 0x9E3779B97F4A7C15
        return (_mix64(signature) & (period - 1)) == 0

    def note_sample(
        self, quality_index: int, quality_generation: int, status: str,
        offset_context_index: int = 0,
        offset_context_generation: int | None = None,
        recovery_generation: int | None = None,
        audit_generation: int | None = None,
    ) -> tuple[str, str]:
        """Apply one resolved direct candidate sample and return transition."""
        if status not in ("useful", "unused"):
            raise ValueError("only useful/unused samples train direct quality")
        entry = self.entries[quality_index]
        if not entry.valid or entry.generation != quality_generation:
            self.stats["orphaned_feedback_labels"] += 1
            return "observe", "observe"
        if not 0 <= offset_context_index < self.config.offset_context_slots:
            self.stats["orphaned_feedback_labels"] += 1
            return "observe", "observe"
        context = self.offset_contexts[quality_index][offset_context_index]
        if (not context.valid or (offset_context_generation is not None
                                  and context.generation != offset_context_generation)):
            self.stats["orphaned_feedback_labels"] += 1
            return "observe", "observe"
        before = context.state
        current_audit_sample = (
            before == "audit"
            and audit_generation == context.audit_generation
        )
        if status == "useful":
            context.useful += 1
        else:
            context.unused += 1
        if (before == "recover"
                and recovery_generation == context.recovery_generation):
            context.recovery_samples += 1
        if current_audit_sample:
            context.audit_samples += 1
            if status == "useful":
                context.audit_useful += 1
            else:
                context.audit_unused += 1
            self.stats[f"audit_samples:{status}"] += 1
        context.resolved_since_decay += 1
        self.stats[f"samples:{status}"] += 1
        self._update_state(context)
        if (self.config.decay_period
                and context.resolved_since_decay >= self.config.decay_period):
            context.useful >>= 1
            context.unused >>= 1
            context.resolved_since_decay = 0
            self.stats["quality_decays"] += 1
            self._update_state(context)
        after = context.state
        if before != after:
            self.stats[f"state_transition:{before}_to_{after}"] += 1
        return before, after

    def _update_state(self, context: _QualityOffsetContext) -> None:
        samples = context.useful + context.unused
        if not context.trained and samples < self.config.min_samples:
            context.state = "observe"
            return
        if samples >= self.config.min_samples:
            context.trained = True
        if not context.trained:
            return
        if context.state == "block":
            reopen_limit = (
                self.config.reopen_unused_per_useful * context.useful
                - self.config.reopen_guard
            )
            if context.unused <= reopen_limit:
                if self.config.reopen_confirm_samples:
                    context.state = "recover"
                    context.recovery_samples = 0
                    context.recovery_generation += 1
                else:
                    self._enter_open_or_audit(context)
            return
        if context.state == "recover":
            block_limit = (
                self.config.unused_per_useful * context.useful
                + self.config.block_guard
            )
            if context.unused >= block_limit:
                context.state = "block"
                context.recovery_samples = 0
                return
            reopen_limit = (
                self.config.reopen_unused_per_useful * context.useful
                - self.config.reopen_guard
            )
            if (context.recovery_samples >= self.config.reopen_confirm_samples
                    and context.unused <= reopen_limit):
                self._enter_open_or_audit(context)
            return
        if context.state == "audit":
            if context.audit_samples < self.config.audit_samples:
                return
            if self._audit_blocks(context):
                context.state = "block"
                self.stats["audit_failures"] += 1
            else:
                context.state = "open"
                self.stats["audit_passes"] += 1
            return
        block_limit = (
            self.config.unused_per_useful * context.useful
            + self.config.block_guard
        )
        context.state = "block" if context.unused >= block_limit else "open"

    def _enter_open_or_audit(self, context: _QualityOffsetContext) -> None:
        if not self.config.audit_samples:
            context.state = "open"
            return
        context.state = "audit"
        context.audit_samples = 0
        context.audit_useful = 0
        context.audit_unused = 0
        context.audit_generation += 1
        self.stats["audit_entries"] += 1

    def _audit_blocks(self, context: _QualityOffsetContext) -> bool:
        return context.audit_unused >= (
            self.config.audit_unused_per_useful * context.audit_useful
            + self.config.audit_block_guard
        )

    def context_snapshot(self, pc: int, kind: str) -> list[dict[str, object]]:
        """Read final context state without allocating a report-time entry."""
        set_index, tag = self._key(pc, kind)
        base = set_index * self.config.quality_ways
        for way in range(self.config.quality_ways):
            index = base + way
            entry = self.entries[index]
            if entry.valid and entry.tag == tag:
                snapshots = [
                    {
                        "offset": context.offset,
                        "state": context.state,
                        "block_class": (
                            self._block_class(_QualityLookup(
                                index, 0, context.offset, context.state,
                                context.useful, context.unused,
                                entry.generation, context.generation,
                                context.recovery_generation,
                                context.audit_generation,
                            )) if context.state == "block" else None
                        ),
                        "sampled_useful": context.useful,
                        "sampled_unused": context.unused,
                        "trained": context.trained,
                    }
                    for context in self.offset_contexts[index]
                    if context.valid
                ]
                if self.config.reopen_confirm_samples:
                    for context, snapshot in zip(
                        (context for context in self.offset_contexts[index]
                         if context.valid),
                        snapshots,
                    ):
                        snapshot["recovery_generation"] = (
                            context.recovery_generation
                        )
                        snapshot["recovery_samples"] = context.recovery_samples
                if self.config.audit_samples:
                    for context, snapshot in zip(
                        (context for context in self.offset_contexts[index]
                         if context.valid),
                        snapshots,
                    ):
                        snapshot["audit_generation"] = context.audit_generation
                        snapshot["audit_samples"] = context.audit_samples
                        snapshot["audit_useful"] = context.audit_useful
                        snapshot["audit_unused"] = context.audit_unused
                return snapshots
        return []

    def report(self) -> dict[str, int]:
        return {key: int(value) for key, value in sorted(self.stats.items())}


class SampledFeedbackTable:
    """Bounded candidate-line ownership table for direct sample labels."""

    def __init__(
        self, config: DirectQualityConfig,
        on_resolve: Callable[[_FeedbackEntry, str], None],
        on_drop: Callable[[_FeedbackEntry], None],
    ):
        self.config = config
        self.on_resolve = on_resolve
        self.on_drop = on_drop
        self.entries = [_FeedbackEntry() for _ in range(config.feedback_entries)]
        self.set_next_victim = [0] * config.feedback_sets
        self.by_line: dict[int, deque[int]] = defaultdict(deque)
        self.expiry_heap: list[tuple[int, int, int]] = []
        self.clock = 0
        self.stats: Counter[str] = Counter()

    def _line(self, addr: int) -> int:
        return addr & ~(64 - 1)

    def _key(self, line: int) -> tuple[int, int]:
        signature = _mix64(line >> 6)
        set_bits = self.config.feedback_sets.bit_length() - 1
        return (
            signature & (self.config.feedback_sets - 1),
            (signature >> set_bits) & ((1 << self.config.feedback_tag_bits) - 1),
        )

    def _remove_line_index(self, line: int, index: int) -> None:
        pending = self.by_line.get(line)
        if pending is None:
            return
        try:
            pending.remove(index)
        except ValueError:
            return
        if not pending:
            self.by_line.pop(line, None)

    def _invalidate(self, index: int, reason: str, status: str | None = None) -> None:
        entry = self.entries[index]
        if not entry.valid:
            return
        self._remove_line_index(entry.line, index)
        self.stats[reason] += 1
        if status is not None:
            self.on_resolve(entry, status)
        else:
            self.on_drop(entry)
        entry.valid = False

    def insert(
        self, addr: int, quality_index: int, quality_generation: int,
        demand_index: int, candidate_id: int, offset_context_index: int = 0,
        offset_context_generation: int = 0, recovery_generation: int = 0,
        audit_generation: int = 0,
    ) -> bool:
        line = self._line(addr)
        set_index, tag = self._key(line)
        base = set_index * self.config.feedback_ways
        self.clock += 1
        self.stats["feedback_insert_attempts"] += 1

        # A later request to a line already awaiting a direct label can never
        # add an independent useful/unused observation: one demand consumes
        # only the earliest candidate and labels later ones redundant. Do not
        # spend feedback capacity on those duplicates.
        for index in self.by_line.get(line, ()):
            if self.entries[index].valid:
                self.stats["feedback_coalesced"] += 1
                return False

        victim = next(
            (base + way for way in range(self.config.feedback_ways)
             if not self.entries[base + way].valid),
            None,
        )
        if victim is None:
            victim_way = self.set_next_victim[set_index]
            self.set_next_victim[set_index] = (
                victim_way + 1
            ) % self.config.feedback_ways
            victim = base + victim_way
            self._invalidate(victim, "feedback_evicted_without_label")
        entry = self.entries[victim]
        entry.valid = True
        entry.line = line
        entry.tag = tag
        entry.quality_index = quality_index
        entry.quality_generation = quality_generation
        entry.offset_context_index = offset_context_index
        entry.offset_context_generation = offset_context_generation
        entry.recovery_generation = recovery_generation
        entry.audit_generation = audit_generation
        entry.demand_index_at_issue = demand_index
        entry.candidate_id = candidate_id
        entry.last_touch = self.clock
        self.by_line[line].append(victim)
        heapq.heappush(
            self.expiry_heap,
            (demand_index + self.config.horizon, candidate_id, victim),
        )
        self.stats["feedback_inserted"] += 1
        return True

    def observe_demand(self, addr: int, demand_index: int) -> None:
        while self.expiry_heap and self.expiry_heap[0][0] < demand_index:
            _, candidate_id, index = heapq.heappop(self.expiry_heap)
            entry = self.entries[index]
            if entry.valid and entry.candidate_id == candidate_id:
                self._invalidate(index, "feedback_expired_without_label")

        line = self._line(addr)
        pending = self.by_line.get(line)
        if not pending:
            return
        # A demand consumes exactly one earliest sampled candidate. Later
        # samples to the same line are redundant feedback and intentionally do
        # not contribute a false negative label.
        while pending:
            index = pending[0]
            entry = self.entries[index]
            if not entry.valid:
                pending.popleft()
                continue
            self._invalidate(index, "feedback_useful", "useful")
            while pending:
                duplicate = pending[0]
                duplicate_entry = self.entries[duplicate]
                if not duplicate_entry.valid:
                    pending.popleft()
                    continue
                self._invalidate(duplicate, "feedback_redundant", "redundant")
            return

    def finish(self) -> None:
        while self.expiry_heap:
            _, candidate_id, index = heapq.heappop(self.expiry_heap)
            entry = self.entries[index]
            if entry.valid and entry.candidate_id == candidate_id:
                self._invalidate(index, "feedback_censored", "censored")

    def discard_pending(self, reason: str) -> None:
        """Drop unresolved samples at a reporting-window boundary."""
        for index, entry in enumerate(self.entries):
            if entry.valid:
                self._invalidate(index, reason)
        self.expiry_heap.clear()

    def report(self) -> dict[str, int]:
        return {key: int(value) for key, value in sorted(self.stats.items())}


class _QualityAccumulator:
    """One combined/isolated demand oracle with local pending state."""

    def __init__(
        self, horizon: int,
        on_resolve: Callable[[_IssuedCandidate, str], None] | None = None,
    ):
        self.horizon = horizon
        self.on_resolve = on_resolve
        self.eligible_demands = 0
        self.demand_index = 0
        self.pending_by_line: dict[int, deque[_IssuedCandidate]] = defaultdict(deque)
        self.expiry_heap: list[tuple[int, int, _IssuedCandidate]] = []
        self.pending_ids: set[int] = set()
        self.counts: Counter[str] = Counter()

    def observe_demand(self, demand: replay.Demand) -> None:
        self.eligible_demands += 1
        self.demand_index += 1
        while self.expiry_heap and self.expiry_heap[0][0] < self.demand_index:
            _, _, candidate = heapq.heappop(self.expiry_heap)
            if candidate.candidate_id in self.pending_ids:
                self._resolve(candidate, "unused")
        pending = self.pending_by_line.get(demand.addr)
        if not pending:
            return
        while pending and pending[0].candidate_id not in self.pending_ids:
            pending.popleft()
        if not pending:
            return
        self._resolve(pending.popleft(), "useful")
        while pending:
            self._resolve(pending.popleft(), "redundant")

    def emit(self, candidate: _IssuedCandidate) -> None:
        self.pending_ids.add(candidate.candidate_id)
        self.pending_by_line[candidate.addr].append(candidate)
        heapq.heappush(
            self.expiry_heap,
            (self.demand_index + self.horizon, candidate.candidate_id, candidate),
        )

    def _resolve(self, candidate: _IssuedCandidate, status: str) -> None:
        if candidate.candidate_id not in self.pending_ids:
            return
        self.pending_ids.remove(candidate.candidate_id)
        self.counts[status] += 1
        if self.on_resolve is not None:
            self.on_resolve(candidate, status)
        self._discard_resolved_prefix(candidate.addr)

    def _discard_resolved_prefix(self, addr: int) -> None:
        pending = self.pending_by_line.get(addr)
        if pending is None:
            return
        while pending and pending[0].candidate_id not in self.pending_ids:
            pending.popleft()
        if not pending:
            self.pending_by_line.pop(addr, None)

    def finish(self) -> replay.QualityMetrics:
        while self.expiry_heap:
            _, _, candidate = heapq.heappop(self.expiry_heap)
            self._resolve(candidate, "censored")
        denominator = (
            self.counts["useful"] + self.counts["unused"] + self.counts["redundant"]
        )
        return replay.QualityMetrics(
            candidates=sum(self.counts.values()),
            useful=self.counts["useful"],
            unused=self.counts["unused"],
            redundant=self.counts["redundant"],
            censored=self.counts["censored"],
            eligible_demands=self.eligible_demands,
            covered_demands=self.counts["useful"],
            accuracy=(self.counts["useful"] / denominator) if denominator else None,
            coverage=(self.counts["useful"] / self.eligible_demands)
            if self.eligible_demands else None,
            mean_demand_distance=None,
            mean_tick_distance=None,
        )


class DirectQualityReplay:
    """Causal direct-feedback policy plus raw/direct quality scoreboards."""

    def __init__(self, config: DirectQualityConfig, window: replay.EvaluationWindow):
        self.config = config
        self.window = window
        self.controller = DirectQualityController(config)
        self.context_counts: dict[tuple[int, str], _ContextCounts] = defaultdict(_ContextCounts)
        self.candidate_sample_context: dict[int, tuple[tuple[int, str], bool]] = {}
        self.next_candidate_id = 0
        self.feedback_demand_index = 0
        self.entered_evaluation_window = False
        self.raw = self._make_metrics(self._resolve_raw_combined)
        self.direct = self._make_metrics(self._resolve_direct_combined)
        self.feedback = SampledFeedbackTable(
            config, self._resolve_feedback, self._drop_feedback,
        )
        self.stats: Counter[str] = Counter()

    def _make_metrics(
        self, combined_resolve: Callable[[_IssuedCandidate, str], None] | None = None,
    ) -> dict[str, _QualityAccumulator]:
        return {
            kind: _QualityAccumulator(
                self.config.horizon,
                combined_resolve if kind == "combined" else None,
            )
            for kind in ("combined", "large", "small")
        }

    @staticmethod
    def _context(event: replay.ReplayEvent) -> tuple[int, str] | None:
        if not event.trigger_has_pc:
            return None
        return event.trigger_pc, event.bop_kind

    def _candidate(
        self, event: replay.ReplayEvent, context: tuple[int, str] | None,
    ) -> _IssuedCandidate:
        candidate = _IssuedCandidate(
            self.next_candidate_id, event.bop_kind, event.access_seq, event.tick,
            event.raw_candidate_addr, self.raw["combined"].demand_index,
            event.phase_id, context,
        )
        self.next_candidate_id += 1
        return candidate

    def observe_demand(self, demand: replay.Demand) -> None:
        selected = replay._in_evaluation_window(demand, self.window)
        if selected and not self.entered_evaluation_window:
            # Preserve warmup-trained state, but a warmup candidate must not
            # consume a stable-window demand. Its unresolved label is dropped.
            self.feedback.discard_pending("feedback_window_boundary_drop")
            self.entered_evaluation_window = True
        self.feedback_demand_index += 1
        self.feedback.observe_demand(demand.addr, self.feedback_demand_index)
        if selected:
            for accumulator in self.raw.values():
                accumulator.observe_demand(demand)
            for accumulator in self.direct.values():
                accumulator.observe_demand(demand)
        self.stats["demands"] += 1

    def observe_event(self, event: replay.ReplayEvent) -> None:
        self.stats["events"] += 1
        if not event.raw_candidate_valid:
            return
        selected = replay._in_evaluation_window(event, self.window)
        context = self._context(event)
        raw_candidate = self._candidate(event, context)
        if context is not None and selected:
            self.context_counts[context].raw_candidates += 1
        if selected:
            for accumulator in (self.raw["combined"], self.raw[event.bop_kind]):
                accumulator.emit(raw_candidate)

        if context is None:
            issued = True
            sampled = False
            state = "open"
            lookup = None
        else:
            pc, kind = context
            lookup = self.controller.lookup(pc, kind, event.best_offset_after)
            state = lookup.state
            if selected:
                counts = self.context_counts[context]
                setattr(
                    counts, f"state_{state}",
                    getattr(counts, f"state_{state}") + 1,
                )
            issued, sampled = self.controller.should_issue(
                lookup, pc, kind, event.trigger_addr & ~(64 - 1),
            )

        if not issued:
            self.stats["policy_suppressed"] += 1
            if context is not None and selected:
                self.context_counts[context].suppressed_candidates += 1
            return

        direct_candidate = self._candidate(event, context)
        if selected:
            for accumulator in (self.direct["combined"], self.direct[event.bop_kind]):
                accumulator.emit(direct_candidate)
        self.stats["policy_issued"] += 1
        if context is not None and selected:
            self.context_counts[context].issued_candidates += 1

        if sampled and lookup is not None:
            inserted = self.feedback.insert(
                direct_candidate.addr, lookup.index, lookup.generation,
                self.feedback_demand_index, direct_candidate.candidate_id,
                lookup.context_index, lookup.context_generation,
                lookup.recovery_generation, lookup.audit_generation,
            )
            if inserted:
                # The feedback callbacks are only reachable after a successful
                # allocation. Coalesced samples must not leave an unresolvable
                # candidate-id mapping behind.
                self.candidate_sample_context[direct_candidate.candidate_id] = (
                    context, selected,
                )
                if selected:
                    self.context_counts[context].samples_inserted += 1

    def _resolve_feedback(self, entry: _FeedbackEntry, status: str) -> None:
        context_record = self.candidate_sample_context.pop(entry.candidate_id, None)
        if context_record is None:
            self.stats["feedback_untracked_candidate"] += 1
            return
        candidate_context, selected = context_record
        if status == "useful":
            if selected:
                self.context_counts[candidate_context].samples_useful += 1
            before, after = self.controller.note_sample(
                entry.quality_index, entry.quality_generation, "useful",
                entry.offset_context_index, entry.offset_context_generation,
                entry.recovery_generation, entry.audit_generation,
            )
        elif status == "unused":
            if selected:
                self.context_counts[candidate_context].samples_unused += 1
            before, after = self.controller.note_sample(
                entry.quality_index, entry.quality_generation, "unused",
                entry.offset_context_index, entry.offset_context_generation,
                entry.recovery_generation, entry.audit_generation,
            )
        elif status == "redundant":
            if selected:
                self.context_counts[candidate_context].samples_redundant += 1
        else:
            if selected:
                self.context_counts[candidate_context].samples_censored += 1
            return
        if selected:
            counts = self.context_counts[candidate_context]
            if before in ("block", "recover") and after == "audit":
                counts.audit_entries += 1
            elif before == "audit" and after == "open":
                counts.audit_passes += 1
            elif before == "audit" and after == "block":
                counts.audit_failures += 1

    def _resolve_direct_combined(
        self, candidate: _IssuedCandidate, status: str,
    ) -> None:
        if candidate.context is not None:
            self.context_counts[candidate.context].resolve_quality(status)

    def _resolve_raw_combined(
        self, candidate: _IssuedCandidate, status: str,
    ) -> None:
        if candidate.context is not None:
            self.context_counts[candidate.context].resolve_raw_quality(status)

    def _drop_feedback(self, entry: _FeedbackEntry) -> None:
        context_record = self.candidate_sample_context.pop(entry.candidate_id, None)
        if context_record is None:
            self.stats["feedback_drop_untracked_candidate"] += 1
            return
        context, selected = context_record
        if selected:
            self.context_counts[context].samples_dropped += 1

    def finish(self) -> dict[str, object]:
        self.feedback.finish()
        raw = {kind: asdict(accumulator.finish()) for kind, accumulator in self.raw.items()}
        direct = {
            kind: asdict(accumulator.finish()) for kind, accumulator in self.direct.items()
        }
        return {
            "quality": {"raw": raw, "direct_quality_gate": direct},
            "direct_quality_config": asdict(self.config),
            "controller_stats": self.controller.report(),
            "feedback_stats": self.feedback.report(),
            "replay_stats": {key: int(value) for key, value in sorted(self.stats.items())},
            "contexts": self._context_report(),
            "marginal_vs_raw": _marginal(direct["combined"], raw["combined"]),
        }

    def _context_report(self) -> list[dict[str, object]]:
        rows = []
        for (pc, kind), counts in self.context_counts.items():
            final_offset_contexts = self.controller.context_snapshot(pc, kind)
            single_context = (
                final_offset_contexts[0]
                if self.config.offset_context_slots == 1 and final_offset_contexts
                else None
            )
            raw = {
                "useful": counts.raw_useful,
                "unused": counts.raw_unused,
                "redundant": counts.raw_redundant,
                "censored": counts.raw_censored,
            }
            direct = {
                "useful": counts.useful,
                "unused": counts.unused,
                "redundant": counts.redundant,
                "censored": counts.censored,
            }
            row = {
                "issuer_trigger_pc": hex(pc),
                "bop_kind": kind,
                "raw_candidates": counts.raw_candidates,
                "raw_useful": counts.raw_useful,
                "raw_unused": counts.raw_unused,
                "raw_redundant": counts.raw_redundant,
                "raw_censored": counts.raw_censored,
                "issued_candidates": counts.issued_candidates,
                "suppressed_candidates": counts.suppressed_candidates,
                "useful": counts.useful,
                "unused": counts.unused,
                "redundant": counts.redundant,
                "censored": counts.censored,
                "samples_inserted": counts.samples_inserted,
                "samples_useful": counts.samples_useful,
                "samples_unused": counts.samples_unused,
                "samples_redundant": counts.samples_redundant,
                "samples_dropped": counts.samples_dropped,
                "samples_censored": counts.samples_censored,
                "state_observe_checks": counts.state_observe,
                "state_open_checks": counts.state_open,
                "state_block_checks": counts.state_block,
                # Retain the original fields for K=1 consumers. Multi-offset
                # results must use the explicit context list below.
                "final_direct_state": (
                    single_context["state"] if single_context is not None else None
                ),
                "final_sampled_useful": (
                    single_context["sampled_useful"]
                    if single_context is not None else None
                ),
                "final_sampled_unused": (
                    single_context["sampled_unused"]
                    if single_context is not None else None
                ),
                "final_offset_contexts": final_offset_contexts,
                # Candidate removal can transfer a later-demand useful label
                # between PCs. Per-context marginal results are diagnostic;
                # only combined marginal accounting is a pass/fail contract.
                "marginal_vs_raw_diagnostic": _marginal(direct, raw),
            }
            if self.config.audit_samples:
                row["state_audit_checks"] = counts.state_audit
                row["audit_entries"] = counts.audit_entries
                row["audit_passes"] = counts.audit_passes
                row["audit_failures"] = counts.audit_failures
            if self.config.reopen_confirm_samples:
                row["state_recover_checks"] = counts.state_recover
            rows.append(row)
        return sorted(
            rows,
            key=lambda row: (-int(row["suppressed_candidates"]), row["issuer_trigger_pc"], row["bop_kind"]),
        )


def _marginal(
    actual: Mapping[str, object], raw: Mapping[str, object],
) -> dict[str, object]:
    lost_useful = int(raw["useful"]) - int(actual["useful"])
    saved_unused = int(raw["unused"]) - int(actual["unused"])
    added_useful = max(0, -lost_useful)
    added_unused = max(0, -saved_unused)
    return {
        "lost_useful": max(0, lost_useful),
        "saved_unused": max(0, saved_unused),
        "saved_unused_per_lost_useful": (
            saved_unused / lost_useful if lost_useful > 0 else None
        ),
        "added_useful": added_useful,
        "added_unused": added_unused,
        "added_unused_per_added_useful": (
            added_unused / added_useful if added_useful > 0 else None
        ),
        "value_at_10_to_1": saved_unused - 10 * lost_useful,
        "passes_10_to_1": saved_unused >= 10 * lost_useful,
    }


def replay_direct_quality_gate(
    connection: sqlite3.Connection, config: DirectQualityConfig,
    window: replay.EvaluationWindow,
) -> dict[str, object]:
    connection.row_factory = sqlite3.Row
    runner = DirectQualityReplay(config, window)
    replay._stream_trace_rows(
        connection, runner.observe_demand, runner.observe_event, lambda: None,
    )
    return runner.finish()


def _parse_config(path: Path | None) -> DirectQualityConfig:
    if path is None:
        return DirectQualityConfig()
    payload = json.loads(path.read_text())
    if not isinstance(payload, dict):
        raise ValueError("direct-quality config must be a JSON object")
    fields = set(DirectQualityConfig.__dataclass_fields__)
    unknown = sorted(set(payload) - fields)
    if unknown:
        raise ValueError("unknown direct-quality config fields: " + ", ".join(unknown))
    return DirectQualityConfig(**payload)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("database", type=Path, help="certified V5 BOP trace DB")
    parser.add_argument(
        "--config", type=Path, default=None,
        help="offline direct-quality JSON override",
    )
    parser.add_argument(
        "--evaluation-phase", default="stable",
        help="V5 reporting phase; full trace state is replayed",
    )
    parser.add_argument("--output", type=Path, default=None)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    started = time.perf_counter()
    config = _parse_config(args.config)
    with sqlite3.connect(args.database) as connection:
        _, phases = replay._streaming_metadata(connection)
        window = replay.resolve_evaluation_window(
            phase_name=args.evaluation_phase,
            start_tick=None,
            stats_path=None,
            stats_block=1,
            phases=phases,
        )
        report = replay_direct_quality_gate(connection, config, window)
    report.update({
        "database": str(args.database),
        "evaluation_window": {
            "name": window.name,
            "source": window.source,
            "phase_id": window.phase_id,
            "phase_name": window.phase_name,
            "start_tick": window.start_tick,
            "state_replay": "full_trace",
            "demand_oracle": "selected_window_only",
        },
        "model": {
            "controller": "causal_sampled_direct_candidate_quality",
            "candidate_stream": "recorded_online_raw_bop",
            "feedback_label": "later_l2_demand_read_only",
            "feedback_table_eviction": "drop_without_negative_label",
            "not_modeled": "local filters, cache fills, MSHRs, residency, bandwidth",
            "marginal_pass_rule": "saved_unused >= 10 * lost_useful",
        },
        "replay_engine": {
            "engine": "streaming_direct_quality_gate",
            "wall_seconds": time.perf_counter() - started,
        },
    })
    encoded = json.dumps(report, indent=2, sort_keys=True)
    if args.output is None:
        print(encoded)
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
