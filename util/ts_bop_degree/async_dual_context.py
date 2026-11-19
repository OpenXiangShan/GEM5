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
    Access,
    BopReplay,
    Candidate,
    ResourceConfig,
    SchemeTracker,
    block_address,
    iter_accesses,
    pbop_config,
    read_clock_period,
    read_marker,
    same_page,
    vbop_config,
)


RESOURCE_NAME = "f256_p8e4_3to1"
RESOURCE_CONFIG = ResourceConfig(
    name=RESOURCE_NAME,
    primary_fifo_size=8,
    extra_fifo_size=4,
    primary_slots=3,
    extra_slots=1,
    recent_filter_entries=256,
)

DEFAULT_CONFIGS = (
    (2, 2, False),
    (2, 2, True),
    (2, 8, False),
    (4, 2, False),
    (3, 3, False),
    (4, 8, False),
)


def load_json(path: pathlib.Path) -> Dict:
    return json.loads(path.read_text())


def find_scheme(document: Mapping, name: str) -> Mapping:
    return next(item for item in document["schemes"] if item["name"] == name)


def find_resource(scheme: Mapping, name: str) -> Mapping:
    return next(
        item for item in scheme.get("resources", []) if item["name"] == name
    )


def scheme_name(
    vbop_slots: int,
    pbop_slots: int,
    pbop_delay_enabled: bool,
) -> str:
    suffix = "_pbop_delay" if pbop_delay_enabled else ""
    return f"async_v{vbop_slots}_p{pbop_slots}{suffix}"


def parse_config(value: str) -> Tuple[int, int, bool]:
    fields = value.split(":")
    if len(fields) not in (2, 3):
        raise argparse.ArgumentTypeError(
            "config must be VBOP_SLOTS:PBOP_SLOTS[:PBOP_DELAY]"
        )
    try:
        vbop_slots = int(fields[0])
        pbop_slots = int(fields[1])
        pbop_delay = bool(int(fields[2])) if len(fields) == 3 else False
    except ValueError as error:
        raise argparse.ArgumentTypeError(
            "slot counts and PBOP_DELAY must be integers"
        ) from error
    if vbop_slots <= 0 or pbop_slots <= 0:
        raise argparse.ArgumentTypeError("slot counts must be positive")
    return vbop_slots, pbop_slots, pbop_delay


class AsyncDualContextReplay:
    """Two asynchronous BOP contexts sharing one RR history instance."""

    def __init__(
        self,
        clock_period_ticks: int,
        vbop_slots: int,
        pbop_slots: int,
        pbop_delay_enabled: bool = False,
    ):
        self.name = scheme_name(
            vbop_slots,
            pbop_slots,
            pbop_delay_enabled,
        )
        self.cfg = replace(vbop_config(clock_period_ticks), name=self.name)
        self.clock_period_ticks = clock_period_ticks
        self.vbop = BopReplay(
            replace(
                vbop_config(clock_period_ticks),
                student_pool_size=vbop_slots,
            ),
            clock_period_ticks,
        )
        pbop = pbop_config(clock_period_ticks)
        if pbop_delay_enabled:
            pbop = replace(
                pbop,
                student_delay_enabled=True,
                student_delay_size=64,
            )
        self.pbop = BopReplay(
            replace(pbop, student_pool_size=pbop_slots),
            clock_period_ticks,
        )

        # The two historical BOPs receive the same train stream and use the
        # same RR geometry. Keep one physical history/delay queue while each
        # family retains independent score, phase, and Student state.
        self.history = BopReplay(
            replace(vbop_config(clock_period_ticks), name="SharedRR"),
            clock_period_ticks,
        )
        for replay in (self.vbop, self.pbop):
            replay.rr_left = self.history.rr_left
            replay.rr_right = self.history.rr_right

        self.stats: Counter[str] = Counter()
        self._marker_stats: Optional[Counter[str]] = None
        self._marker_context_stats: Optional[Dict[str, Counter[str]]] = None
        self._last_phase_seq: Dict[str, Optional[int]] = {
            "VBOP": None,
            "PBOP": None,
        }

    @property
    def config(self) -> Dict:
        return {
            "vbop_slots": self.vbop.cfg.student_pool_size,
            "pbop_slots": self.pbop.cfg.student_pool_size,
            "pbop_delay_enabled": self.pbop.cfg.student_delay_enabled,
            "vbop_delay_enabled": self.vbop.cfg.student_delay_enabled,
            "teacher_score_max": self.vbop.cfg.score_max,
            "teacher_round_max": self.vbop.cfg.round_max,
            "teacher_tests_per_access": 2,
            "shared_rr": True,
            "asynchronous_family_phases": True,
        }

    def mark_boundary(self) -> None:
        self._marker_stats = Counter(self.stats)
        self._marker_context_stats = {
            "VBOP": Counter(self.vbop.stats),
            "PBOP": Counter(self.pbop.stats),
        }

    def measurement_stats(self) -> Dict[str, Dict[str, int]]:
        before = self._marker_context_stats or {
            "VBOP": Counter(),
            "PBOP": Counter(),
        }
        return {
            "VBOP": dict(Counter(self.vbop.stats) - before["VBOP"]),
            "PBOP": dict(Counter(self.pbop.stats) - before["PBOP"]),
        }

    def measurement_async_stats(self) -> Dict[str, int]:
        before = self._marker_stats or Counter()
        return dict(Counter(self.stats) - before)

    def _advance_shared_history(self, access: Access, addr: int) -> None:
        if (
            access.vbop_rr_insert_count is not None
            and access.pbop_rr_insert_count is not None
            and access.vbop_rr_insert_count != access.pbop_rr_insert_count
        ):
            raise ValueError(
                "shared RR requires identical VBOP/PBOP insert counts: "
                f"{access.vbop_rr_insert_count} != "
                f"{access.pbop_rr_insert_count} at access {access.seq}"
            )
        target_count = access.vbop_rr_insert_count
        if target_count is None:
            self.history._drain_delay_events(access.tick)
        else:
            self.history._drain_rr_to_count(target_count)
        self.history._enqueue_rr(addr, access.tick)

    def _record_phase_end(self, family: str, access: Access) -> None:
        key = family.lower()
        self.stats[f"{key}_phase_ends"] += 1
        previous = self._last_phase_seq[family]
        if previous is not None:
            interval = access.seq - previous
            self.stats[f"{key}_phase_interval_sum"] += interval
            self.stats[f"{key}_phase_interval_max"] = max(
                self.stats[f"{key}_phase_interval_max"],
                interval,
            )
        self._last_phase_seq[family] = access.seq

    def _issue_family(
        self,
        replay: BopReplay,
        access: Access,
        addr: int,
        issue_gate: bool,
        rank: int,
        role: str,
    ) -> List[Candidate]:
        student_issue = (
            replay.student_selected_valid and replay.student_selected_enable
        )
        if student_issue:
            issue_offset = replay.student_selected_offsets[0]
        elif replay.issue_prefetch:
            issue_offset = replay.best_offset
        else:
            replay.stats["throttled"] += 1
            return []
        if not issue_gate:
            replay.stats["throttled"] += 1
            return []

        prefetch_addr = (
            addr + issue_offset * 64
        ) & ((1 << 64) - 1)
        accepted = replay.cfg.cross_page or same_page(
            access.addr,
            prefetch_addr,
        )
        replay.stats["generated"] += 1
        replay.stats["generated_rank_0"] += 1
        replay.stats[f"issued_offset::{issue_offset}"] += 1
        replay.stats[
            "accepted" if accepted else "cross_page_filtered"
        ] += 1
        self.stats[f"generated_{replay.cfg.name.lower()}"] += 1
        return [
            Candidate(
                seq=access.seq,
                tick=access.tick,
                addr=prefetch_addr,
                context_id=access.context_id,
                has_context_id=access.has_context_id,
                secure=access.secure,
                offset=issue_offset,
                rank=rank,
                role=role,
                bop_kind=replay.cfg.name,
                accepted=accepted,
            )
        ]

    def step(self, access: Access) -> List[Candidate]:
        addr = block_address(access.addr)
        vbop_gate = self.vbop._issue_gate_allows()
        pbop_gate = self.pbop._issue_gate_allows()
        self._advance_shared_history(access, addr)

        vbop_winners = self.vbop._teacher_train(addr)
        pbop_winners = self.pbop._teacher_train(addr)
        self.vbop._student_observe(addr, access.tick)
        self.pbop._student_observe(addr, access.tick)

        vbop_phase_end = vbop_winners is not None
        pbop_phase_end = pbop_winners is not None
        if vbop_phase_end:
            self.vbop._student_phase_end(vbop_winners)
            self._record_phase_end("VBOP", access)
        if pbop_phase_end:
            self.pbop._student_phase_end(pbop_winners)
            self._record_phase_end("PBOP", access)
        if vbop_phase_end and pbop_phase_end:
            self.stats["simultaneous_phase_ends"] += 1

        candidates = self._issue_family(
            self.vbop,
            access,
            addr,
            vbop_gate,
            rank=0,
            role="primary",
        )
        candidates.extend(
            self._issue_family(
                self.pbop,
                access,
                addr,
                pbop_gate,
                rank=1,
                role="extra",
            )
        )

        if vbop_phase_end:
            self.vbop._update_issue_confidence()
        if pbop_phase_end:
            self.pbop._update_issue_confidence()
        return candidates


def reference_results(result_dir: pathlib.Path) -> Dict:
    shortlist = load_json(result_dir / "shortlist_corrected.json")
    resources = load_json(result_dir / "resources_filter.json")
    dual_resources = load_json(result_dir / "resources_dual.json")
    dual = find_scheme(shortlist, "dual_current")
    merged = find_scheme(
        resources,
        "merged_d2_greedy_marginal_th2_top1",
    )
    dual_local = find_resource(
        find_scheme(dual_resources, "dual_current"),
        "dual_p16e16_strict",
    )
    merged_local = find_resource(merged, RESOURCE_NAME)
    return {
        "dual": {
            "metrics": dual["metrics"],
            "local_metrics": dual_local["metrics"],
            "replay_stats": {
                family: values["measurement"]
                for family, values in dual["replay_stats"].items()
            },
        },
        "merged_baseline": {
            "metrics": merged["metrics"],
            "local_metrics": merged_local["metrics"],
        },
    }


def stat_distribution(stats: Mapping[str, int], prefix: str) -> Dict[int, int]:
    return {
        int(name[len(prefix):]): int(value)
        for name, value in stats.items()
        if name.startswith(prefix) and value
    }


def validate_full_control(
    measurement_stats: Mapping[str, Mapping[str, int]],
    reference_stats: Mapping[str, Mapping[str, int]],
) -> Dict:
    scalar_fields = (
        "generated",
        "throttled",
        "student_phases",
        "student_issue_phases",
        "student_fallback_phases",
        "teacher_injected",
        "teacher_phases",
    )
    distribution_prefixes = (
        "issued_offset::",
        "student_selected_offset::",
        "teacher_injected_offset::",
        "teacher_winner_offset::",
    )
    mismatches = []
    for family in ("VBOP", "PBOP"):
        actual = measurement_stats[family]
        expected = reference_stats[family]
        for field in scalar_fields:
            actual_value = int(actual.get(field, 0))
            expected_value = int(expected.get(field, 0))
            if actual_value != expected_value:
                mismatches.append({
                    "family": family,
                    "field": field,
                    "expected": expected_value,
                    "actual": actual_value,
                })
        for prefix in distribution_prefixes:
            actual_dist = stat_distribution(actual, prefix)
            expected_dist = stat_distribution(expected, prefix)
            if actual_dist != expected_dist:
                mismatches.append({
                    "family": family,
                    "field": prefix,
                    "expected": expected_dist,
                    "actual": actual_dist,
                })
    return {
        "all_match": not mismatches,
        "mismatches": mismatches,
    }


def run_checkpoint(
    entry: Mapping,
    configs: Sequence[Tuple[int, int, bool]],
) -> Dict:
    result_dir = pathlib.Path(entry["result_dir"])
    db_path = result_dir / "trace.db"
    clock_period_ticks = read_clock_period(db_path, None)
    connection = sqlite3.connect(str(db_path))
    marker_tick, marker_seq = read_marker(connection)

    trackers: Dict[str, SchemeTracker] = {}
    replays: Dict[str, AsyncDualContextReplay] = {}
    for vbop_slots, pbop_slots, pbop_delay in configs:
        replay = AsyncDualContextReplay(
            clock_period_ticks,
            vbop_slots,
            pbop_slots,
            pbop_delay,
        )
        tracker = SchemeTracker(replay.name, [replay])
        tracker.attach_resources(
            [RESOURCE_CONFIG],
            clock_period_ticks,
            marker_tick,
            marker_seq,
        )
        trackers[replay.name] = tracker
        replays[replay.name] = replay

    access_count = 0
    for access in iter_accesses(connection):
        access_count += 1
        measuring = access.seq > marker_seq
        for tracker in trackers.values():
            tracker.process(access, measuring)
        if access.seq == marker_seq:
            for tracker in trackers.values():
                tracker.mark_boundary()
            for replay in replays.values():
                replay.mark_boundary()
    connection.close()

    references = reference_results(result_dir)
    schemes = {}
    for name, tracker in trackers.items():
        replay = replays[name]
        measurement_stats = replay.measurement_stats()
        scheme = {
            "config": replay.config,
            "metrics": tracker.metrics.to_dict(),
            "local_metrics": tracker.resource_results()[0]["metrics"],
            "measurement_stats": measurement_stats,
            "async_stats": replay.measurement_async_stats(),
        }
        if (
            replay.vbop.cfg.student_pool_size == 4
            and replay.pbop.cfg.student_pool_size == 8
            and not replay.pbop.cfg.student_delay_enabled
        ):
            scheme["dual_reproduction"] = validate_full_control(
                measurement_stats,
                references["dual"]["replay_stats"],
            )
        schemes[name] = scheme

    return {
        "name": entry["name"],
        "workload": entry["workload"],
        "weight": float(entry["weight"]),
        "result_dir": str(result_dir),
        "access_count": access_count,
        "marker": {"tick": marker_tick, "access_seq": marker_seq},
        "references": references,
        "schemes": schemes,
    }


def aggregate_ratio(
    checkpoints: Sequence[Mapping],
    selector,
    numerator: str,
    denominator: str,
) -> Dict[str, float]:
    weight_sum = sum(float(item["weight"]) for item in checkpoints)
    checkpoint_weighted = sum(
        float(item["weight"])
        * selector(item)[numerator]
        / selector(item)[denominator]
        for item in checkpoints
    ) / weight_sum
    numerator_sum = sum(selector(item)[numerator] for item in checkpoints)
    denominator_sum = sum(selector(item)[denominator] for item in checkpoints)
    return {
        "checkpoint_weighted": checkpoint_weighted,
        "access_weighted": (
            numerator_sum / denominator_sum if denominator_sum else 0.0
        ),
    }


def sent_metrics(metrics: Mapping) -> Dict[str, int]:
    return {
        "sent_count": sum(metrics["sent_by_role"].values()),
        "demand_count": metrics["demand_count"],
    }


def aggregate(checkpoints: Sequence[Mapping]) -> Dict:
    if not checkpoints:
        return {}
    result: Dict = {"references": {}, "schemes": {}}
    for reference in ("dual", "merged_baseline"):
        ideal = lambda item, name=reference: item["references"][name]["metrics"]
        local = lambda item, name=reference: item["references"][name]["local_metrics"]
        result["references"][reference] = {
            "ideal_coverage": aggregate_ratio(
                checkpoints, ideal, "covered_count", "demand_count"
            ),
            "generated_per_demand": aggregate_ratio(
                checkpoints, ideal, "generated_count", "demand_count"
            ),
            "local_coverage": aggregate_ratio(
                checkpoints, local, "covered_count", "demand_count"
            ),
            "local_sent_per_demand": aggregate_ratio(
                checkpoints,
                lambda item, select=local: sent_metrics(select(item)),
                "sent_count",
                "demand_count",
            ),
        }

    for name in sorted(checkpoints[0]["schemes"]):
        ideal = lambda item, scheme=name: item["schemes"][scheme]["metrics"]
        local = lambda item, scheme=name: item["schemes"][scheme]["local_metrics"]
        result["schemes"][name] = {
            "config": checkpoints[0]["schemes"][name]["config"],
            "ideal_coverage": aggregate_ratio(
                checkpoints, ideal, "covered_count", "demand_count"
            ),
            "generated_per_demand": aggregate_ratio(
                checkpoints, ideal, "generated_count", "demand_count"
            ),
            "local_coverage": aggregate_ratio(
                checkpoints, local, "covered_count", "demand_count"
            ),
            "local_sent_per_demand": aggregate_ratio(
                checkpoints,
                lambda item, select=local: sent_metrics(select(item)),
                "sent_count",
                "demand_count",
            ),
        }
    return result


def write_result(
    path: pathlib.Path,
    manifest: pathlib.Path,
    checkpoints: Sequence[Mapping],
    configs: Sequence[Tuple[int, int, bool]],
) -> None:
    result = {
        "manifest": str(manifest),
        "definition": (
            "Static replay of one shared RR/history engine with independent "
            "VBOP/PBOP Teacher pointers, scores, asynchronous phase ends, and "
            "family-specific Student epochs. PBOP Student coverage is immediate "
            "unless the diagnostic PBOP-delay variant is selected."
        ),
        "resource_config": asdict(RESOURCE_CONFIG),
        "configs": [
            {
                "vbop_slots": vbop_slots,
                "pbop_slots": pbop_slots,
                "pbop_delay_enabled": pbop_delay,
            }
            for vbop_slots, pbop_slots, pbop_delay in configs
        ],
        "checkpoints": list(checkpoints),
        "aggregate": aggregate(checkpoints),
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Replay asynchronous shared-Teacher TS-BOP contexts"
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
        help="VBOP_SLOTS:PBOP_SLOTS[:PBOP_DELAY]; may be repeated",
    )
    parser.add_argument("--checkpoint-regex")
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--resume", action="store_true")
    args = parser.parse_args()

    configs = tuple(args.config) if args.config else DEFAULT_CONFIGS
    if len({scheme_name(*item) for item in configs}) != len(configs):
        raise ValueError("duplicate async configurations")
    manifest = load_json(args.manifest)
    pattern = re.compile(args.checkpoint_regex) if args.checkpoint_regex else None
    selected = [
        entry
        for entry in manifest
        if pattern is None or pattern.search(entry["name"])
    ]
    if not selected:
        raise ValueError("checkpoint filter selected no entries")

    completed: Dict[str, Mapping] = {}
    if args.resume and args.out_json.exists():
        previous = load_json(args.out_json)
        completed = {
            item["name"]: item for item in previous.get("checkpoints", [])
        }

    results_by_name: Dict[str, Mapping] = {
        name: value
        for name, value in completed.items()
        if any(entry["name"] == name for entry in selected)
    }
    pending = [entry for entry in selected if entry["name"] not in completed]

    def record(result: Mapping) -> None:
        results_by_name[result["name"]] = result
        ordered = [
            results_by_name[entry["name"]]
            for entry in selected
            if entry["name"] in results_by_name
        ]
        write_result(args.out_json, args.manifest, ordered, configs)
        summaries = {
            name: {
                "ideal": scheme["metrics"]["coverage"],
                "local": scheme["local_metrics"]["coverage"],
            }
            for name, scheme in result["schemes"].items()
        }
        print(json.dumps({
            "checkpoint": result["name"],
            "schemes": summaries,
        }, sort_keys=True), flush=True)

    if args.jobs <= 1:
        for entry in pending:
            record(run_checkpoint(entry, configs))
    else:
        with concurrent.futures.ProcessPoolExecutor(
            max_workers=args.jobs
        ) as executor:
            futures = {
                executor.submit(run_checkpoint, entry, configs): entry
                for entry in pending
            }
            for future in concurrent.futures.as_completed(futures):
                record(future.result())

    ordered = [results_by_name[entry["name"]] for entry in selected]
    write_result(args.out_json, args.manifest, ordered, configs)


if __name__ == "__main__":
    main()
