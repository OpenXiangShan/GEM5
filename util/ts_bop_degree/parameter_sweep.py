#!/usr/bin/env python3

"""Quick static-trace sweep for the merged TS-BOP parameter candidates.

This tool intentionally keeps the functional gem5 BOP implementation untouched.
It reuses the ArchDB replay model and gives every candidate its own Student
state and output-resource model.
"""

import argparse
import concurrent.futures
import json
import pathlib
import re
import sqlite3
import sys
from dataclasses import asdict, dataclass
from typing import Dict, List, Mapping, Sequence, Tuple

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from util.ts_bop_degree.analyze import (  # noqa: E402
    BopReplay,
    ResourceConfig,
    SchemeTracker,
    iter_accesses,
    merged_config,
    pbop_config,
    read_clock_period,
    read_marker,
    vbop_config,
)
from util.ts_bop_degree.merged_multi_test import (  # noqa: E402
    DUAL_RESOURCE,
    MultiTestPartitionedReplay,
    phase_summary,
)


BASELINE_RESOURCE = ResourceConfig(
    name="f256_p8e4_3to1",
    primary_fifo_size=8,
    extra_fifo_size=4,
    primary_slots=3,
    extra_slots=1,
    recent_filter_entries=256,
)


@dataclass(frozen=True)
class Variant:
    name: str
    tests_per_access: int
    round_max: int
    score_max: int
    vbop_slots: int
    pbop_slots: int
    student_filter_entries: int
    extra_fifo_size: int
    recent_filter_entries: int
    primary_fifo_size: int = 8
    primary_slots: int = 3
    extra_slots: int = 1

    def resource(self) -> ResourceConfig:
        return ResourceConfig(
            name=(
                f"{self.name}_p{self.primary_fifo_size}"
                f"e{self.extra_fifo_size}_rf{self.recent_filter_entries}"
            ),
            primary_fifo_size=self.primary_fifo_size,
            extra_fifo_size=self.extra_fifo_size,
            primary_slots=self.primary_slots,
            extra_slots=self.extra_slots,
            recent_filter_entries=self.recent_filter_entries,
        )


# The first entry is the selected research default. The remaining entries
# retain the R25 sensitivity and the most useful score, pool/filter, FIFO,
# and recent-filter controls.
DEFAULT_VARIANTS = (
    Variant("selected_n2r50s20_v4p4_sf2048_e8_rf128", 2, 50, 20, 4, 4, 2048, 8, 128),
    Variant("sensitivity_n2r25s20_v4p4_sf2048_e8_rf128", 2, 25, 20, 4, 4, 2048, 8, 128),
    Variant("score_r50s16", 2, 50, 16, 4, 4, 2048, 8, 128),
    Variant("score_r50s31", 2, 50, 31, 4, 4, 2048, 8, 128),
    Variant("pool_v2p2", 2, 50, 20, 2, 2, 2048, 8, 128),
    Variant("student_filter_4096", 2, 50, 20, 4, 4, 4096, 8, 128),
    Variant("extra_fifo_4", 2, 50, 20, 4, 4, 2048, 4, 128),
    Variant("recent_filter_off", 2, 50, 20, 4, 4, 2048, 8, 0),
    Variant("recent_filter_256", 2, 50, 20, 4, 4, 2048, 8, 256),
)


def parse_variant(value: str) -> Variant:
    """Parse NAME:N:R:S:V:P:STUDENT_FILTER:EXTRA_FIFO:RECENT_FILTER."""

    fields = value.split(":")
    if len(fields) != 9:
        raise argparse.ArgumentTypeError(
            "variant must be NAME:N:R:S:V:P:STUDENT_FILTER:EXTRA_FIFO:"
            "RECENT_FILTER"
        )
    name = fields[0]
    if not name:
        raise argparse.ArgumentTypeError("variant name must not be empty")
    try:
        values = [int(field) for field in fields[1:]]
    except ValueError as error:
        raise argparse.ArgumentTypeError(
            "variant numeric fields must be integers"
        ) from error
    (
        tests_per_access,
        round_max,
        score_max,
        vbop_slots,
        pbop_slots,
        student_filter_entries,
        extra_fifo_size,
        recent_filter_entries,
    ) = values
    if min(
        tests_per_access,
        round_max,
        score_max,
        vbop_slots,
        pbop_slots,
        student_filter_entries,
        extra_fifo_size,
    ) <= 0:
        raise argparse.ArgumentTypeError(
            "variant positive fields must be greater than zero"
        )
    if recent_filter_entries < 0:
        raise argparse.ArgumentTypeError(
            "recent filter entries must not be negative"
        )
    if student_filter_entries & (student_filter_entries - 1):
        raise argparse.ArgumentTypeError(
            "student filter entries must be a power of two"
        )
    return Variant(
        name,
        tests_per_access,
        round_max,
        score_max,
        vbop_slots,
        pbop_slots,
        student_filter_entries,
        extra_fifo_size,
        recent_filter_entries,
    )


def build_trackers(
    clock_period_ticks: int,
    variants: Sequence[Variant],
) -> Dict[str, SchemeTracker]:
    dual = SchemeTracker(
        "dual_current",
        [
            BopReplay(vbop_config(clock_period_ticks), clock_period_ticks),
            BopReplay(pbop_config(clock_period_ticks), clock_period_ticks),
        ],
    )
    merged = SchemeTracker(
        "merged_d2",
        [
            BopReplay(
                merged_config(2, "greedy_marginal", 0.02, 1),
                clock_period_ticks,
            )
        ],
    )
    trackers = {
        dual.name: dual,
        merged.name: merged,
    }
    for variant in variants:
        replay = MultiTestPartitionedReplay(
            clock_period_ticks,
            variant.tests_per_access,
            variant.round_max,
            variant.score_max,
            vbop_slots=variant.vbop_slots,
            pbop_slots=variant.pbop_slots,
            student_filter_entries=variant.student_filter_entries,
            replay_name=variant.name,
        )
        trackers[variant.name] = SchemeTracker(
            variant.name,
            [replay],
        )
    return trackers


def local_sent_count(metrics: Mapping) -> int:
    return sum(metrics.get("sent_by_role", {}).values())


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
    return result


def run_checkpoint(
    entry: Mapping,
    variants: Sequence[Variant],
    max_measurement_accesses: int,
) -> Dict:
    db_path = pathlib.Path(entry["result_dir"]) / "trace.db"
    clock_period_ticks = read_clock_period(db_path, None)
    connection = sqlite3.connect(str(db_path))
    marker_tick, marker_seq = read_marker(connection)
    trackers = build_trackers(clock_period_ticks, variants)
    for tracker in trackers.values():
        if tracker.name == "dual_current":
            resource = DUAL_RESOURCE
        elif tracker.name == "merged_d2":
            resource = BASELINE_RESOURCE
        else:
            resource = next(
                variant.resource()
                for variant in variants
                if variant.name == tracker.name
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
                checkpoints, scheme, "metrics", "covered_count", "demand_count"
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
    variants: Sequence[Variant],
    max_measurement_accesses: int,
    checkpoints: Sequence[Mapping],
) -> None:
    document = {
        "definition": (
            "Quick static-trace sweep of one merged Teacher with a partitioned "
            "Student. Each candidate has an independent Student filter and "
            "output FIFO/recent-filter model. PBOP-family coverage is immediate "
            "and VBOP-family coverage uses the 300-cycle delay queue."
        ),
        "manifest": str(manifest),
        "max_measurement_accesses": max_measurement_accesses,
        "variants": [asdict(variant) for variant in variants],
        "baseline_resource": asdict(BASELINE_RESOURCE),
        "checkpoints": list(checkpoints),
        "aggregate": aggregate(checkpoints),
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Sweep merged TS-BOP parameters using static ArchDB traces"
    )
    parser.add_argument(
        "--manifest",
        type=pathlib.Path,
        default=pathlib.Path("util/ts_bop_degree/checkpoints.json"),
    )
    parser.add_argument("--out-json", required=True, type=pathlib.Path)
    parser.add_argument(
        "--variant",
        action="append",
        type=parse_variant,
        default=[],
        help=(
            "NAME:N:R:S:V:P:STUDENT_FILTER:EXTRA_FIFO:RECENT_FILTER; "
            "may be repeated"
        ),
    )
    parser.add_argument("--checkpoint-regex")
    parser.add_argument("--max-measurement-accesses", type=int, default=200000)
    parser.add_argument("--jobs", type=int, default=1)
    args = parser.parse_args()
    if args.max_measurement_accesses <= 0:
        raise ValueError("max measurement accesses must be positive")
    if args.jobs <= 0:
        raise ValueError("jobs must be positive")

    variants = tuple(args.variant or DEFAULT_VARIANTS)
    if len({variant.name for variant in variants}) != len(variants):
        raise ValueError("variant names must be unique")
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
                variants,
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
                variants,
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
        variants,
        args.max_measurement_accesses,
        ordered,
    )
    print(json.dumps(aggregate(ordered), indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
