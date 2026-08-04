#!/usr/bin/env python3
"""Counterfactually replay RR evidence used by BOP PC validation.

The trace's raw BOP candidate and selected best offset remain fixed.  This
tool changes only the availability of the retrospective RR predecessor used
by PC validation, then replays the shared PC/global controller and evaluates
the resulting candidates against the L2-demand oracle at Horizon 2048.

The points are deliberately controller-side counterfactuals, not new
learner configurations:

* ``current`` uses the recorded online ValidationHit exactly.
* ``no_conflict`` preserves native delay actions but retains every matured RR
  line, removing direct-mapped replacement as a source of validation misses.
* ``unique_lru`` preserves native delay actions and native RR hits, but allows
  a recorded miss to use one of the most recent unique matured demand lines.
* ``lru_full`` exposes the same bounded LRU behavior with recovered evidence
  explicitly source-tagged, so admission and producer-credit factors can be
  isolated without changing the legacy ``unique_lru`` point.
* ``no_delay_drop`` accepts every training line into the native delayed path,
  retains the original delay timing, and still uses the finite direct-mapped
  RR.  Newly admitted lines can therefore create different RR conflicts.
* ``no_conflict_no_drop`` combines the two idealizations.

Local filters, cache fills, MSHRs, and cache residency remain outside this
model.  The output is filter-free raw-candidate quality, as in the existing
BOP replay tools.
"""

from __future__ import annotations

import argparse
import json
import sqlite3
import time
from collections import Counter, OrderedDict, deque
from dataclasses import asdict, dataclass, replace
from pathlib import Path
from typing import Iterable, Iterator

import bop_replay as replay


HORIZON = 2048
POINTS = (
    "current",
    "no_conflict",
    "unique_lru",
    "lru_full",
    "direct_only",
    "credit_only",
    "cross_pc_credit_off",
    "same_pc_credit_only",
    "cross_pc_credit_only",
    "unique_lru_offset_match_r1",
    "unique_lru_offset_match_r1_gate",
    "unique_lru_offset_match_r1_gate_probation",
    "no_delay_drop",
    "no_conflict_no_drop",
)


@dataclass(frozen=True)
class _EvidenceEntry:
    line: int
    tag: int
    originated_from_delay_drop: bool
    owner_pc: int
    owner_valid: bool
    # Offset used by the valid producer when this demand entered the delayed
    # BOP history.  It is distinct from the current validation offset.
    owner_best_offset: int = 0
    # Ordinal of this line's most recent native RR maturity for its BOP.
    mature_ordinal: int = 0


class _RecordedActionCursor:
    """Consume each V5 per-BOP action stream without materializing it."""

    def __init__(
        self, connection: sqlite3.Connection,
        configs_by_name: dict[str, replay.BOPConfig],
    ):
        self.configs_by_name = configs_by_name
        self.cursors: dict[str, Iterator[sqlite3.Row]] = {}
        self.next_rows: dict[str, sqlite3.Row | None] = {}
        for bop_name, config in configs_by_name.items():
            if not config.delay_queue_enabled:
                continue
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

    def _advance(self, bop_name: str) -> replay.ReplayDelayAction:
        row = self.next_rows[bop_name]
        if row is None:
            raise ValueError(
                f"{bop_name}: delay-action stream ended before learner event"
            )
        self.next_rows[bop_name] = next(self.cursors[bop_name], None)
        return replay._stream_delay_action_from_row(row)

    def actions_for_event(
        self, event: replay.ReplayEvent,
    ) -> tuple[list[replay.ReplayDelayAction], replay.ReplayDelayAction | None]:
        """Return native dequeues before one trigger, plus its action."""
        config = self.configs_by_name[event.bop_name]
        if not config.delay_queue_enabled:
            return [], None

        before: list[replay.ReplayDelayAction] = []
        row = self.next_rows[event.bop_name]
        while row is not None and int(row["ReplayOrder"]) < event.replay_order:
            action = self._advance(event.bop_name)
            if action.action != "dequeue_to_rr":
                raise ValueError(
                    f"{event.bop_name}: unexpected {action.action!r} before "
                    "learner trigger"
                )
            before.append(action)
            row = self.next_rows[event.bop_name]
        if row is not None and int(row["ReplayOrder"]) == event.replay_order:
            return before, self._advance(event.bop_name)
        raise ValueError(
            f"{event.bop_name}: missing delay action at replay order "
            f"{event.replay_order}"
        )


class _PersistentRREvidence:
    """Native delay timing with ideal no-replacement RR presence."""

    def __init__(self, config: replay.BOPConfig):
        self.config = config
        self.mature_lines: dict[int, _EvidenceEntry] = {}
        self.delay_queue: deque[_EvidenceEntry] = deque()
        self.mature_insertions = 0
        self.delay_dequeues = 0
        self.delay_enqueues = 0
        self.delay_drops = 0

    def _line(self, address: int) -> int:
        return (address & replay.UINT64_MASK) & ~(self.config.block_size - 1)

    def _tag(self, address: int) -> int:
        line_number = self._line(address) // self.config.block_size
        index_bits = self.config.rr_entries.bit_length() - 1
        return (line_number >> index_bits) & ((1 << self.config.tag_bits) - 1)

    def _entry(self, event: replay.ReplayEvent) -> _EvidenceEntry:
        return _EvidenceEntry(
            self._line(event.trigger_addr),
            self._tag(event.trigger_addr),
            False,
            event.trigger_pc,
            event.trigger_has_pc and event.trigger_is_demand
            and event.trigger_is_read,
            event.best_offset_after,
        )

    def _mature(self, entry: _EvidenceEntry) -> None:
        """Record one native RR insertion and refresh this line's age."""
        self.mature_insertions += 1
        self.mature_lines[entry.line] = replace(
            entry, mature_ordinal=self.mature_insertions,
        )

    def apply_dequeue(self, action: replay.ReplayDelayAction) -> None:
        if action.action != "dequeue_to_rr":
            raise ValueError(f"unexpected native action {action.action!r}")
        if not self.delay_queue:
            raise ValueError("native delay dequeue on an empty evidence queue")
        entry = self.delay_queue.popleft()
        if entry.line != self._line(action.addr):
            raise ValueError("native delay dequeue address mismatch")
        self._mature(entry)
        if len(self.delay_queue) != action.queue_size_after:
            raise ValueError("native delay dequeue queue size mismatch")
        self.delay_dequeues += 1

    def apply_trigger(
        self, event: replay.ReplayEvent, action: replay.ReplayDelayAction | None,
    ) -> None:
        entry = self._entry(event)
        if not self.config.delay_queue_enabled:
            self._mature(entry)
            return
        if action is None:
            raise ValueError(f"{event.bop_name}: missing native delay action")
        if self._line(action.addr) != entry.line:
            raise ValueError(f"{event.bop_name}: native delay address mismatch")
        if action.action == "enqueue":
            self.delay_queue.append(entry)
            self.delay_enqueues += 1
        elif action.action == "drop_full":
            self.delay_drops += 1
        else:
            raise ValueError(
                f"{event.bop_name}: unexpected trigger action {action.action!r}"
            )
        if len(self.delay_queue) != action.queue_size_after:
            raise ValueError("native delay trigger queue size mismatch")

    def hit(
        self, address: int, max_insert_age: int | None = None,
    ) -> tuple[bool, str, int, bool]:
        line = self._line(address)
        # The untouched right bank in the online BOP makes tag-zero an alias.
        if self._tag(line) == 0:
            return True, "tag_zero_alias", 0, False
        entry = self.mature_lines.get(line)
        if entry is not None:
            age = self.mature_insertions - entry.mature_ordinal
            if max_insert_age is not None and age > max_insert_age:
                return False, "stale_matured", 0, False
            return True, "matured_recorded", entry.owner_pc, entry.owner_valid
        return False, "miss", 0, False

    def stats(self) -> dict[str, int]:
        return {
            "mature_lines": len(self.mature_lines),
            "mature_insertions": self.mature_insertions,
            "delay_pending": len(self.delay_queue),
            "delay_dequeues": self.delay_dequeues,
            "delay_enqueues": self.delay_enqueues,
            "delay_drops": self.delay_drops,
        }


class _UniqueAddressLRUEvidence:
    """Bounded exact-line evidence populated at native RR maturity.

    This is deliberately separate from the online direct-mapped RR.  A
    recorded RR hit remains authoritative; this structure is queried only for
    a recorded miss, so its full-address lookup can recover replacement loss
    without changing native tag-alias behavior.  The fixed-size OrderedDict is
    a bounded unique-address LRU: each maturity refreshes recency, and an
    address without a valid producer PC cannot overwrite a prior valid owner.
    """

    def __init__(self, config: replay.BOPConfig, capacity: int):
        if capacity <= 0:
            raise ValueError("unique-address LRU capacity must be positive")
        self.config = config
        self.capacity = capacity
        self.entries: OrderedDict[int, _EvidenceEntry] = OrderedDict()
        self.delay_queue: deque[_EvidenceEntry] = deque()
        self.mature_insertions = 0
        self.delay_dequeues = 0
        self.delay_enqueues = 0
        self.delay_drops = 0
        self.duplicate_refreshes = 0
        self.capacity_evictions = 0
        self.max_resident = 0

    def _line(self, address: int) -> int:
        return (address & replay.UINT64_MASK) & ~(self.config.block_size - 1)

    def _tag(self, address: int) -> int:
        line_number = self._line(address) // self.config.block_size
        index_bits = self.config.rr_entries.bit_length() - 1
        return (line_number >> index_bits) & ((1 << self.config.tag_bits) - 1)

    def _entry(self, event: replay.ReplayEvent) -> _EvidenceEntry:
        return _EvidenceEntry(
            self._line(event.trigger_addr),
            self._tag(event.trigger_addr),
            False,
            event.trigger_pc,
            event.trigger_has_pc and event.trigger_is_demand
            and event.trigger_is_read,
            event.best_offset_after,
        )

    def _mature(self, entry: _EvidenceEntry) -> None:
        self.mature_insertions += 1
        existing = self.entries.get(entry.line)
        if existing is not None:
            self.duplicate_refreshes += 1
            # Keep the newest usable producer identity.  A demand without a
            # PC still refreshes the address but cannot erase a valid owner.
            if not entry.owner_valid:
                entry = replace(
                    entry, owner_pc=existing.owner_pc,
                    owner_valid=existing.owner_valid,
                    owner_best_offset=existing.owner_best_offset,
                )
            self.entries[entry.line] = entry
            self.entries.move_to_end(entry.line)
            return
        if len(self.entries) == self.capacity:
            self.entries.popitem(last=False)
            self.capacity_evictions += 1
        self.entries[entry.line] = entry
        self.max_resident = max(self.max_resident, len(self.entries))

    def apply_dequeue(self, action: replay.ReplayDelayAction) -> None:
        if action.action != "dequeue_to_rr":
            raise ValueError(f"unexpected native action {action.action!r}")
        if not self.delay_queue:
            raise ValueError("native delay dequeue on an empty evidence queue")
        entry = self.delay_queue.popleft()
        if entry.line != self._line(action.addr):
            raise ValueError("native delay dequeue address mismatch")
        self._mature(entry)
        if len(self.delay_queue) != action.queue_size_after:
            raise ValueError("native delay dequeue queue size mismatch")
        self.delay_dequeues += 1

    def apply_trigger(
        self, event: replay.ReplayEvent, action: replay.ReplayDelayAction | None,
    ) -> None:
        entry = self._entry(event)
        if not self.config.delay_queue_enabled:
            self._mature(entry)
            return
        if action is None:
            raise ValueError(f"{event.bop_name}: missing native delay action")
        if self._line(action.addr) != entry.line:
            raise ValueError(f"{event.bop_name}: native delay address mismatch")
        if action.action == "enqueue":
            self.delay_queue.append(entry)
            self.delay_enqueues += 1
        elif action.action == "drop_full":
            self.delay_drops += 1
        else:
            raise ValueError(
                f"{event.bop_name}: unexpected trigger action {action.action!r}"
            )
        if len(self.delay_queue) != action.queue_size_after:
            raise ValueError("native delay trigger queue size mismatch")

    def hit_detail(self, address: int) -> tuple[bool, str, int, bool, int]:
        entry = self.entries.get(self._line(address))
        if entry is None:
            return False, "lru_miss", 0, False, 0
        self.entries.move_to_end(entry.line)
        return (
            True,
            "unique_lru",
            entry.owner_pc,
            entry.owner_valid,
            entry.owner_best_offset,
        )

    def hit(self, address: int) -> tuple[bool, str, int, bool]:
        hit, source, owner_pc, owner_valid, _ = self.hit_detail(address)
        return hit, source, owner_pc, owner_valid

    def stats(self) -> dict[str, int]:
        return {
            "capacity": self.capacity,
            "resident_lines": len(self.entries),
            "max_resident": self.max_resident,
            "mature_insertions": self.mature_insertions,
            "duplicate_refreshes": self.duplicate_refreshes,
            "capacity_evictions": self.capacity_evictions,
            "delay_pending": len(self.delay_queue),
            "delay_dequeues": self.delay_dequeues,
            "delay_enqueues": self.delay_enqueues,
            "delay_drops": self.delay_drops,
        }


class _NoDropRREvidence:
    """Unbounded native delay queue followed by the finite direct-mapped RR."""

    def __init__(self, config: replay.BOPConfig, persistent: bool):
        self.config = config
        self.persistent = persistent
        self.rr_left: list[_EvidenceEntry | None] = [None] * config.rr_entries
        self.mature_lines: dict[int, _EvidenceEntry] = {}
        self.delay_queue: deque[tuple[_EvidenceEntry, int]] = deque()
        self.delay_event_tick: int | None = None
        self.delay_enqueues = 0
        self.delay_dequeues = 0
        self.direct_replacements = 0
        self.max_delay_depth = 0

    def _line(self, address: int) -> int:
        return (address & replay.UINT64_MASK) & ~(self.config.block_size - 1)

    def _hash(self, address: int) -> int:
        line_number = self._line(address) // self.config.block_size
        index_bits = self.config.rr_entries.bit_length() - 1
        mask = self.config.rr_entries - 1
        return (line_number & mask) ^ ((line_number >> index_bits) & mask)

    def _tag(self, address: int) -> int:
        line_number = self._line(address) // self.config.block_size
        index_bits = self.config.rr_entries.bit_length() - 1
        return (line_number >> index_bits) & ((1 << self.config.tag_bits) - 1)

    def _next_cycle(self, tick: int) -> int:
        period = self.config.clock_period_ticks
        return ((tick + period - 1) // period + 1) * period

    def _mature(self, entry: _EvidenceEntry) -> None:
        if self.persistent:
            self.mature_lines[entry.line] = entry
            return
        index = self._hash(entry.line)
        previous = self.rr_left[index]
        if previous is not None and previous.line != entry.line:
            self.direct_replacements += 1
        self.rr_left[index] = entry

    def _run_delay_events_before(self, tick: int) -> None:
        """Match BOP's strict-before-trigger callback ordering."""
        while self.delay_event_tick is not None and self.delay_event_tick < tick:
            callback_tick = self.delay_event_tick
            self.delay_event_tick = None
            if self.delay_queue and self.delay_queue[0][1] <= callback_tick:
                entry, _ = self.delay_queue.popleft()
                self._mature(entry)
                self.delay_dequeues += 1
            if not self.delay_queue:
                continue
            if self.delay_queue[0][1] <= callback_tick:
                self.delay_event_tick = self._next_cycle(callback_tick)
            else:
                self.delay_event_tick = self.delay_queue[0][1]

    def apply_trigger(
        self, event: replay.ReplayEvent, originated_from_delay_drop: bool,
    ) -> None:
        self._run_delay_events_before(event.tick)
        entry = _EvidenceEntry(
            self._line(event.trigger_addr),
            self._tag(event.trigger_addr),
            originated_from_delay_drop,
            event.trigger_pc,
            event.trigger_has_pc and event.trigger_is_demand
            and event.trigger_is_read,
            event.best_offset_after,
        )
        if not self.config.delay_queue_enabled:
            self._mature(entry)
            return
        process_tick = event.tick + self.config.delay_ticks
        self.delay_queue.append((entry, process_tick))
        self.delay_enqueues += 1
        self.max_delay_depth = max(self.max_delay_depth, len(self.delay_queue))
        if self.delay_event_tick is None:
            self.delay_event_tick = process_tick

    def hit(self, address: int) -> tuple[bool, str, int, bool]:
        line = self._line(address)
        if self._tag(line) == 0:
            return True, "tag_zero_alias", 0, False
        if self.persistent:
            entry = self.mature_lines.get(line)
            if entry is not None:
                return (
                    True,
                    "delay_drop" if entry.originated_from_delay_drop
                    else "matured_recorded",
                    entry.owner_pc,
                    entry.owner_valid,
                )
            return False, "miss", 0, False
        entry = self.rr_left[self._hash(line)]
        if entry is not None and entry.tag == self._tag(line):
            return (
                True,
                "delay_drop" if entry.originated_from_delay_drop
                else "matured_recorded",
                entry.owner_pc,
                entry.owner_valid,
            )
        return False, "miss", 0, False

    def stats(self) -> dict[str, int]:
        return {
            "delay_enqueues": self.delay_enqueues,
            "delay_dequeues": self.delay_dequeues,
            "delay_pending": len(self.delay_queue),
            "max_delay_depth": self.max_delay_depth,
            "direct_replacements": self.direct_replacements,
            "mature_lines": len(self.mature_lines) if self.persistent else 0,
        }


class _CounterfactualPoint:
    """One controller/oracle replay and its validation-delta accounting."""

    def __init__(
        self, name: str, config: replay.BOPConfig,
        window: replay.EvaluationWindow, no_conflict_max_insert_age: int | None = None,
    ):
        self.name = name
        self.config = config
        self.no_conflict_max_insert_age = no_conflict_max_insert_age
        self.policy = replay._StreamingPolicyReplay(config, HORIZON, window)
        self.validation: Counter[str] = Counter()

    @staticmethod
    def _is_validation_check(event: replay.ReplayEvent) -> bool:
        return (
            event.raw_candidate_valid
            and event.validation_enabled
            and event.validation_hit in (0, 1)
        )

    def observe_demand(self, demand: replay.Demand) -> None:
        self.policy.observe_demand(demand)

    def emit(
        self, event: replay.ReplayEvent, validation_hit: int, source: str,
        selected: bool, validation_owner_pc: int = 0,
        validation_owner_valid: bool = False,
        validation_source: str = "native",
    ) -> None:
        if self._is_validation_check(event):
            self.validation["checks"] += 1
            if event.validation_hit:
                self.validation["recorded_hits"] += 1
            else:
                self.validation["recorded_misses"] += 1
            if validation_hit:
                self.validation["counterfactual_hits"] += 1
            else:
                self.validation["counterfactual_misses"] += 1
                if source == "stale_matured":
                    self.validation["stale_age_misses"] += 1
            lru_recovery = (
                not event.validation_hit
                and source in (
                    "unique_lru",
                    "unique_lru_offset_match",
                    "unique_lru_offset_mismatch",
                )
            )
            if lru_recovery:
                self.validation["recovered_unique_lru_detected_hits"] += 1
                if source == "unique_lru_offset_match":
                    self.validation["recovered_unique_lru_offset_match_hits"] += 1
                elif source == "unique_lru_offset_mismatch":
                    self.validation["recovered_unique_lru_offset_mismatch_hits"] += 1
            if not event.validation_hit and validation_hit:
                self.validation["recovered_hits"] += 1
                if source == "delay_drop":
                    self.validation["recovered_delay_drop_hits"] += 1
                elif source == "matured_recorded":
                    self.validation["recovered_conflict_hits"] += 1
                elif source in ("unique_lru", "unique_lru_offset_match"):
                    self.validation["recovered_unique_lru_hits"] += 1
                else:
                    self.validation["recovered_other_hits"] += 1
            elif event.validation_hit and not validation_hit:
                self.validation["lost_recorded_hits"] += 1

        self.policy.emit_values(
            bop_kind=event.bop_kind,
            best_offset=event.best_offset_after,
            best_offset_changed=event.best_offset_changed,
            raw_candidate_valid=event.raw_candidate_valid,
            raw_candidate_addr=event.raw_candidate_addr,
            pc_confidence_enabled=event.pc_confidence_enabled,
            validation_enabled=event.validation_enabled,
            validation_hit=validation_hit,
            trigger_addr=event.trigger_addr,
            trigger_pc=event.trigger_pc,
            trigger_has_pc=event.trigger_has_pc,
            access_seq=event.access_seq,
            tick=event.tick,
            phase_id=event.phase_id,
            selected=selected,
            validation_owner_pc=validation_owner_pc,
            validation_owner_valid=validation_owner_valid,
            validation_source=validation_source,
        )

    def commit(self) -> None:
        self.policy.commit()

    def report(self) -> dict[str, object]:
        checks = self.validation["checks"]
        validation = {
            key: int(self.validation[key])
            for key in (
                "checks",
                "recorded_hits",
                "recorded_misses",
                "counterfactual_hits",
                "counterfactual_misses",
                "recovered_hits",
                "recovered_conflict_hits",
                "recovered_unique_lru_hits",
                "recovered_delay_drop_hits",
                "recovered_other_hits",
                "recovered_unique_lru_detected_hits",
                "recovered_unique_lru_offset_match_hits",
                "recovered_unique_lru_offset_mismatch_hits",
                "lost_recorded_hits",
                "stale_age_misses",
            )
        }
        validation["recorded_hit_rate"] = (
            validation["recorded_hits"] / checks if checks else 0.0
        )
        validation["counterfactual_hit_rate"] = (
            validation["counterfactual_hits"] / checks if checks else 0.0
        )
        return {
            "counterfactual": {
                "no_conflict_max_insert_age": self.no_conflict_max_insert_age,
            },
            "controller_parameters": replay._controller_parameter_report(
                self.config
            ),
            "validation": validation,
            "controller_stats": self.policy.controller.stats(),
            "quality": {
                kind: asdict(metrics)
                for kind, metrics in self.policy.finish().items()
            },
        }


def _validation_address(
    event: replay.ReplayEvent, config: replay.BOPConfig,
) -> int:
    return (
        event.trigger_addr - event.best_offset_after * config.block_size
    ) & replay.UINT64_MASK


def _configs_by_name(config: replay.BOPConfig) -> dict[str, replay.BOPConfig]:
    result = {
        learner.bop_name: learner
        for learner in config.learner_configs.values()
    }
    if len(result) != 2:
        raise ValueError("counterfactual replay requires exactly Large and Small BOP")
    return result


_OFFSET_MATCH_LRU_POINTS = (
    "unique_lru_offset_match_r1",
    "unique_lru_offset_match_r1_gate",
    "unique_lru_offset_match_r1_gate_probation",
)
_RECOVERY_FACTOR_POINTS = (
    "lru_full",
    "direct_only",
    "credit_only",
    "cross_pc_credit_off",
    "same_pc_credit_only",
    "cross_pc_credit_only",
)
_LRU_POINTS = ("unique_lru", *_RECOVERY_FACTOR_POINTS, *_OFFSET_MATCH_LRU_POINTS)


def _point_config(name: str, config: replay.BOPConfig) -> replay.BOPConfig:
    """Return one offline-only recovered-evidence controller profile."""
    values: dict[str, object]
    if name in _RECOVERY_FACTOR_POINTS:
        values = {
            "pc_validation_recovered_admission": name != "credit_only",
            "pc_validation_recovered_producer_credit": name != "direct_only",
            "pc_validation_recovered_same_pc_credit": (
                name != "cross_pc_credit_only"
            ),
            "pc_validation_recovered_cross_pc_credit": (
                name not in ("cross_pc_credit_off", "same_pc_credit_only")
            ),
        }
    elif name in _OFFSET_MATCH_LRU_POINTS:
        values = {
            "pc_validation_recovered_hit_increment": 1,
            "pc_validation_recovered_same_pc_hit_gate": (
                name != "unique_lru_offset_match_r1"
            ),
            "pc_validation_recovered_probation": (
                name == "unique_lru_offset_match_r1_gate_probation"
            ),
        }
    else:
        return config
    learner_configs = {
        kind: replace(learner, **values)
        for kind, learner in config.learner_configs.items()
    }
    return replace(config, **values, learner_configs=learner_configs)


def _no_conflict_validation(
    event: replay.ReplayEvent, evidence: _PersistentRREvidence, address: int,
    max_insert_age: int | None, native_owner_pc: int = 0,
    native_owner_valid: bool = False,
) -> tuple[bool, str, int, bool]:
    """Resolve a no-conflict check while preserving an online RR hit exactly."""
    hit, source, owner_pc, owner_valid = evidence.hit(address, max_insert_age)
    # Full-line evidence intentionally does not model finite-tag aliases.  A
    # recorded native hit, including an old mature line, must stay a hit and
    # retain the owner reconstructed from the native RR state.
    if event.validation_hit:
        return True, "recorded", native_owner_pc, native_owner_valid
    return hit, source, owner_pc, owner_valid


def _unique_lru_validation(
    event: replay.ReplayEvent, evidence: _UniqueAddressLRUEvidence, address: int,
    native_owner_pc: int = 0, native_owner_valid: bool = False,
) -> tuple[bool, str, int, bool]:
    """Preserve native RR hits; recover only recorded misses from the LRU."""
    if event.validation_hit:
        return True, "recorded", native_owner_pc, native_owner_valid
    return evidence.hit(address)


def _unique_lru_offset_match_validation(
    event: replay.ReplayEvent, evidence: _UniqueAddressLRUEvidence, address: int,
    native_owner_pc: int = 0, native_owner_valid: bool = False,
) -> tuple[bool, str, int, bool]:
    """Admit recovered producer credit only for an offset-consistent owner.

    A native hit is still authoritative.  On a native miss, an LRU address
    hit says only that the predecessor line appeared recently.  It becomes a
    positive producer sample only when its valid producer used the same BOP
    offset as the current validation attempt; every other recovered line is
    intentionally sent through the regular controller miss path.
    """
    if event.validation_hit:
        return True, "recorded", native_owner_pc, native_owner_valid
    hit, source, owner_pc, owner_valid, owner_offset = evidence.hit_detail(address)
    if not hit:
        return False, source, owner_pc, owner_valid
    if owner_valid and owner_offset == event.best_offset_after:
        return True, "unique_lru_offset_match", owner_pc, owner_valid
    return False, "unique_lru_offset_mismatch", owner_pc, owner_valid


def replay_counterfactuals(
    connection: sqlite3.Connection, config: replay.BOPConfig,
    window: replay.EvaluationWindow, points: Iterable[str] = POINTS,
    no_conflict_max_insert_ages: Iterable[int] = (),
    unique_lru_entries: int = 2048,
) -> dict[str, object]:
    selected_points = tuple(points)
    if not selected_points or any(point not in POINTS for point in selected_points):
        raise ValueError(f"points must be a non-empty subset of {POINTS}")
    age_limits = tuple(dict.fromkeys(no_conflict_max_insert_ages))
    if any(age < 0 for age in age_limits):
        raise ValueError("no-conflict maximum insertion ages must be non-negative")
    if unique_lru_entries <= 0:
        raise ValueError("unique-address LRU entries must be positive")
    age_point_names = {
        age: f"no_conflict_age_{age}" for age in age_limits
    }
    producer_consumer = config.pc_validation_producer_consumer
    pc_supported_points = {"current", "no_conflict", *_LRU_POINTS}
    if producer_consumer and any(
            point not in pc_supported_points
            for point in selected_points):
        raise ValueError(
            "producer/consumer counterfactual replay currently supports only "
            "the current, no_conflict, and unique-LRU evidence points"
        )
    replay._shared_controller_config(config)
    configs_by_name = _configs_by_name(config)
    action_cursor = _RecordedActionCursor(connection, configs_by_name)

    persistent = (
        {name: _PersistentRREvidence(learner)
         for name, learner in configs_by_name.items()}
        if "no_conflict" in selected_points or age_limits else {}
    )
    unique_lru = (
        {name: _UniqueAddressLRUEvidence(learner, unique_lru_entries)
         for name, learner in configs_by_name.items()}
        if any(point in _LRU_POINTS for point in selected_points) else {}
    )
    no_drop = (
        {name: _NoDropRREvidence(learner, persistent=False)
         for name, learner in configs_by_name.items()}
        if "no_delay_drop" in selected_points else {}
    )
    combined = (
        {name: _NoDropRREvidence(learner, persistent=True)
         for name, learner in configs_by_name.items()}
        if "no_conflict_no_drop" in selected_points else {}
    )
    policy_points = {
        name: _CounterfactualPoint(name, _point_config(name, config), window)
        for name in selected_points
    }
    policy_points.update({
        name: _CounterfactualPoint(name, config, window, age)
        for age, name in age_point_names.items()
    })
    counters: Counter[str] = Counter()
    native_learners = (
        {
            kind: replay.BOPLearner(
                config.for_kind(kind), use_recorded_delay_actions=True,
            )
            for kind in ("large", "small")
        }
        if producer_consumer else {}
    )
    owner_verifier = replay._OnlineVerifier() if producer_consumer else None
    owner_validation_mismatches = 0

    def on_demand(demand: replay.Demand) -> None:
        counters["demands"] += 1
        if replay._in_evaluation_window(demand, window):
            counters["window_demands"] += 1
        for point in policy_points.values():
            point.observe_demand(demand)

    def on_event(event: replay.ReplayEvent) -> None:
        nonlocal owner_validation_mismatches
        counters["events"] += 1
        selected = replay._in_evaluation_window(event, window)
        if selected:
            counters["window_events"] += 1
        learner_config = configs_by_name[event.bop_name]
        before, trigger_action = action_cursor.actions_for_event(event)
        for action in before:
            if event.bop_name in persistent:
                persistent[event.bop_name].apply_dequeue(action)
            if event.bop_name in unique_lru:
                unique_lru[event.bop_name].apply_dequeue(action)
            if producer_consumer:
                native_learners[event.bop_kind].apply_delay_action(action)

        dropped = trigger_action is not None and trigger_action.action == "drop_full"
        if event.bop_name in persistent:
            persistent[event.bop_name].apply_trigger(event, trigger_action)
        if event.bop_name in unique_lru:
            unique_lru[event.bop_name].apply_trigger(event, trigger_action)
        if event.bop_name in no_drop:
            no_drop[event.bop_name].apply_trigger(event, dropped)
        if event.bop_name in combined:
            combined[event.bop_name].apply_trigger(event, dropped)

        native_output = None
        if producer_consumer:
            native_output = native_learners[event.bop_kind].process(
                event, trigger_action,
            )
            assert owner_verifier is not None
            owner_verifier.observe(event, native_output)
            if native_output.validation_hit != event.validation_hit:
                owner_validation_mismatches += 1

        if "current" in policy_points:
            policy_points["current"].emit(
                event, event.validation_hit, "recorded", selected,
                native_output.validation_owner_pc if native_output else 0,
                native_output.validation_owner_valid if native_output else False,
            )
        if _CounterfactualPoint._is_validation_check(event):
            address = _validation_address(event, learner_config)
            native_owner_pc = (
                native_output.validation_owner_pc if native_output else 0
            )
            native_owner_valid = (
                native_output.validation_owner_valid if native_output else False
            )
            if "no_conflict" in policy_points:
                hit, source, owner_pc, owner_valid = _no_conflict_validation(
                    event, persistent[event.bop_name], address, None,
                    native_owner_pc, native_owner_valid,
                )
                policy_points["no_conflict"].emit(
                    event, int(hit), source, selected, owner_pc, owner_valid,
                )
            if "unique_lru" in policy_points:
                hit, source, owner_pc, owner_valid = _unique_lru_validation(
                    event, unique_lru[event.bop_name], address,
                    native_owner_pc, native_owner_valid,
                )
                policy_points["unique_lru"].emit(
                    event, int(hit), source, selected, owner_pc, owner_valid,
                )
            for name in _RECOVERY_FACTOR_POINTS:
                if name not in policy_points:
                    continue
                hit, source, owner_pc, owner_valid = _unique_lru_validation(
                    event, unique_lru[event.bop_name], address,
                    native_owner_pc, native_owner_valid,
                )
                policy_points[name].emit(
                    event, int(hit), source, selected, owner_pc, owner_valid,
                    validation_source=(
                        "recovered" if source == "unique_lru" else "native"
                    ),
                )
            for name in _OFFSET_MATCH_LRU_POINTS:
                if name not in policy_points:
                    continue
                hit, source, owner_pc, owner_valid = (
                    _unique_lru_offset_match_validation(
                        event, unique_lru[event.bop_name], address,
                        native_owner_pc, native_owner_valid,
                    )
                )
                policy_points[name].emit(
                    event, int(hit), source, selected, owner_pc, owner_valid,
                    validation_source=(
                        "recovered"
                        if source == "unique_lru_offset_match" else "native"
                    ),
                )
            for age, name in age_point_names.items():
                hit, source, owner_pc, owner_valid = _no_conflict_validation(
                    event, persistent[event.bop_name], address, age,
                    native_owner_pc, native_owner_valid,
                )
                policy_points[name].emit(
                    event, int(hit), source, selected, owner_pc, owner_valid,
                )
            if "no_delay_drop" in policy_points:
                hit, source, owner_pc, owner_valid = no_drop[event.bop_name].hit(address)
                policy_points["no_delay_drop"].emit(
                    event, int(hit), source, selected, owner_pc, owner_valid,
                )
            if "no_conflict_no_drop" in policy_points:
                hit, source, owner_pc, owner_valid = combined[event.bop_name].hit(address)
                if event.validation_hit:
                    hit, source = True, "recorded"
                    owner_pc = native_output.validation_owner_pc if native_output else 0
                    owner_valid = (
                        native_output.validation_owner_valid
                        if native_output else False
                    )
                policy_points["no_conflict_no_drop"].emit(
                    event, int(hit), source, selected, owner_pc, owner_valid,
                )
        else:
            # Preserve the no-validation sentinel for every controller point.
            for name in (*age_point_names.values(), "no_conflict", *_LRU_POINTS,
                         "no_delay_drop", "no_conflict_no_drop"):
                if name in policy_points:
                    policy_points[name].emit(event, event.validation_hit, "none", selected)

    def on_access_end() -> None:
        for point in policy_points.values():
            point.commit()

    replay._stream_trace_rows(connection, on_demand, on_event, on_access_end)
    owner_reconstruction = None
    if owner_verifier is not None:
        owner_reconstruction = owner_verifier.report()
        owner_reconstruction["validation_hit_mismatches"] = owner_validation_mismatches
        owner_reconstruction["pass"] = (
            owner_reconstruction["pass"] and owner_validation_mismatches == 0
        )
        if not owner_reconstruction["pass"]:
            raise ValueError(
                "native P/C owner reconstruction diverged from V5 learner trace: "
                f"{owner_reconstruction}"
            )
    report = {
        "model": {
            "horizon": HORIZON,
            "raw_candidate_stream": "recorded_online",
            "best_offset_stream": "recorded_online",
            "controller": "shared_pc_global_replay",
            "no_conflict": (
                "native delay actions; every matured RR line remains present"
            ),
            "no_conflict_freshness": (
                "age-limited points preserve an online ValidationHit, but "
                "recover a recorded miss only when the target line's latest "
                "native RR maturity is at most the configured number of "
                "native RR insertions old"
            ),
            "unique_lru": (
                "native RR hits are preserved; a recorded miss is recovered only "
                "by an exact line in this BOP's bounded native-maturity LRU"
            ),
            "recovered_evidence_factors": (
                "lru_full source-tags every LRU-recovered miss; direct_only, "
                "credit_only, and same/cross-PC credit points independently "
                "control recovered admission and producer confidence updates"
            ),
            "unique_lru_entries": unique_lru_entries,
            "no_delay_drop": (
                "unbounded delay queue with native callback timing; finite "
                "direct-mapped RR remains"
            ),
            "no_conflict_no_drop": (
                "unbounded delay queue plus persistent matured RR presence"
            ),
            "not_modeled": "local filters, cache fills, MSHRs, residency, bandwidth",
        },
        "controller_parameters": replay._controller_parameter_report(config),
        "no_conflict_max_insert_ages": list(age_limits),
        "evaluation_window": {
            **asdict(window),
            "state_replay": "full_trace",
            "demand_oracle": "selected_window_only",
            "demands": int(counters["window_demands"]),
            "events": int(counters["window_events"]),
        },
        "demands": int(counters["demands"]),
        "events": int(counters["events"]),
        "owner_reconstruction": owner_reconstruction,
        "points": {name: point.report() for name, point in policy_points.items()},
        "evidence_state": {
            "no_conflict": {
                name: evidence.stats() for name, evidence in persistent.items()
            },
            "no_delay_drop": {
                name: evidence.stats() for name, evidence in no_drop.items()
            },
            "unique_lru": {
                name: evidence.stats() for name, evidence in unique_lru.items()
            },
            "no_conflict_no_drop": {
                name: evidence.stats() for name, evidence in combined.items()
            },
        },
    }
    return report


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("database", type=Path, help="certified V5 BOP trace DB")
    parser.add_argument(
        "--controller-config", type=Path, default=None,
        help="shared controller JSON override; default uses trace metadata",
    )
    parser.add_argument(
        "--unique-lru-entries", type=int, default=2048,
        help=(
            "capacity of each BOP's exact unique-address LRU used only to "
            "recover a recorded RR miss"
        ),
    )
    parser.add_argument(
        "--evaluation-phase", default="stable",
        help="V5 phase used for reporting; full state is always replayed",
    )
    parser.add_argument(
        "--points", nargs="+", choices=POINTS, default=list(POINTS),
        help="counterfactual points to run; default runs all configured points",
    )
    parser.add_argument(
        "--no-conflict-max-insert-age", nargs="+", type=int, default=[],
        metavar="N",
        help=(
            "add no_conflict_age_N points; an online hit is preserved, while "
            "a recovered mature line must be at most N native RR insertions old"
        ),
    )
    parser.add_argument("--output", type=Path, default=None)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    start = time.perf_counter()
    with sqlite3.connect(args.database) as connection:
        config, phases = replay._streaming_metadata(connection)
        if args.controller_config is not None:
            config = replay._load_controller_overrides(args.controller_config, config)
        window = replay.resolve_evaluation_window(
            phase_name=args.evaluation_phase,
            start_tick=None,
            stats_path=None,
            stats_block=None,
            phases=phases,
        )
        report = replay_counterfactuals(
            connection, config, window, args.points,
            args.no_conflict_max_insert_age, args.unique_lru_entries,
        )
    report["database"] = str(args.database)
    report["schema_version"] = config.schema_version
    report["replay_engine"] = {
        "engine": "streaming_pc_validation_counterfactual",
        "wall_seconds": time.perf_counter() - start,
    }
    encoded = json.dumps(report, indent=2, sort_keys=True)
    if args.output is None:
        print(encoded)
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
