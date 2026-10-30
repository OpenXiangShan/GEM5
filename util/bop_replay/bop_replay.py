#!/usr/bin/env python3
"""Read and validate the target-branch BOP teacher/student replay trace.

This utility intentionally models only the BOP-student merge decision.  CQF
admission remains an online decision recorded in ``CQFAllowed`` and is not
reimplemented here.
"""

from __future__ import annotations

import argparse
import json
import sqlite3
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Any


SCHEMA_VERSION = 7


@dataclass(frozen=True)
class BOPConfig:
    name: str
    schema_version: int
    block_size: int
    score_max: int
    round_max: int
    bad_score: int
    rr_entries: int
    tag_bits: int
    student_cover_enabled: bool
    student_pool_size: int
    student_issue_top_n: int
    student_conf_alpha: float
    student_cov_threshold: float
    student_teacher_top_n: int
    student_filter_entries: int
    student_hash_mode: str
    student_hash_count: int
    student_large_offset_priority: bool
    student_large_offset_priority_coeff: float
    student_delay_queue_enabled: bool
    student_delay_queue_size: int
    student_delay_ticks: int
    offsets: tuple[int, ...]

    @classmethod
    def from_row(cls, row: sqlite3.Row) -> "BOPConfig":
        def value(name: str, default: Any) -> Any:
            return row[name] if name in row.keys() else default

        offsets = tuple(
            int(item) for item in str(value("Offsets", "")).split(",") if item
        )
        return cls(
            name=str(row["BOPName"]),
            schema_version=int(row["SchemaVersion"]),
            block_size=int(row["BlockSize"]),
            score_max=int(row["ScoreMax"]),
            round_max=int(row["RoundMax"]),
            bad_score=int(row["BadScore"]),
            rr_entries=int(row["RREntries"]),
            tag_bits=int(row["TagBits"]),
            student_cover_enabled=bool(value("StudentCoverEnabled", False)),
            student_pool_size=int(value("StudentPoolSize", 0)),
            student_issue_top_n=int(value("StudentIssueTopN", 1)),
            student_conf_alpha=float(value("StudentConfAlpha", 0.0)),
            student_cov_threshold=float(value("StudentCovThreshold", 0.0)),
            student_teacher_top_n=int(value("StudentTeacherTopN", 1)),
            student_filter_entries=int(value("StudentFilterEntries", 0)),
            student_hash_mode=str(value("StudentHashMode", "splitmix")),
            student_hash_count=int(value("StudentHashCount", 1)),
            student_large_offset_priority=bool(
                value("StudentLargeOffsetPriority", False)
            ),
            student_large_offset_priority_coeff=float(
                value("StudentLargeOffsetPriorityCoeff", 0.99)
            ),
            student_delay_queue_enabled=bool(
                value("StudentDelayQueueEnabled", False)
            ),
            student_delay_queue_size=int(value("StudentDelayQueueSize", 0)),
            student_delay_ticks=int(value("StudentDelayTicks", 0)),
            offsets=offsets,
        )


@dataclass(frozen=True)
class ReplayEvent:
    access_seq: int
    replay_order: int
    bop_name: str
    bop_kind: str
    tick: int
    trigger_addr: int
    trigger_pc: int
    trigger_has_pc: bool
    late: bool
    teacher_offset: int
    teacher_issue_enabled: bool
    student_issue_enabled: bool
    student_selected_valid: bool
    student_selected_enable: bool
    student_selected_offset: int
    selected_offset: int
    issue_enabled: bool
    cqf_allowed: bool
    candidate_rank: int = 1
    page_allowed: bool = True

    @classmethod
    def from_row(cls, row: sqlite3.Row) -> "ReplayEvent":
        def value(name: str, default: Any) -> Any:
            return row[name] if name in row.keys() else default

        return cls(
            access_seq=int(row["AccessSeq"]),
            replay_order=int(row["ReplayOrder"]),
            bop_name=str(row["BOPName"]),
            bop_kind=str(row["BOPKind"]),
            tick=int(row["Tick"]),
            trigger_addr=int(row["TriggerAddr"]),
            trigger_pc=int(row["TriggerPC"]),
            trigger_has_pc=bool(row["TriggerHasPC"]),
            late=bool(row["Late"]),
            teacher_offset=int(row["TeacherOffset"]),
            teacher_issue_enabled=bool(row["TeacherIssueEnabled"]),
            student_issue_enabled=bool(row["StudentIssueEnabled"]),
            student_selected_valid=bool(row["StudentSelectedValid"]),
            student_selected_enable=bool(row["StudentSelectedEnable"]),
            student_selected_offset=int(row["StudentSelectedOffset"]),
            selected_offset=int(row["SelectedOffset"]),
            issue_enabled=bool(row["IssueEnabled"]),
            cqf_allowed=bool(row["CQFAllowed"]),
            candidate_rank=int(value("CandidateRank", 1)),
            page_allowed=bool(value("PageAllowed", True)),
        )


def _table_exists(connection: sqlite3.Connection, name: str) -> bool:
    row = connection.execute(
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", (name,)
    ).fetchone()
    return row is not None


def load_trace(path: str | Path) -> tuple[dict[str, BOPConfig], list[ReplayEvent]]:
    """Load BOP metadata and events from an Arch DB SQLite file."""

    connection = sqlite3.connect(str(path))
    connection.row_factory = sqlite3.Row
    try:
        if not _table_exists(connection, "BOPReplayMeta"):
            raise ValueError("BOPReplayMeta table is missing")
        if not _table_exists(connection, "BOPReplayEvent"):
            raise ValueError("BOPReplayEvent table is missing")
        metadata = {
            config.name: config
            for config in map(
                BOPConfig.from_row,
                connection.execute("SELECT * FROM BOPReplayMeta ORDER BY ID"),
            )
        }
        versions = {config.schema_version for config in metadata.values()}
        if versions and not versions.issubset({6, SCHEMA_VERSION}):
            raise ValueError(
                f"unsupported replay schema version(s): {sorted(versions)}"
            )
        events = [
            ReplayEvent.from_row(row)
            for row in connection.execute(
                "SELECT * FROM BOPReplayEvent "
                "ORDER BY AccessSeq, ReplayOrder, ID"
            )
        ]
        return metadata, events
    finally:
        connection.close()


def replay_student_decisions(events: list[ReplayEvent]) -> list[dict[str, Any]]:
    """Check the recorded teacher/student merge decision for every event."""

    mismatches: list[dict[str, Any]] = []
    for event in events:
        student_issue = (
            event.student_issue_enabled
            and event.student_selected_valid
            and event.student_selected_enable
        )
        if event.candidate_rank == 2:
            expected_offset = event.student_selected_offset
            expected_issue = student_issue and expected_offset != 0
        else:
            expected_offset = (
                event.student_selected_offset if student_issue else event.teacher_offset
            )
            expected_issue = (event.teacher_issue_enabled or student_issue) and (
                expected_offset != 0
            )
        if (
            expected_offset != event.selected_offset
            or expected_issue != event.issue_enabled
        ):
            mismatches.append(
                {
                    "access_seq": event.access_seq,
                    "replay_order": event.replay_order,
                    "bop_name": event.bop_name,
                    "candidate_rank": event.candidate_rank,
                    "expected_offset": expected_offset,
                    "recorded_offset": event.selected_offset,
                    "expected_issue": expected_issue,
                    "recorded_issue": event.issue_enabled,
                }
            )
    return mismatches


def summarize(configs: dict[str, BOPConfig], events: list[ReplayEvent]) -> dict[str, Any]:
    mismatches = replay_student_decisions(events)
    return {
        "schema_version": SCHEMA_VERSION,
        "bop_count": len(configs),
        "event_count": len(events),
        "teacher_issue_count": sum(event.teacher_issue_enabled for event in events),
        "student_issue_count": sum(event.student_issue_enabled for event in events),
        "recorded_issue_count": sum(event.issue_enabled for event in events),
        "cqf_allowed_count": sum(event.cqf_allowed for event in events),
        "merge_mismatch_count": len(mismatches),
        "merge_mismatches": mismatches,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path, help="Arch DB SQLite file")
    parser.add_argument(
        "--check", action="store_true", help="return non-zero on merge mismatch"
    )
    args = parser.parse_args()
    configs, events = load_trace(args.trace)
    result = summarize(configs, events)
    print(json.dumps(result, indent=2, sort_keys=True))
    return 1 if args.check and result["merge_mismatch_count"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
