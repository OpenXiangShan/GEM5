#!/usr/bin/env python3
"""Certify an online DirectQualityGate raw-candidate trace.

The certification stream deliberately stops at DirectQualityGate.  It neither
models nor reads local-filter, PFQ, translation, cache, or bandwidth state.
For every recorded raw BOP candidate and L2 read demand, it reconstructs the
bounded feedback ledger and compares the resulting causal event stream with
the trace that GEM5 recorded online.
"""

from __future__ import annotations

import argparse
import json
import sqlite3
import sys
from collections import deque
from dataclasses import asdict
from pathlib import Path
from typing import Any

from replay_bop_direct_quality_gate import (
    DirectQualityConfig,
    DirectQualityController,
    FEEDBACK_ADDRESS_LAYOUT_LEGACY,
    FEEDBACK_ADDRESS_LAYOUT_SV48,
    FEEDBACK_ADDRESS_LAYOUT_SV48_TRUNCATED,
    FEEDBACK_AGE_ENCODING_EPOCH6,
    FEEDBACK_AGE_ENCODING_FULL,
    FEEDBACK_EXPIRY_MODE_HEAP,
    FEEDBACK_EXPIRY_MODE_ROUND_ROBIN,
    FEEDBACK_OWNER_LAYOUT_QUALITY_KEY,
    FEEDBACK_OWNER_LAYOUT_SLOT_GENERATION,
    QUALITY_HASH_LAYOUT_MIX64,
    QUALITY_HASH_LAYOUT_XOR_FOLD,
    SampledFeedbackTable,
    _FeedbackEntry,
)


UINT64_MASK = (1 << 64) - 1
STATE_VALUES = {"observe": 0, "open": 1, "block": 2, "recover": 3}
# DirectQuality is also usable without the legacy PC-validation pair.  In
# that configuration BOP keeps the default Generic kind (0); it must remain a
# distinct hash namespace from the Large/Small shared-table kinds.
KIND_NAMES = {0: "generic", 1: "large", 2: "small"}


def _u64(value: int) -> int:
    return int(value) & UINT64_MASK


def _row_value(row: sqlite3.Row, name: str) -> int:
    return _u64(int(row[name]))


class RawTraceReplay:
    """Offline reconstruction of a V3/V4/V5 raw DirectQuality event stream."""

    def __init__(self, config: DirectQualityConfig, on_event: Any):
        self.config = config
        self.controller = DirectQualityController(config)
        self.on_event = on_event
        self.next_event_sequence = 0
        self.next_feedback_id = 0
        self.demand_sequence = 0
        self.event_count = 0
        self.feedback_kinds: dict[int, int] = {}
        self.feedback = SampledFeedbackTable(
            config, self._resolve_feedback, self._drop_feedback,
        )

    def _append(self, event_type: str, **values: Any) -> None:
        self.next_event_sequence += 1
        self.event_count += 1
        self.on_event({
            "event_sequence": self.next_event_sequence,
            "type": event_type,
            **values,
        })

    def _owner_is_current(self, entry: _FeedbackEntry) -> bool:
        if self.config.feedback_owner_layout == FEEDBACK_OWNER_LAYOUT_QUALITY_KEY:
            return self.controller.lookup_owner_key(
                entry.quality_set, entry.quality_tag, entry.quality_kind,
            ) is not None
        root = self.controller.entries[entry.quality_index]
        return root.valid and root.generation == entry.quality_generation

    def _drop_feedback(self, entry: _FeedbackEntry, reason: str) -> None:
        # Horizon expiry is a real negative label when the quality owner is
        # still live.  Only replacement/conflict drops are unknown labels.
        if reason in (
                "feedback_expired_without_label",
                "feedback_sweep_expired_without_label"):
            if self._owner_is_current(entry):
                self._resolve_feedback(entry, "unused")
            else:
                self._append(
                    "outcome",
                    feedback_id=entry.candidate_id,
                    resolve_demand_sequence=self.demand_sequence,
                    line=entry.line,
                    outcome="unknown_owner_replaced",
                )
            return
        self._append(
            "outcome",
            feedback_id=entry.candidate_id,
            resolve_demand_sequence=self.demand_sequence,
            line=entry.line,
            outcome="unknown_feedback_replacement",
        )

    def _resolve_feedback(self, entry: _FeedbackEntry, status: str) -> None:
        if self.config.feedback_owner_layout == FEEDBACK_OWNER_LAYOUT_QUALITY_KEY:
            owner = self.controller.lookup_owner_key(
                entry.quality_set, entry.quality_tag, entry.quality_kind,
            )
            if owner is None:
                self._append(
                    "outcome",
                    feedback_id=entry.candidate_id,
                    resolve_demand_sequence=self.demand_sequence,
                    line=entry.line,
                    outcome="unknown_owner_replaced",
                )
                return
            self.controller.note_sample(
                owner.index, owner.generation, status,
                owner.context_index, owner.context_generation,
                owner.recovery_generation, owner.audit_generation,
            )
            self._append(
                "outcome",
                feedback_id=entry.candidate_id,
                resolve_demand_sequence=self.demand_sequence,
                line=entry.line,
                outcome=status,
            )
            return

        if not self._owner_is_current(entry):
            self._append(
                "outcome",
                feedback_id=entry.candidate_id,
                resolve_demand_sequence=self.demand_sequence,
                line=entry.line,
                outcome="unknown_owner_replaced",
            )
            return

        if status not in ("useful", "unused"):
            raise AssertionError(f"unexpected feedback status {status!r}")
        self.controller.note_sample(
            entry.quality_index, entry.quality_generation, status,
            entry.offset_context_index, entry.offset_context_generation,
            entry.recovery_generation, entry.audit_generation,
        )
        self._append(
            "outcome",
            feedback_id=entry.candidate_id,
            resolve_demand_sequence=self.demand_sequence,
            line=entry.line,
            outcome=status,
        )

    def candidate(self, row: sqlite3.Row) -> None:
        pc = _row_value(row, "PC")
        kind_value = int(row["Kind"])
        kind = KIND_NAMES.get(kind_value)
        if kind is None:
            raise ValueError(f"unsupported BOP kind {kind_value}")
        trigger_line = _row_value(row, "TriggerLine")
        candidate_line = _row_value(row, "CandidateLine")
        lookup = self.controller.lookup(pc, kind)
        allowed, sampled = self.controller.should_issue(
            lookup, pc, kind, trigger_line,
        )
        self._append(
            "candidate",
            pc=pc,
            kind=kind_value,
            trigger_line=trigger_line,
            candidate_line=candidate_line,
            state=STATE_VALUES[lookup.state],
            allowed=int(allowed),
            sampled=int(sampled),
        )

        if not sampled:
            return

        feedback_id = self.next_feedback_id + 1
        self.feedback_kinds[feedback_id] = kind_value
        owner_set, owner_tag = self.controller._key(pc, kind)
        inserted = self.feedback.insert(
            candidate_line, lookup.index, lookup.generation,
            self.demand_sequence, feedback_id,
            lookup.context_index, lookup.context_generation,
            lookup.recovery_generation, lookup.audit_generation,
            owner_set, owner_tag, kind,
        )
        if not inserted:
            self.feedback_kinds.pop(feedback_id)
            return

        self.next_feedback_id = feedback_id
        self._append(
            "issue",
            feedback_id=feedback_id,
            issue_demand_sequence=self.demand_sequence,
            line=candidate_line,
            kind=kind_value,
        )

    def demand(self, row: sqlite3.Row) -> None:
        self.demand_sequence += 1
        line = _row_value(row, "Line")
        self._append(
            "demand", demand_sequence=self.demand_sequence, line=line,
        )
        self.feedback.observe_demand(line, self.demand_sequence)


def _config_from_meta(row: sqlite3.Row) -> DirectQualityConfig:
    schema_version = int(row["SchemaVersion"])
    if schema_version not in (3, 4, 5, 6):
        raise ValueError(
            "direct-quality certification requires V3, V4, V5, or V6 raw-candidate trace "
            "metadata"
        )
    if schema_version in (5, 6):
        return DirectQualityConfig(
            quality_entries=int(row["QualityEntries"]),
            quality_ways=int(row["QualityWays"]),
            quality_tag_bits=int(row["QualityTagBits"]),
            quality_hash_layout=str(row["QualityHashLayout"]),
            feedback_owner_layout=str(row["FeedbackOwnerLayout"]),
            offset_context_slots=1,
            feedback_entries=int(row["FeedbackEntries"]),
            feedback_ways=int(row["FeedbackWays"]),
            feedback_tag_bits=int(row["FeedbackTagBits"]),
            feedback_address_layout=str(row["FeedbackAddressLayout"]),
            horizon=int(row["Horizon"]),
            feedback_expiry_mode=str(row["FeedbackExpiryMode"]),
            feedback_age_encoding=str(row["FeedbackAgeEncoding"]),
            feedback_epoch_timeout=int(row["FeedbackEpochTimeout"]),
            feedback_epoch_bits=(
                int(row["FeedbackEpochBits"]) if schema_version == 6 else 0
            ),
            feedback_epoch_shift=(
                int(row["FeedbackEpochShift"]) if schema_version == 6 else 0
            ),
            observe_sample_period=int(row["ObserveSamplePeriod"]),
            observe_issue_all=True,
            open_sample_period=int(row["OpenSamplePeriod"]),
            block_probe_period=int(row["BlockProbePeriod"]),
            borderline_block_probe_period=int(row["BorderlineBlockProbePeriod"]),
            min_samples=int(row["MinSamples"]),
            unused_per_useful=int(row["UnusedPerUseful"]),
            block_guard=int(row["BlockGuard"]),
            strict_unused_per_useful=int(row["StrictUnusedPerUseful"]),
            strict_block_guard=int(row["StrictBlockGuard"]),
            reopen_unused_per_useful=int(row["ReopenUnusedPerUseful"]),
            reopen_guard=int(row["ReopenGuard"]),
            reopen_probe_period=int(row["ReopenProbePeriod"]),
            reopen_confirm_samples=int(row["ReopenConfirmSamples"]),
            decay_period=int(row["DecayPeriod"]),
        )
    return DirectQualityConfig(
        quality_entries=int(row["QualityEntries"]),
        quality_ways=int(row["QualityWays"]),
        quality_tag_bits=int(row["QualityTagBits"]),
        quality_hash_layout=QUALITY_HASH_LAYOUT_MIX64,
        feedback_owner_layout=FEEDBACK_OWNER_LAYOUT_SLOT_GENERATION,
        offset_context_slots=1,
        feedback_entries=int(row["FeedbackEntries"]),
        feedback_ways=int(row["FeedbackWays"]),
        feedback_tag_bits=(36 if schema_version == 4 else 24),
        feedback_address_layout=(
            FEEDBACK_ADDRESS_LAYOUT_SV48
            if schema_version == 4 else FEEDBACK_ADDRESS_LAYOUT_LEGACY
        ),
        horizon=int(row["Horizon"]),
        feedback_expiry_mode=FEEDBACK_EXPIRY_MODE_HEAP,
        feedback_age_encoding=FEEDBACK_AGE_ENCODING_FULL,
        observe_sample_period=int(row["ObserveSamplePeriod"]),
        observe_issue_all=True,
        open_sample_period=int(row["OpenSamplePeriod"]),
        block_probe_period=int(row["BlockProbePeriod"]),
        borderline_block_probe_period=int(row["BorderlineBlockProbePeriod"]),
        min_samples=int(row["MinSamples"]),
        unused_per_useful=int(row["UnusedPerUseful"]),
        block_guard=int(row["BlockGuard"]),
        strict_unused_per_useful=int(row["StrictUnusedPerUseful"]),
        strict_block_guard=int(row["StrictBlockGuard"]),
        reopen_unused_per_useful=int(row["ReopenUnusedPerUseful"]),
        reopen_guard=int(row["ReopenGuard"]),
        reopen_probe_period=int(row["ReopenProbePeriod"]),
        reopen_confirm_samples=int(row["ReopenConfirmSamples"]),
        decay_period=int(row["DecayPeriod"]),
    )


def _event_rows(connection: sqlite3.Connection) -> Any:
    """Stream one normalized record per online trace event in event order."""
    return connection.execute(
        "SELECT EventSequence, 'candidate' AS EventType, PC, Kind, "
        "TriggerLine, CandidateLine, State, Allowed, Sampled, "
        "NULL AS FeedbackId, NULL AS IssueDemandSequence, "
        "NULL AS DemandSequence, NULL AS Line, NULL AS Outcome "
        "FROM BOPDirectQualityCandidate "
        "UNION ALL "
        "SELECT EventSequence, 'issue' AS EventType, NULL, Kind, "
        "NULL, NULL, NULL, NULL, NULL, FeedbackId, IssueDemandSequence, "
        "NULL, Line, NULL FROM BOPDirectQualityIssue "
        "UNION ALL "
        "SELECT EventSequence, 'demand' AS EventType, NULL, NULL, "
        "NULL, NULL, NULL, NULL, NULL, NULL, NULL, DemandSequence, "
        "Line, NULL FROM BOPDirectQualityDemand "
        "UNION ALL "
        "SELECT EventSequence, 'outcome' AS EventType, NULL, NULL, "
        "NULL, NULL, NULL, NULL, NULL, FeedbackId, NULL, "
        "ResolveDemandSequence, Line, Outcome FROM BOPDirectQualityOutcome "
        "ORDER BY EventSequence"
    )


def _actual_event(row: sqlite3.Row) -> dict[str, Any]:
    event_type = str(row["EventType"])
    event: dict[str, Any] = {
        "event_sequence": int(row["EventSequence"]), "type": event_type,
    }
    if event_type == "candidate":
        event.update({
            "pc": _row_value(row, "PC"),
            "kind": int(row["Kind"]),
            "trigger_line": _row_value(row, "TriggerLine"),
            "candidate_line": _row_value(row, "CandidateLine"),
            "state": int(row["State"]),
            "allowed": int(row["Allowed"]),
            "sampled": int(row["Sampled"]),
        })
    elif event_type == "issue":
        event.update({
            "feedback_id": int(row["FeedbackId"]),
            "issue_demand_sequence": int(row["IssueDemandSequence"]),
            "line": _row_value(row, "Line"), "kind": int(row["Kind"]),
        })
    elif event_type == "demand":
        event.update({
            "demand_sequence": int(row["DemandSequence"]),
            "line": _row_value(row, "Line"),
        })
    elif event_type == "outcome":
        event.update({
            "feedback_id": int(row["FeedbackId"]),
            "resolve_demand_sequence": int(row["DemandSequence"]),
            "line": _row_value(row, "Line"),
            "outcome": str(row["Outcome"]),
        })
    else:
        raise AssertionError(f"unknown event type {event_type!r}")
    return event


def certify(database: Path) -> dict[str, Any]:
    with sqlite3.connect(database) as connection:
        connection.row_factory = sqlite3.Row
        tables = {
            str(row[0]) for row in connection.execute(
                "SELECT name FROM sqlite_master WHERE type = 'table'"
            )
        }
        required = {
            "BOPDirectQualityMeta", "BOPDirectQualityCandidate",
            "BOPDirectQualityIssue", "BOPDirectQualityDemand",
            "BOPDirectQualityOutcome",
        }
        missing = sorted(required - tables)
        if missing:
            raise ValueError("missing direct-quality trace tables: " + ", ".join(missing))
        meta = connection.execute("SELECT * FROM BOPDirectQualityMeta").fetchone()
        if meta is None:
            raise ValueError("BOPDirectQualityMeta is empty")
        config = _config_from_meta(meta)
        pending_expected: deque[dict[str, Any]] = deque()
        runner = RawTraceReplay(config, pending_expected.append)
        mismatches: list[dict[str, Any]] = []
        mismatch_count = 0
        observed_count = 0
        observed_by_type = {event_type: 0 for event_type in (
            "candidate", "issue", "demand", "outcome",
        )}
        for row in _event_rows(connection):
            event_type = str(row["EventType"])
            if event_type == "candidate":
                runner.candidate(row)
            elif event_type == "demand":
                runner.demand(row)

            observed = _actual_event(row)
            expected = pending_expected.popleft() if pending_expected else None
            observed_count += 1
            observed_by_type[event_type] += 1
            if expected != observed:
                mismatch_count += 1
                if len(mismatches) < 50:
                    mismatches.append({
                        "index": observed_count - 1,
                        "expected": expected,
                        "observed": observed,
                    })
        while pending_expected:
            mismatch_count += 1
            if len(mismatches) < 50:
                mismatches.append({
                    "index": observed_count,
                    "expected": pending_expected.popleft(),
                    "observed": None,
                })
    return {
        "database": str(database),
        "pass": mismatch_count == 0,
        "config": asdict(config),
        "expected_events": runner.event_count,
        "observed_events": observed_count,
        "mismatch_count": mismatch_count,
        "candidates": observed_by_type["candidate"],
        "issues": observed_by_type["issue"],
        "demands": observed_by_type["demand"],
        "outcomes": observed_by_type["outcome"],
        "mismatches": mismatches,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("database", type=Path)
    parser.add_argument("--output", type=Path, default=None)
    args = parser.parse_args()
    try:
        report = certify(args.database)
    except (OSError, sqlite3.Error, ValueError) as error:
        print(f"direct-quality certification failed: {error}", file=sys.stderr)
        return 1
    serialized = json.dumps(report, indent=2, sort_keys=True)
    if args.output is not None:
        args.output.write_text(serialized + "\n")
    print(serialized)
    return 0 if report["pass"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
