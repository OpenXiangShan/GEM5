#!/usr/bin/env python3
"""Attribute BOP RR-validation misses from a certified V5 trace.

The online BOP RR is a 256-entry direct-mapped table.  A recorded validation
miss alone cannot say whether its lookup line has never appeared before or
was evicted by a conflicting RR insertion.  This tool replays the V5 native
delay-action order, maintains a shadow history beside the exact learner, and
reports the cause at every validation lookup.
"""

from __future__ import annotations

import argparse
import json
import sqlite3
import time
from collections import Counter, defaultdict, deque
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterator, Mapping

import bop_replay as replay


@dataclass
class PendingInsertion:
    line: int
    access_seq: int
    tick: int


@dataclass
class RRRecord:
    line: int
    tag: int
    insert_ordinal: int
    source_access_seq: int
    source_tick: int
    eviction_ordinal: int | None = None
    eviction_access_seq: int | None = None
    eviction_tick: int | None = None
    evictor_line: int | None = None


class RRShadow:
    """Direct-mapped RR plus enough offline history to explain misses."""

    def __init__(self, config: replay.BOPConfig):
        self.config = config
        self.slots: list[RRRecord | None] = [None] * config.rr_entries
        self.history: dict[int, RRRecord] = {}
        self.delay_queue: deque[PendingInsertion] = deque()
        self.queued_lines: Counter[int] = Counter()
        self.dropped_lines: Counter[int] = Counter()
        self.trigger_lines: set[int] = set()
        self.rr_insertions = 0
        self.slot_replacements = 0
        self.same_line_refreshes = 0
        self.delay_enqueues = 0
        self.delay_drop_full = 0
        self.delay_dequeues = 0

    def line(self, addr: int) -> int:
        return addr & ~(self.config.block_size - 1)

    def hash(self, line: int) -> int:
        line_number = line // self.config.block_size
        index_bits = self.config.rr_entries.bit_length() - 1
        mask = self.config.rr_entries - 1
        return (line_number & mask) ^ ((line_number >> index_bits) & mask)

    def tag(self, line: int) -> int:
        line_number = line // self.config.block_size
        index_bits = self.config.rr_entries.bit_length() - 1
        return (line_number >> index_bits) & ((1 << self.config.tag_bits) - 1)

    def apply_dequeue(self, action: replay.ReplayDelayAction) -> None:
        if action.action != "dequeue_to_rr":
            raise ValueError(f"unexpected non-dequeue action {action.action!r}")
        if not self.delay_queue:
            raise ValueError("delay dequeue on an empty RR shadow queue")
        pending = self.delay_queue.popleft()
        if pending.line != self.line(action.addr):
            raise ValueError("delay dequeue address does not match RR shadow head")
        self.queued_lines[pending.line] -= 1
        if self.queued_lines[pending.line] == 0:
            del self.queued_lines[pending.line]
        if len(self.delay_queue) != action.queue_size_after:
            raise ValueError("delay dequeue queue size does not match trace")
        self.delay_dequeues += 1
        self._insert(pending)

    def apply_trigger(
        self, event: replay.ReplayEvent,
        action: replay.ReplayDelayAction | None,
    ) -> None:
        pending = PendingInsertion(
            self.line(event.trigger_addr), event.access_seq, event.tick,
        )
        self.trigger_lines.add(pending.line)
        if not self.config.delay_queue_enabled:
            if action is not None:
                raise ValueError("non-delayed RR received a delay action")
            self._insert(pending)
            return
        if action is None:
            raise ValueError("delayed RR trigger is missing a V5 delay action")
        if self.line(action.addr) != pending.line:
            raise ValueError("delay trigger action address does not match event")
        if action.action == "enqueue":
            self.delay_queue.append(pending)
            self.queued_lines[pending.line] += 1
            self.delay_enqueues += 1
        elif action.action == "drop_full":
            if len(self.delay_queue) != self.config.delay_queue_size:
                raise ValueError("drop_full did not occur on a full delay queue")
            self.dropped_lines[pending.line] += 1
            self.delay_drop_full += 1
        else:
            raise ValueError(
                f"unexpected trigger delay action {action.action!r}"
            )
        if len(self.delay_queue) != action.queue_size_after:
            raise ValueError("delay trigger queue size does not match trace")

    def _insert(self, pending: PendingInsertion) -> None:
        self.rr_insertions += 1
        line = pending.line
        index = self.hash(line)
        previous = self.slots[index]
        if previous is not None and previous.line != line:
            previous.eviction_ordinal = self.rr_insertions
            previous.eviction_access_seq = pending.access_seq
            previous.eviction_tick = pending.tick
            previous.evictor_line = line
            self.slot_replacements += 1
        elif previous is not None:
            self.same_line_refreshes += 1

        record = RRRecord(
            line=line,
            tag=self.tag(line),
            insert_ordinal=self.rr_insertions,
            source_access_seq=pending.access_seq,
            source_tick=pending.tick,
        )
        self.slots[index] = record
        self.history[line] = record

    def hit(self, line: int) -> bool:
        """Match BOP::testRR(), including the untouched Right-bank tag-zero alias."""
        slot = self.slots[self.hash(line)]
        if slot is not None and slot.tag == self.tag(line):
            return True
        return self.tag(line) == 0

    def miss_cause(self, line: int, prior_demand_lines: set[int]) -> str:
        if self.hit(line):
            return "hit"
        if line in self.history:
            record = self.history[line]
            if record.eviction_ordinal is None:
                raise AssertionError("RR shadow miss has a non-evicted record")
            return "conflict_replaced"
        if self.queued_lines.get(line, 0):
            return "delay_pending"
        if self.dropped_lines.get(line, 0):
            return "delay_drop_full"
        if line in self.trigger_lines:
            return "prior_trigger_no_rr_insert"
        if line in prior_demand_lines:
            return "prior_demand_no_rr_insert"
        return "no_prior_demand"

    def conflict_record(self, line: int) -> RRRecord:
        record = self.history.get(line)
        if record is None or record.eviction_ordinal is None:
            raise AssertionError("conflict record requested for non-conflict miss")
        return record


class DelayActionCursor:
    """One ordered V5 delay-action cursor per BOP instance."""

    def __init__(self, connection: sqlite3.Connection, bop_names: set[str]):
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

    def _advance(self, bop_name: str) -> replay.ReplayDelayAction:
        row = self.next_rows[bop_name]
        if row is None:
            raise ValueError(f"{bop_name}: delay-action stream ended early")
        self.next_rows[bop_name] = next(self.cursors[bop_name], None)
        return replay._stream_delay_action_from_row(row)

    def actions_for_event(
        self, event: replay.ReplayEvent, learner: replay.BOPLearner,
        shadow: RRShadow,
    ) -> replay.ReplayDelayAction | None:
        bop_name = event.bop_name
        row = self.next_rows[bop_name]
        while row is not None and int(row["ReplayOrder"]) < event.replay_order:
            action = self._advance(bop_name)
            if action.action != "dequeue_to_rr":
                raise ValueError(
                    f"{bop_name}: unexpected {action.action!r} before trigger"
                )
            shadow.apply_dequeue(action)
            learner.apply_delay_action(action)
            row = self.next_rows[bop_name]
        if row is not None and int(row["ReplayOrder"]) == event.replay_order:
            return self._advance(bop_name)
        return None


def _bucket(value: int) -> str:
    if value <= 0:
        return "0"
    low = 1 << (value.bit_length() - 1)
    return f"{low}-{(low << 1) - 1}"


def _counter_payload(counter: Counter[str]) -> dict[str, int | float]:
    checks = counter["validation_checks"]
    misses = counter["misses"]
    payload: dict[str, int | float] = {
        key: int(counter[key])
        for key in (
            "validation_checks",
            "hits",
            "misses",
            "conflict_replaced",
            "delay_pending",
            "delay_drop_full",
            "prior_trigger_no_rr_insert",
            "prior_demand_no_rr_insert",
            "no_prior_demand",
        )
    }
    payload["hit_rate"] = counter["hits"] / checks if checks else 0.0
    payload["conflict_fraction_of_misses"] = (
        counter["conflict_replaced"] / misses if misses else 0.0
    )
    payload["not_inserted_fraction_of_misses"] = (
        (misses - counter["conflict_replaced"]) / misses if misses else 0.0
    )
    return payload


class RRMissAnalyzer:
    """Aggregate selected-window miss causes while replaying full RR state."""

    def __init__(self, config: replay.BOPConfig, top: int):
        self.config = config
        self.shadow = RRShadow(config)
        self.top = top
        self.total: Counter[str] = Counter()
        self.by_pc: dict[int, Counter[str]] = defaultdict(Counter)
        self.by_offset: dict[int, Counter[str]] = defaultdict(Counter)
        self.by_slot: dict[int, Counter[str]] = defaultdict(Counter)
        self.evictor_lines: Counter[int] = Counter()
        self.insert_distance: Counter[str] = Counter()
        self.access_distance: Counter[str] = Counter()
        self.tick_distance: Counter[str] = Counter()
        self.verification_total = 0
        self.verification_mismatches = 0
        self.first_mismatch: dict[str, object] | None = None

    @staticmethod
    def _update(counter: Counter[str], cause: str) -> None:
        counter["validation_checks"] += 1
        if cause == "hit":
            counter["hits"] += 1
        else:
            counter["misses"] += 1
            counter[cause] += 1

    def _validation_addr(self, output: replay.LearnerOutput) -> int:
        return (
            output.event.trigger_addr
            - output.best_offset_after * self.config.block_size
        ) & replay.UINT64_MASK

    def observe(
        self, event: replay.ReplayEvent, output: replay.LearnerOutput,
        selected: bool, prior_demand_lines: set[int],
    ) -> None:
        self.verification_total += 1
        shadow_hit = -1
        validation_addr = self._validation_addr(output)
        if output.raw_candidate_valid and output.validation_enabled:
            shadow_hit = int(self.shadow.hit(validation_addr))
        if shadow_hit != output.validation_hit or output.validation_hit != event.validation_hit:
            self.verification_mismatches += 1
            if self.first_mismatch is None:
                self.first_mismatch = {
                    "access_seq": event.access_seq,
                    "bop_name": event.bop_name,
                    "recorded_validation_hit": event.validation_hit,
                    "learner_validation_hit": output.validation_hit,
                    "shadow_validation_hit": shadow_hit,
                    "validation_addr": hex(validation_addr),
                }

        if not selected or output.validation_hit not in (0, 1):
            return

        line = self.shadow.line(validation_addr)
        cause = self.shadow.miss_cause(line, prior_demand_lines)
        if output.validation_hit == 1 and cause != "hit":
            raise AssertionError("recorded RR hit classified as a shadow miss")
        if output.validation_hit == 0 and cause == "hit":
            raise AssertionError("recorded RR miss classified as a shadow hit")

        self._update(self.total, cause)
        self._update(self.by_pc[event.trigger_pc], cause)
        self._update(self.by_offset[output.best_offset_after], cause)
        self._update(self.by_slot[self.shadow.hash(line)], cause)
        if cause != "conflict_replaced":
            return

        record = self.shadow.conflict_record(line)
        assert record.eviction_ordinal is not None
        assert record.evictor_line is not None
        self.evictor_lines[record.evictor_line] += 1
        self.insert_distance[_bucket(
            self.shadow.rr_insertions - record.insert_ordinal
        )] += 1
        self.access_distance[_bucket(event.access_seq - record.source_access_seq)] += 1
        self.tick_distance[_bucket(event.tick - record.source_tick)] += 1

    @staticmethod
    def _top_groups(
        groups: Mapping[int, Counter[str]], name: str, top: int,
        format_key,
    ) -> list[dict[str, object]]:
        ordered = sorted(
            groups.items(),
            key=lambda item: (
                item[1]["misses"], item[1]["conflict_replaced"], item[0],
            ),
            reverse=True,
        )[:top]
        return [
            {name: format_key(key), **_counter_payload(counter)}
            for key, counter in ordered
        ]

    def report(self) -> dict[str, object]:
        verification = {
            "total_events": self.verification_total,
            "mismatched_events": self.verification_mismatches,
            "first_mismatch": self.first_mismatch,
            "pass": self.verification_mismatches == 0,
        }
        return {
            "rr_parameters": {
                "entries": self.config.rr_entries,
                "tag_bits": self.config.tag_bits,
                "block_size": self.config.block_size,
                "delay_queue_enabled": self.config.delay_queue_enabled,
                "delay_queue_size": self.config.delay_queue_size,
                "delay_ticks": self.config.delay_ticks,
            },
            "verification": verification,
            "validation": _counter_payload(self.total),
            "rr_history": {
                "rr_insertions": self.shadow.rr_insertions,
                "distinct_inserted_lines": len(self.shadow.history),
                "slot_replacements": self.shadow.slot_replacements,
                "same_line_refreshes": self.shadow.same_line_refreshes,
                "delay_enqueues": self.shadow.delay_enqueues,
                "delay_drop_full": self.shadow.delay_drop_full,
                "delay_dequeues": self.shadow.delay_dequeues,
            },
            "top_trigger_pcs": self._top_groups(
                self.by_pc, "trigger_pc", self.top, lambda pc: hex(pc),
            ),
            "top_best_offsets": self._top_groups(
                self.by_offset, "best_offset", self.top, int,
            ),
            "top_rr_slots": self._top_groups(
                self.by_slot, "rr_slot", self.top, int,
            ),
            "top_evictor_lines": [
                {"line": hex(line), "conflict_misses": count}
                for line, count in self.evictor_lines.most_common(self.top)
            ],
            "conflict_reuse_distance": {
                "rr_insertions_since_target_insert": dict(
                    sorted(self.insert_distance.items())
                ),
                "access_seq_since_target_insert": dict(
                    sorted(self.access_distance.items())
                ),
                "ticks_since_target_insert": dict(
                    sorted(self.tick_distance.items())
                ),
            },
        }


def _stream_events(
    connection: sqlite3.Connection, config: replay.BOPConfig,
    window: replay.EvaluationWindow, top: int,
) -> dict[str, RRMissAnalyzer]:
    block_sizes = {
        learner_config.block_size
        for learner_config in config.learner_configs.values()
    }
    if len(block_sizes) != 1:
        raise ValueError("RR analysis requires a common Large/Small block size")
    block_size = next(iter(block_sizes))
    analyzers = {
        kind: RRMissAnalyzer(config.for_kind(kind), top)
        for kind in ("large", "small")
    }
    learners = {
        kind: replay.BOPLearner(
            config.for_kind(kind), use_recorded_delay_actions=True,
        )
        for kind in ("large", "small")
    }
    bop_names = {learner.config.bop_name for learner in learners.values()}
    action_cursor = DelayActionCursor(connection, bop_names)
    demand_rows = iter(connection.execute(
        "SELECT AccessSeq,Addr FROM L2DemandTrace ORDER BY AccessSeq"
    ))
    event_rows = iter(connection.execute(
        "SELECT rowid AS TraceOrder,* FROM BOPReplayEvent "
        "ORDER BY AccessSeq,rowid"
    ))
    demand_row = next(demand_rows, None)
    prior_demand_lines: set[int] = set()

    for row in event_rows:
        event = replay._stream_event_from_row(row)
        while demand_row is not None and int(demand_row["AccessSeq"]) <= event.access_seq:
            address = replay._sqlite_u64(demand_row["Addr"])
            prior_demand_lines.add(address & ~(block_size - 1))
            demand_row = next(demand_rows, None)

        learner = learners[event.bop_kind]
        analyzer = analyzers[event.bop_kind]
        trigger_action = action_cursor.actions_for_event(
            event, learner, analyzer.shadow,
        )
        analyzer.shadow.apply_trigger(event, trigger_action)
        output = learner.process(event, trigger_action)
        analyzer.observe(
            event, output, replay._in_evaluation_window(event, window),
            prior_demand_lines,
        )

    return analyzers


def analyze_trace(
    database: Path, evaluation_phase: str = "stable", top: int = 20,
) -> dict[str, object]:
    if top <= 0:
        raise ValueError("top must be positive")
    with sqlite3.connect(database) as connection:
        connection.row_factory = sqlite3.Row
        config, phases = replay._streaming_metadata(connection)
        if config.schema_version != replay.SCHEMA_VERSION:
            raise ValueError("RR miss analysis requires schema V5 trace data")
        window = replay.resolve_evaluation_window(
            phase_name=evaluation_phase,
            start_tick=None,
            stats_path=None,
            stats_block=None,
            phases=phases,
        )
        start_time = time.perf_counter()
        analyzers = _stream_events(connection, config, window, top)
        wall_seconds = time.perf_counter() - start_time

    result = {
        "database": str(database),
        "schema_version": config.schema_version,
        "evaluation_window": asdict(window),
        "replay_engine": {
            "engine": "streaming_rr_shadow",
            "wall_seconds": wall_seconds,
        },
        "bops": {kind: analyzer.report() for kind, analyzer in analyzers.items()},
    }
    result["verification"] = {
        "total_events": sum(
            analyzer.verification_total for analyzer in analyzers.values()
        ),
        "mismatched_events": sum(
            analyzer.verification_mismatches for analyzer in analyzers.values()
        ),
    }
    result["verification"]["pass"] = (
        result["verification"]["mismatched_events"] == 0
    )
    return result


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Attribute BOP RR validation misses from a V5 trace"
    )
    parser.add_argument("database", type=Path)
    parser.add_argument("--evaluation-phase", default="stable")
    parser.add_argument("--top", type=int, default=20)
    parser.add_argument("--output", type=Path, default=None)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    report = analyze_trace(args.database, args.evaluation_phase, args.top)
    encoded = json.dumps(report, indent=2, sort_keys=True)
    if args.output is None:
        print(encoded)
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded + "\n")
    return 0 if report["verification"]["pass"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
