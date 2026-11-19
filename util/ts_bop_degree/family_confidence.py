#!/usr/bin/env python3

"""Full static-trace evaluation of the selected TS-BOP family gate."""

import argparse
import concurrent.futures
import json
import pathlib
import re
import sqlite3
import sys
from dataclasses import asdict
from typing import Dict, Mapping, Sequence

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from util.ts_bop_degree.analyze import (  # noqa: E402
    BopReplay,
    ResourceConfig,
    SchemeTracker,
    iter_accesses,
    pbop_config,
    read_clock_period,
    read_marker,
    vbop_config,
)
from util.ts_bop_degree.merged_multi_test import (  # noqa: E402
    DUAL_RESOURCE,
    FamilyConfidencePartitionedReplay,
    MultiTestPartitionedReplay,
    phase_summary,
)
from util.ts_bop_degree.parameter_sweep import aggregate  # noqa: E402


BASELINE_SCHEME = "baseline_n2r50s20"
FINAL_SCHEME = "final_v2p0_family_conf_n2r50s20"
FINAL_RESOURCE = ResourceConfig(
    name="p8e8_rf128_3to1",
    primary_fifo_size=8,
    extra_fifo_size=8,
    primary_slots=3,
    extra_slots=1,
    recent_filter_entries=128,
)
FINAL_PARAMETERS = {
    "teacher_offset_count": 109,
    "teacher_tests_per_access": 2,
    "teacher_round_max": 50,
    "teacher_score_max": 20,
    "teacher_vbop_bad_score": 2,
    "teacher_pbop_bad_score": 0,
    "teacher_family_comparison": "strict_greater_than",
    "teacher_winners_per_family": 1,
    "student_vbop_slots": 4,
    "student_pbop_slots": 4,
    "student_filter_entries": 2048,
    "student_hash_mode": "pairs_low9",
    "student_degree": 2,
    "student_selection": "independent_best_per_family",
    "student_entry_confidence_scope": "per_family",
    "student_primary_coverage_threshold": 0.0,
    "student_extra_marginal_threshold": 0.02,
    "student_vbop_delay_cycles": 300,
    "student_pbop_delay_cycles": 0,
    "issue_confidence_bits": 4,
    "issue_confidence_initial": 0,
    "issue_confidence_max": 15,
    "issue_confidence_threshold": 14,
    "issue_confidence_fail_increment": 2,
    "issue_confidence_pass_decrement": 1,
    "primary_fifo_entries": 8,
    "extra_fifo_entries": 8,
    "primary_arbiter_slots": 3,
    "extra_arbiter_slots": 1,
    "arbiter_mode": "work_conserving",
    "recent_filter_entries": 128,
    "recent_filter_position": "before_primary_extra_fifos",
}


def build_trackers(clock_period_ticks: int) -> Dict[str, SchemeTracker]:
    dual = SchemeTracker(
        "dual_current",
        [
            BopReplay(vbop_config(clock_period_ticks), clock_period_ticks),
            BopReplay(pbop_config(clock_period_ticks), clock_period_ticks),
        ],
    )
    baseline_replay = MultiTestPartitionedReplay(
        clock_period_ticks,
        2,
        50,
        20,
        vbop_slots=4,
        pbop_slots=4,
        student_filter_entries=2048,
        replay_name=BASELINE_SCHEME,
    )
    final_replay = FamilyConfidencePartitionedReplay(
        clock_period_ticks,
        replay_name=FINAL_SCHEME,
    )
    return {
        dual.name: dual,
        BASELINE_SCHEME: SchemeTracker(BASELINE_SCHEME, [baseline_replay]),
        FINAL_SCHEME: SchemeTracker(FINAL_SCHEME, [final_replay]),
    }


def tracker_result(tracker: SchemeTracker) -> Dict:
    replay = tracker.replays[0] if len(tracker.replays) == 1 else None
    result = {
        "metrics": tracker.metrics.to_dict(),
        "local_metrics": tracker.resource_results()[0]["metrics"],
        "resource": tracker.resource_results()[0],
    }
    if replay is not None:
        stats = tracker.replay_stats()[replay.cfg.name]["measurement"]
        result["phase"] = phase_summary(stats)
        result["measurement_stats"] = stats
    return result


def run_checkpoint(entry: Mapping, max_measurement_accesses: int) -> Dict:
    db_path = pathlib.Path(entry["result_dir"]) / "trace.db"
    clock_period_ticks = read_clock_period(db_path, None)
    connection = sqlite3.connect(str(db_path))
    marker_tick, marker_seq = read_marker(connection)
    trackers = build_trackers(clock_period_ticks)
    for tracker in trackers.values():
        resource = (
            DUAL_RESOURCE
            if tracker.name == "dual_current"
            else FINAL_RESOURCE
        )
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


def write_result(
    path: pathlib.Path,
    manifest: pathlib.Path,
    max_measurement_accesses: int,
    checkpoints: Sequence[Mapping],
) -> None:
    document = {
        "definition": (
            "Full warmup plus measurement replay of the selected merged "
            "TS-BOP. A shared Teacher emits family winners with strict "
            "VBOP score > 2 and PBOP score > 0 qualification. The two "
            "families maintain independent 4-bit issue-confidence gates."
        ),
        "manifest": str(manifest),
        "max_measurement_accesses": max_measurement_accesses,
        "final_parameters": FINAL_PARAMETERS,
        "final_resource": asdict(FINAL_RESOURCE),
        "checkpoints": list(checkpoints),
        "aggregate": aggregate(checkpoints),
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Evaluate the selected TS-BOP family-confidence mechanism"
    )
    parser.add_argument(
        "--manifest",
        type=pathlib.Path,
        default=pathlib.Path("util/ts_bop_degree/checkpoints.json"),
    )
    parser.add_argument("--out-json", required=True, type=pathlib.Path)
    parser.add_argument("--checkpoint-regex")
    parser.add_argument("--max-measurement-accesses", type=int, default=200000)
    parser.add_argument("--jobs", type=int, default=1)
    args = parser.parse_args()
    if args.max_measurement_accesses <= 0:
        raise ValueError("max measurement accesses must be positive")
    if args.jobs <= 0:
        raise ValueError("jobs must be positive")

    manifest = json.loads(args.manifest.read_text())
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
        args.max_measurement_accesses,
        ordered,
    )
    print(json.dumps(aggregate(ordered), indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
