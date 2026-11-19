#!/usr/bin/env python3

import argparse
import concurrent.futures
import json
import pathlib
import re
import sqlite3
import sys
from collections import Counter
from dataclasses import asdict, replace
from typing import Dict, List, Mapping, Optional, Sequence, Tuple

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from util.ts_bop_degree.analyze import (
    BLOCK_BITS,
    BLOCK_SIZE,
    MASK64,
    Access,
    BopReplay,
    Candidate,
    ResourceConfig,
    SchemeTracker,
    block_address,
    iter_accesses,
    merged_config,
    pbop_config,
    read_clock_period,
    read_marker,
    same_page,
    vbop_config,
)
from util.ts_bop_degree.partitioned_pool import (
    PartitionedBopReplay,
    offset_family,
)


SHARED_RESOURCE = ResourceConfig(
    name="f256_p8e4_3to1",
    primary_fifo_size=8,
    extra_fifo_size=4,
    primary_slots=3,
    extra_slots=1,
    recent_filter_entries=256,
)
DUAL_RESOURCE = ResourceConfig(
    name="dual_p16e16_strict",
    primary_fifo_size=16,
    extra_fifo_size=16,
    primary_slots=1,
    extra_slots=1,
    role_mode="bop_kind",
    strict_primary=True,
)
DEFAULT_CONFIGS = (
    (1, 50, 31),
    (2, 50, 31),
    (2, 25, 31),
    (4, 50, 31),
    (4, 25, 31),
    (4, 13, 31),
)


def load_json(path: pathlib.Path) -> Dict:
    return json.loads(path.read_text())


def parse_config(value: str) -> Tuple[int, int, int]:
    fields = value.split(":")
    if len(fields) not in (2, 3):
        raise argparse.ArgumentTypeError(
            "config must be TESTS_PER_ACCESS:ROUND_MAX[:SCORE_MAX]"
        )
    try:
        tests_per_access = int(fields[0])
        round_max = int(fields[1])
        score_max = int(fields[2]) if len(fields) == 3 else 20
    except ValueError as error:
        raise argparse.ArgumentTypeError(
            "config fields must be integers"
        ) from error
    if tests_per_access <= 0 or round_max <= 0 or score_max <= 0:
        raise argparse.ArgumentTypeError("config values must be positive")
    return tests_per_access, round_max, score_max


def scheme_name(tests_per_access: int, round_max: int, score_max: int) -> str:
    return f"mixed_v2p2_n{tests_per_access}_r{round_max}_s{score_max}"


class MultiTestPartitionedReplay(PartitionedBopReplay):
    """One merged Teacher and one partitioned Student with mixed delay."""

    def __init__(
        self,
        clock_period_ticks: int,
        tests_per_access: int,
        round_max: int,
        score_max: int,
        vbop_slots: int = 2,
        pbop_slots: int = 2,
        student_filter_entries: int = 4096,
        replay_name: Optional[str] = None,
    ):
        super().__init__(
            clock_period_ticks,
            vbop_slots=vbop_slots,
            pbop_slots=pbop_slots,
            selection_policy="independent",
        )
        self.tests_per_access = tests_per_access
        self.cfg = replace(
            self.cfg,
            name=(replay_name or scheme_name(
                tests_per_access, round_max, score_max)),
            round_max=round_max,
            score_max=score_max,
            student_filter_entries=student_filter_entries,
        )

    def _rank_teacher_winners(self) -> List[int]:
        winners = []
        for family in ("vbop", "pbop"):
            indexes = [
                index
                for index, score in enumerate(self.scores)
                if score > 0
                and offset_family(self.cfg.offsets[index]) == family
            ]
            if not indexes:
                continue
            winner_index = max(
                indexes,
                key=lambda index: (self.scores[index], -index),
            )
            winner = self.cfg.offsets[winner_index]
            winners.append(winner)
            self.stats[f"teacher_{family}_winner_offset::{winner}"] += 1
        self.stats[f"teacher_family_winner_count::{len(winners)}"] += 1
        return winners

    def _student_enqueue_prediction(
        self,
        train_addr: int,
        bit_index: int,
        offset: int,
        tick: int,
    ) -> None:
        if offset_family(offset) == "vbop":
            super()._student_enqueue_prediction(
                train_addr,
                bit_index,
                offset,
                tick,
            )
            return

        predicted = train_addr + offset * BLOCK_SIZE
        if predicted < 0 or predicted > MASK64:
            return
        if not self.cfg.cross_page and not same_page(train_addr, predicted):
            return
        indexes = self._student_hash_indexes(predicted >> BLOCK_BITS)
        self._student_insert_mask(indexes, 1 << bit_index)
        self.stats["student_pbop_immediate_inserts"] += 1

    def _run_teacher_tests(self, addr: int) -> Optional[List[int]]:
        self.stats["teacher_test_batches"] += 1
        for _ in range(self.tests_per_access):
            self.stats["teacher_tests"] += 1
            winners = super()._teacher_train(addr)
            if winners is not None:
                return winners
        return None

    def step(self, access: Access) -> List[Candidate]:
        addr = block_address(access.addr)
        issue_gate = self._issue_gate_allows()
        rr_insert_count = access.vbop_rr_insert_count
        if rr_insert_count is None:
            self._drain_delay_events(access.tick)
        else:
            self._drain_rr_to_count(rr_insert_count)
        self._enqueue_rr(addr, access.tick)

        teacher_winners = self._run_teacher_tests(addr)
        self._student_observe(addr, access.tick)
        phase_end = teacher_winners is not None
        if phase_end:
            self._student_phase_end(teacher_winners)

        student_issue = (
            self.student_selected_valid and self.student_selected_enable
        )
        if student_issue:
            issue_offsets = list(self.student_selected_offsets)
        elif self.issue_prefetch:
            issue_offsets = [self.best_offset]
        else:
            issue_offsets = []

        candidates: List[Candidate] = []
        if issue_offsets and issue_gate:
            for rank, offset in enumerate(issue_offsets):
                prefetch_addr = (addr + offset * BLOCK_SIZE) & MASK64
                accepted = self.cfg.cross_page or same_page(
                    access.addr,
                    prefetch_addr,
                )
                role = "primary" if rank == 0 else "extra"
                candidates.append(Candidate(
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
                ))
                self.stats["generated"] += 1
                self.stats[f"generated_rank_{rank}"] += 1
                self.stats[f"issued_offset::{offset}"] += 1
                self.stats[
                    "accepted" if accepted else "cross_page_filtered"
                ] += 1
        else:
            self.stats["throttled"] += 1
        if phase_end:
            self._update_issue_confidence()
        return candidates


class FamilyConfidencePartitionedReplay(MultiTestPartitionedReplay):
    """Merged Teacher with family-qualified winners and issue gates."""

    FAMILY_ORDER = ("vbop", "pbop")

    def __init__(
        self,
        clock_period_ticks: int,
        tests_per_access: int = 2,
        round_max: int = 50,
        score_max: int = 20,
        vbop_slots: int = 4,
        pbop_slots: int = 4,
        student_filter_entries: int = 2048,
        vbop_bad_score: int = 2,
        pbop_bad_score: int = 0,
        replay_name: str = "final_v2p0_family_conf_n2r50s20",
    ):
        super().__init__(
            clock_period_ticks,
            tests_per_access,
            round_max,
            score_max,
            vbop_slots=vbop_slots,
            pbop_slots=pbop_slots,
            student_filter_entries=student_filter_entries,
            replay_name=replay_name,
        )
        self.family_bad_score = {
            "vbop": vbop_bad_score,
            "pbop": pbop_bad_score,
        }
        self.family_qualified = {
            family: False for family in self.FAMILY_ORDER
        }
        self.family_issue_confidence = {
            family: 0 for family in self.FAMILY_ORDER
        }

    def _student_confidence_groups(self) -> List[List[int]]:
        return [
            [
                index
                for index, entry in enumerate(self.student_pool)
                if offset_family(entry.offset) == family
            ]
            for family in self.FAMILY_ORDER
        ]

    def _rank_teacher_winners(self) -> List[int]:
        winners = []
        for family in self.FAMILY_ORDER:
            indexes = [
                index
                for index, offset in enumerate(self.cfg.offsets)
                if offset_family(offset) == family
            ]
            winner_index = max(
                indexes,
                key=lambda index: (self.scores[index], -index),
            )
            winner_score = self.scores[winner_index]
            qualified = winner_score > self.family_bad_score[family]
            self.family_qualified[family] = qualified
            self.stats[f"teacher_{family}_best_score_sum"] += winner_score
            self.stats[
                f"teacher_{family}_best_score::{winner_score}"
            ] += 1
            self.stats[
                f"teacher_{family}_qualified_phases"
                if qualified
                else f"teacher_{family}_rejected_phases"
            ] += 1
            if not qualified:
                continue
            winner = self.cfg.offsets[winner_index]
            winners.append(winner)
            self.stats[f"teacher_{family}_winner_offset::{winner}"] += 1
        self.stats[f"teacher_family_winner_count::{len(winners)}"] += 1
        return winners

    def _family_issue_gate_allows(self, family: str) -> bool:
        return (
            self.family_issue_confidence[family]
            < self.cfg.issue_conf_threshold
        )

    def _update_family_issue_confidence(self) -> None:
        for family in self.FAMILY_ORDER:
            confidence = self.family_issue_confidence[family]
            if self.family_qualified[family]:
                confidence = max(0, confidence - 1)
            else:
                confidence = min(15, confidence + 2)
            self.family_issue_confidence[family] = confidence
            self.stats[f"{family}_issue_confidence::{confidence}"] += 1

    def _gate_issue_offsets(self, offsets: Sequence[int]) -> List[int]:
        gated = []
        for original_rank, offset in enumerate(offsets):
            family = offset_family(offset)
            if not self._family_issue_gate_allows(family):
                self.stats[f"family_gate_blocked::{family}"] += 1
                continue
            if not gated and original_rank != 0:
                self.stats[f"family_gate_promoted::{family}"] += 1
            gated.append(offset)
            self.stats[f"family_gate_allowed::{family}"] += 1
        return gated

    def step(self, access: Access) -> List[Candidate]:
        addr = block_address(access.addr)
        rr_insert_count = access.vbop_rr_insert_count
        if rr_insert_count is None:
            self._drain_delay_events(access.tick)
        else:
            self._drain_rr_to_count(rr_insert_count)
        self._enqueue_rr(addr, access.tick)

        teacher_winners = self._run_teacher_tests(addr)
        self._student_observe(addr, access.tick)
        phase_end = teacher_winners is not None
        if phase_end:
            self._student_phase_end(teacher_winners)

        student_issue = (
            self.student_selected_valid and self.student_selected_enable
        )
        if student_issue:
            issue_offsets = list(self.student_selected_offsets)
        elif self.issue_prefetch:
            issue_offsets = [self.best_offset]
        else:
            issue_offsets = []
        issue_offsets = self._gate_issue_offsets(issue_offsets)

        candidates: List[Candidate] = []
        if issue_offsets:
            for rank, offset in enumerate(issue_offsets):
                prefetch_addr = (addr + offset * BLOCK_SIZE) & MASK64
                accepted = self.cfg.cross_page or same_page(
                    access.addr,
                    prefetch_addr,
                )
                role = "primary" if rank == 0 else "extra"
                family = offset_family(offset)
                candidates.append(Candidate(
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
                ))
                self.stats["generated"] += 1
                self.stats[f"generated_rank_{rank}"] += 1
                self.stats[f"generated_family::{family}"] += 1
                self.stats[f"issued_offset::{offset}"] += 1
                self.stats[
                    "accepted" if accepted else "cross_page_filtered"
                ] += 1
        else:
            self.stats["throttled"] += 1
        if phase_end:
            self._update_family_issue_confidence()
        return candidates


def local_sent_count(metrics: Mapping) -> int:
    return sum(metrics.get("sent_by_role", {}).values())


def phase_summary(stats: Mapping[str, int]) -> Dict:
    phases = int(stats.get("teacher_phases", 0))
    tests = int(stats.get("teacher_tests", 0))
    batches = int(stats.get("teacher_test_batches", 0))
    summary = {
        "phase_count": phases,
        "tests_per_access": tests / batches if batches else 0.0,
        "average_rounds": (
            stats.get("teacher_rounds_sum", 0) / phases if phases else 0.0
        ),
        "average_best_score": (
            stats.get("teacher_best_score_sum", 0) / phases
            if phases
            else 0.0
        ),
        "issue_enabled_fraction": (
            stats.get("teacher_issue_enabled_phases", 0) / phases
            if phases
            else 0.0
        ),
        "two_family_winner_fraction": (
            stats.get("teacher_family_winner_count::2", 0) / phases
            if phases
            else 0.0
        ),
    }
    for family in ("vbop", "pbop"):
        summary[f"{family}_qualified_fraction"] = (
            stats.get(f"teacher_{family}_qualified_phases", 0) / phases
            if phases
            else 0.0
        )
        summary[f"{family}_average_best_score"] = (
            stats.get(f"teacher_{family}_best_score_sum", 0) / phases
            if phases
            else 0.0
        )
    return summary


def tracker_result(tracker: SchemeTracker) -> Dict:
    replay = tracker.replays[0] if len(tracker.replays) == 1 else None
    result = {
        "metrics": tracker.metrics.to_dict(),
        "local_metrics": tracker.resource_results()[0]["metrics"],
    }
    if replay is not None:
        stats = tracker.replay_stats()[replay.cfg.name]["measurement"]
        result["phase"] = phase_summary(stats)
    return result


def build_trackers(
    clock_period_ticks: int,
    configs: Sequence[Tuple[int, int, int]],
) -> Dict[str, SchemeTracker]:
    dual = SchemeTracker(
        "dual_current",
        [
            BopReplay(vbop_config(clock_period_ticks), clock_period_ticks),
            BopReplay(pbop_config(clock_period_ticks), clock_period_ticks),
        ],
    )
    merged_cfg = merged_config(2, "greedy_marginal", 0.02, 1)
    merged = SchemeTracker(
        "merged_d2",
        [BopReplay(merged_cfg, clock_period_ticks)],
    )
    old_partition = SchemeTracker(
        "partition_v2p2_all_delay_n1_r50_s31",
        [PartitionedBopReplay(
            clock_period_ticks,
            vbop_slots=2,
            pbop_slots=2,
            selection_policy="independent",
        )],
    )
    trackers = {
        dual.name: dual,
        merged.name: merged,
        old_partition.name: old_partition,
    }
    for config in configs:
        replay = MultiTestPartitionedReplay(clock_period_ticks, *config)
        trackers[replay.cfg.name] = SchemeTracker(
            replay.cfg.name,
            [replay],
        )
    return trackers


def run_checkpoint(
    entry: Mapping,
    configs: Sequence[Tuple[int, int, int]],
    max_measurement_accesses: int,
) -> Dict:
    db_path = pathlib.Path(entry["result_dir"]) / "trace.db"
    clock_period_ticks = read_clock_period(db_path, None)
    connection = sqlite3.connect(str(db_path))
    marker_tick, marker_seq = read_marker(connection)
    trackers = build_trackers(clock_period_ticks, configs)
    for tracker in trackers.values():
        resource = DUAL_RESOURCE if tracker.name == "dual_current" else SHARED_RESOURCE
        tracker.attach_resources(
            [resource],
            clock_period_ticks,
            marker_tick,
            marker_seq,
        )

    access_count = 0
    measurement_count = 0
    for access in iter_accesses(connection):
        measuring = access.seq > marker_seq
        if measuring and measurement_count >= max_measurement_accesses:
            break
        access_count += 1
        for tracker in trackers.values():
            tracker.process(access, measuring)
        if access.seq == marker_seq:
            for tracker in trackers.values():
                tracker.mark_boundary()
        if measuring:
            measurement_count += 1
    connection.close()

    return {
        "name": entry["name"],
        "workload": entry["workload"],
        "weight": float(entry["weight"]),
        "result_dir": entry["result_dir"],
        "marker": {"tick": marker_tick, "access_seq": marker_seq},
        "replayed_accesses": access_count,
        "measurement_accesses": measurement_count,
        "schemes": {
            name: tracker_result(tracker)
            for name, tracker in trackers.items()
        },
    }


def aggregate_ratio(
    checkpoints: Sequence[Mapping],
    scheme: str,
    metric_group: str,
    numerator: str,
    denominator: str,
) -> Dict[str, float]:
    weight_sum = sum(float(item["weight"]) for item in checkpoints)
    checkpoint_weighted = sum(
        float(item["weight"])
        * item["schemes"][scheme][metric_group][numerator]
        / item["schemes"][scheme][metric_group][denominator]
        for item in checkpoints
    ) / weight_sum
    numerator_sum = sum(
        item["schemes"][scheme][metric_group][numerator]
        for item in checkpoints
    )
    denominator_sum = sum(
        item["schemes"][scheme][metric_group][denominator]
        for item in checkpoints
    )
    return {
        "checkpoint_weighted": checkpoint_weighted,
        "access_weighted": numerator_sum / denominator_sum,
    }


def aggregate(checkpoints: Sequence[Mapping]) -> Dict:
    if not checkpoints:
        return {}
    output = {}
    for scheme in checkpoints[0]["schemes"]:
        local_sent = []
        for item in checkpoints:
            metrics = item["schemes"][scheme]["local_metrics"]
            local_sent.append({
                "weight": item["weight"],
                "sent": local_sent_count(metrics),
                "demand": metrics["demand_count"],
            })
        weight_sum = sum(float(item["weight"]) for item in local_sent)
        output[scheme] = {
            "ideal_coverage": aggregate_ratio(
                checkpoints,
                scheme,
                "metrics",
                "covered_count",
                "demand_count",
            ),
            "local_coverage": aggregate_ratio(
                checkpoints,
                scheme,
                "local_metrics",
                "covered_count",
                "demand_count",
            ),
            "generated_per_demand": aggregate_ratio(
                checkpoints,
                scheme,
                "metrics",
                "generated_count",
                "demand_count",
            ),
            "local_sent_per_demand": {
                "checkpoint_weighted": sum(
                    float(item["weight"])
                    * item["sent"] / item["demand"]
                    for item in local_sent
                ) / weight_sum,
                "access_weighted": (
                    sum(item["sent"] for item in local_sent)
                    / sum(item["demand"] for item in local_sent)
                ),
            },
        }
    return output


def write_result(
    path: pathlib.Path,
    manifest: pathlib.Path,
    configs: Sequence[Tuple[int, int, int]],
    max_measurement_accesses: int,
    checkpoints: Sequence[Mapping],
) -> None:
    document = {
        "definition": (
            "Quick open-loop replay of one merged Teacher testing N consecutive "
            "offsets per access. The Teacher emits the highest positive-score "
            "winner from each historical family into a strict 2+2 Student pool. "
            "The Student uses one shared filter/phase; PBOP-family predictions "
            "are immediately visible while VBOP-family predictions use the "
            "300-cycle delay queue."
        ),
        "manifest": str(manifest),
        "max_measurement_accesses": max_measurement_accesses,
        "configs": [
            {
                "tests_per_access": tests,
                "round_max": rounds,
                "score_max": score,
            }
            for tests, rounds, score in configs
        ],
        "resource_config": asdict(SHARED_RESOURCE),
        "checkpoints": list(checkpoints),
        "aggregate": aggregate(checkpoints),
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Quick replay of a multi-test merged TS-BOP Teacher"
    )
    parser.add_argument(
        "--manifest",
        type=pathlib.Path,
        default=pathlib.Path("util/ts_bop_degree/checkpoints.json"),
    )
    parser.add_argument("--out-json", required=True, type=pathlib.Path)
    parser.add_argument(
        "--config",
        action="append",
        type=parse_config,
        default=[],
        help="TESTS_PER_ACCESS:ROUND_MAX[:SCORE_MAX]",
    )
    parser.add_argument("--checkpoint-regex")
    parser.add_argument("--max-measurement-accesses", type=int, default=200000)
    parser.add_argument("--jobs", type=int, default=1)
    args = parser.parse_args()
    if args.max_measurement_accesses <= 0:
        raise ValueError("max measurement accesses must be positive")
    if args.jobs <= 0:
        raise ValueError("jobs must be positive")

    configs = sorted(set(args.config or DEFAULT_CONFIGS))
    manifest = load_json(args.manifest)
    pattern = re.compile(args.checkpoint_regex) if args.checkpoint_regex else None
    selected = [
        entry
        for entry in manifest
        if pattern is None or pattern.search(entry["name"])
    ]
    if not selected:
        raise ValueError("checkpoint filter selected no entries")

    results: Dict[str, Mapping] = {}
    with concurrent.futures.ProcessPoolExecutor(
        max_workers=min(args.jobs, len(selected))
    ) as executor:
        futures = {
            executor.submit(
                run_checkpoint,
                entry,
                configs,
                args.max_measurement_accesses,
            ): entry
            for entry in selected
        }
        for future in concurrent.futures.as_completed(futures):
            entry = futures[future]
            result = future.result()
            results[entry["name"]] = result
            ordered = [
                results[item["name"]]
                for item in selected
                if item["name"] in results
            ]
            write_result(
                args.out_json,
                args.manifest,
                configs,
                args.max_measurement_accesses,
                ordered,
            )
            print(json.dumps({
                "checkpoint": result["name"],
                "measurement_accesses": result["measurement_accesses"],
                "completed": len(ordered),
                "selected": len(selected),
            }), flush=True)

    ordered = [results[item["name"]] for item in selected]
    write_result(
        args.out_json,
        args.manifest,
        configs,
        args.max_measurement_accesses,
        ordered,
    )


if __name__ == "__main__":
    main()
