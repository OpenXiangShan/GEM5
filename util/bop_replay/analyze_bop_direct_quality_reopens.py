#!/usr/bin/env python3
"""Attribute post-reopen quality of the causal direct BOP-quality gate.

This is an offline diagnostic for a fixed direct-quality-gate profile.  It
does not change the controller or use any future demand result for admission.
Every ``BLOCK -> OPEN`` (or ``RECOVER -> OPEN``) transition receives an epoch
identifier.  Stable-window candidates issued while that exact physical
PC-kind-offset context remains OPEN are attributed to the epoch and later
labeled by the same Horizon demand oracle as the controller itself.

The report separates the sample evidence that unlocked each epoch from both
its first N issued candidates and the complete retained OPEN interval.  This
answers whether sparse direct feedback is temporarily optimistic for a
particular PC-kind context before adding another hardware state.
"""

from __future__ import annotations

import argparse
import json
import resource
import sqlite3
import time
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

import bop_replay as replay
from replay_bop_direct_quality_gate import (
    DirectQualityConfig,
    DirectQualityReplay,
    _FeedbackEntry,
    _IssuedCandidate,
)


OUTCOMES = ("useful", "unused", "redundant", "censored")


def _empty_counts() -> Counter[str]:
    return Counter({"candidates": 0, **{outcome: 0 for outcome in OUTCOMES}})


def _quality(counts: Counter[str]) -> dict[str, object]:
    denominator = counts["useful"] + counts["unused"] + counts["redundant"]
    return {
        "candidates": counts["candidates"],
        "useful": counts["useful"],
        "unused": counts["unused"],
        "redundant": counts["redundant"],
        "censored": counts["censored"],
        "accuracy": counts["useful"] / denominator if denominator else None,
    }


@dataclass
class _OpenEpoch:
    epoch_id: int
    pc: int
    kind: str
    key: tuple[int, int, int, int]
    transition: str
    unlock_status: str
    unlock_demand_index: int
    sampled_useful_after_unlock: int
    sampled_unused_after_unlock: int
    issued: Counter[str] = field(default_factory=_empty_counts)
    window_issued: dict[int, Counter[str]] = field(default_factory=dict)

    def note_issued(self, windows: Iterable[int]) -> tuple[int, ...]:
        self.issued["candidates"] += 1
        ordinal = self.issued["candidates"]
        memberships = []
        for window in windows:
            if ordinal <= window:
                self.window_issued[window]["candidates"] += 1
                memberships.append(window)
        return tuple(memberships)

    def note_outcome(self, status: str, windows: Iterable[int]) -> None:
        if status not in OUTCOMES:
            raise ValueError(f"unexpected direct candidate outcome {status!r}")
        self.issued[status] += 1
        for window in windows:
            self.window_issued[window][status] += 1

    def report(self, windows: Iterable[int]) -> dict[str, object]:
        return {
            "epoch_id": self.epoch_id,
            "issuer_trigger_pc": hex(self.pc),
            "bop_kind": self.kind,
            "transition": self.transition,
            "unlock_status": self.unlock_status,
            "unlock_demand_index": self.unlock_demand_index,
            "sampled_useful_after_unlock": self.sampled_useful_after_unlock,
            "sampled_unused_after_unlock": self.sampled_unused_after_unlock,
            "full_open_interval": _quality(self.issued),
            "first_issued_candidates": {
                str(window): _quality(self.window_issued[window])
                for window in windows
            },
        }


class ReopenAuditReplay(DirectQualityReplay):
    """Direct-quality replay with observer-only causal reopen attribution."""

    def __init__(
        self, config: DirectQualityConfig, window: replay.EvaluationWindow,
        candidate_windows: Iterable[int],
    ):
        super().__init__(config, window)
        self.candidate_windows = tuple(sorted(set(candidate_windows)))
        self.epochs: list[_OpenEpoch] = []
        self.active_epochs: dict[tuple[int, int, int, int], int] = {}
        self.candidate_epochs: dict[int, tuple[int, tuple[int, ...]]] = {}

    def _context_key(
        self, event: replay.ReplayEvent,
    ) -> tuple[int, int, int, int] | None:
        """Read the context just created/looked up by the parent event path."""
        context = self._context(event)
        if context is None:
            return None
        set_index, tag = self.controller._key(*context)
        base = set_index * self.config.quality_ways
        for way in range(self.config.quality_ways):
            entry_index = base + way
            entry = self.controller.entries[entry_index]
            if not entry.valid or entry.tag != tag:
                continue
            contexts = self.controller.offset_contexts[entry_index]
            if self.config.offset_context_slots == 1:
                candidate_contexts = enumerate(contexts[:1])
            else:
                candidate_contexts = enumerate(contexts)
            for context_index, offset_context in candidate_contexts:
                if (offset_context.valid
                        and (self.config.offset_context_slots == 1
                             or offset_context.offset == event.best_offset_after)):
                    return (
                        entry_index, entry.generation, context_index,
                        offset_context.generation,
                    )
        return None

    def observe_event(self, event: replay.ReplayEvent) -> None:
        candidate_id_before = self.next_candidate_id
        super().observe_event(event)
        selected = replay._in_evaluation_window(event, self.window)
        # One raw candidate is always allocated first.  A second allocation
        # means the parent admitted the direct candidate; no replay state is
        # changed by this observer path.
        if (not selected or not event.raw_candidate_valid
                or self.next_candidate_id != candidate_id_before + 2):
            return
        key = self._context_key(event)
        if key is None:
            return
        epoch_id = self.active_epochs.get(key)
        if epoch_id is None:
            return
        offset_context = self.controller.offset_contexts[key[0]][key[2]]
        if offset_context.state != "open":
            return
        memberships = self.epochs[epoch_id].note_issued(self.candidate_windows)
        self.candidate_epochs[candidate_id_before + 1] = (epoch_id, memberships)

    def _resolve_feedback(self, entry: _FeedbackEntry, status: str) -> None:
        """Retain the parent's update semantics and observe its transition."""
        context_record = self.candidate_sample_context.pop(entry.candidate_id, None)
        if context_record is None:
            self.stats["feedback_untracked_candidate"] += 1
            return
        candidate_context, selected = context_record
        if status == "useful":
            if selected:
                self.context_counts[candidate_context].samples_useful += 1
        elif status == "unused":
            if selected:
                self.context_counts[candidate_context].samples_unused += 1
        elif status == "redundant":
            if selected:
                self.context_counts[candidate_context].samples_redundant += 1
            return
        else:
            if selected:
                self.context_counts[candidate_context].samples_censored += 1
            return

        entry_live = (
            0 <= entry.quality_index < len(self.controller.entries)
            and self.controller.entries[entry.quality_index].valid
            and self.controller.entries[entry.quality_index].generation
            == entry.quality_generation
            and 0 <= entry.offset_context_index < self.config.offset_context_slots
        )
        before, after = self.controller.note_sample(
            entry.quality_index, entry.quality_generation, status,
            entry.offset_context_index, entry.offset_context_generation,
            entry.recovery_generation, entry.audit_generation,
        )
        if not entry_live:
            return
        offset_context = self.controller.offset_contexts[
            entry.quality_index
        ][entry.offset_context_index]
        if (not offset_context.valid
                or offset_context.generation != entry.offset_context_generation):
            return
        key = (
            entry.quality_index, entry.quality_generation,
            entry.offset_context_index, entry.offset_context_generation,
        )
        if before in ("block", "recover") and after == "open":
            epoch_id = len(self.epochs)
            previous = self.active_epochs.get(key)
            if previous is not None:
                self.active_epochs.pop(key)
            self.epochs.append(_OpenEpoch(
                epoch_id=epoch_id,
                pc=candidate_context[0],
                kind=candidate_context[1],
                key=key,
                transition=f"{before}_to_open",
                unlock_status=status,
                unlock_demand_index=self.feedback_demand_index,
                sampled_useful_after_unlock=offset_context.useful,
                sampled_unused_after_unlock=offset_context.unused,
                window_issued={
                    candidate_window: _empty_counts()
                    for candidate_window in self.candidate_windows
                },
            ))
            self.active_epochs[key] = epoch_id
        elif before == "open" and after != "open":
            self.active_epochs.pop(key, None)

    def _resolve_direct_combined(
        self, candidate: _IssuedCandidate, status: str,
    ) -> None:
        super()._resolve_direct_combined(candidate, status)
        epoch_record = self.candidate_epochs.pop(candidate.candidate_id, None)
        if epoch_record is None:
            return
        epoch_id, memberships = epoch_record
        self.epochs[epoch_id].note_outcome(status, memberships)

    def _aggregate_epoch_quality(self, window: int | None = None) -> Counter[str]:
        aggregate = _empty_counts()
        for epoch in self.epochs:
            source = epoch.issued if window is None else epoch.window_issued[window]
            aggregate.update(source)
        return aggregate

    def finish(self, top: int) -> dict[str, object]:
        base = super().finish()
        epoch_rows = [epoch.report(self.candidate_windows) for epoch in self.epochs]
        epoch_rows.sort(
            key=lambda row: (
                -int(row["full_open_interval"]["candidates"]),
                str(row["issuer_trigger_pc"]), str(row["bop_kind"]),
                int(row["epoch_id"]),
            ),
        )
        by_context: dict[tuple[str, str], Counter[str]] = defaultdict(_empty_counts)
        for epoch in self.epochs:
            by_context[(hex(epoch.pc), epoch.kind)].update(epoch.issued)
        contexts = [
            {
                "issuer_trigger_pc": pc,
                "bop_kind": kind,
                "post_reopen_open_quality": _quality(counts),
            }
            for (pc, kind), counts in by_context.items()
        ]
        contexts.sort(
            key=lambda row: (
                -int(row["post_reopen_open_quality"]["candidates"]),
                str(row["issuer_trigger_pc"]), str(row["bop_kind"]),
            ),
        )
        return {
            "controller": "causal_sampled_direct_candidate_quality_reopen_audit",
            "model_contract": {
                "admission": "unchanged causal direct-quality gate",
                "oracle": "later selected-window L2 demand within Horizon",
                "epoch": "physical PC-kind-offset context BLOCK/RECOVER to OPEN transition",
                "omissions": (
                    "local filters, cache residency, fills, MSHRs, DRAM, and "
                    "bandwidth are not modeled"
                ),
            },
            "candidate_windows": list(self.candidate_windows),
            "baseline": base,
            "reopen_epochs": {
                "count": len(self.epochs),
                "full_open_quality": _quality(self._aggregate_epoch_quality()),
                "first_issued_candidate_quality": {
                    str(window): _quality(self._aggregate_epoch_quality(window))
                    for window in self.candidate_windows
                },
                "contexts": contexts[:top],
                "epochs": epoch_rows[:top],
            },
        }


def _load_config(path: Path) -> DirectQualityConfig:
    payload = json.loads(path.read_text())
    if not isinstance(payload, dict):
        raise ValueError("direct-quality config must be a JSON object")
    fields = set(DirectQualityConfig.__dataclass_fields__)
    unknown = sorted(set(payload) - fields)
    if unknown:
        raise ValueError("unknown direct-quality config fields: " + ", ".join(unknown))
    return DirectQualityConfig(**payload)


def _parse_windows(value: str) -> tuple[int, ...]:
    windows = tuple(sorted({int(item) for item in value.split(",") if item}))
    if not windows or any(window <= 0 for window in windows):
        raise argparse.ArgumentTypeError("candidate windows must be positive integers")
    return windows


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("database", type=Path, help="certified V5 BOP trace DB")
    parser.add_argument("--config", type=Path, required=True, help="direct-quality JSON")
    parser.add_argument("--evaluation-phase", default="stable")
    parser.add_argument("--candidate-windows", type=_parse_windows, default=(256, 1024, 4096))
    parser.add_argument("--top", type=int, default=30)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.top <= 0:
        raise ValueError("--top must be positive")
    config = _load_config(args.config)
    start_time = time.perf_counter()
    with sqlite3.connect(args.database) as connection:
        _, phases = replay._streaming_metadata(connection)
        window = replay.resolve_evaluation_window(
            phase_name=args.evaluation_phase,
            start_tick=None,
            stats_path=None,
            stats_block=1,
            phases=phases,
        )
        runner = ReopenAuditReplay(config, window, args.candidate_windows)
        replay._stream_trace_rows(
            connection, runner.observe_demand, runner.observe_event, lambda: None,
        )
        report = runner.finish(args.top)
    report["database"] = str(args.database)
    report["evaluation_window"] = {
        "name": window.name,
        "source": window.source,
        "phase_id": window.phase_id,
        "phase_name": window.phase_name,
        "start_tick": window.start_tick,
        "state_replay": "full_trace",
        "demand_oracle": "selected_window_only",
    }
    report["execution"] = {
        "wall_seconds": time.perf_counter() - start_time,
        "peak_rss_kib": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
