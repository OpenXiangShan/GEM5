#!/usr/bin/env python3

import argparse
import concurrent.futures
import json
import pathlib
import re
import sqlite3
import sys
from dataclasses import asdict, replace
from typing import Dict, List, Mapping, Sequence, Tuple

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from util.ts_bop_degree.analyze import (
    BopReplay,
    ResourceConfig,
    SchemeTracker,
    iter_accesses,
    merged_config,
    read_clock_period,
    read_marker,
)


BASELINE_SCHEME = "merged_d2_greedy_marginal_th2_top1"
RESOURCE_NAME = "f256_p8e4_3to1"
RESOURCE_CONFIG = ResourceConfig(
    name=RESOURCE_NAME,
    primary_fifo_size=8,
    extra_fifo_size=4,
    primary_slots=3,
    extra_slots=1,
    recent_filter_entries=256,
)
DEFAULT_ROUND_MAX = (30, 40, 50, 65, 80)
DEFAULT_SCORE_MAX = (12, 20, 31, 40)


def load_json(path: pathlib.Path) -> Dict:
    return json.loads(path.read_text())


def parse_param(value: str) -> Tuple[int, int]:
    try:
        round_max, score_max = (int(item) for item in value.split(":"))
    except (TypeError, ValueError) as error:
        raise argparse.ArgumentTypeError(
            "parameter must be ROUND_MAX:SCORE_MAX"
        ) from error
    if round_max <= 0 or score_max <= 0:
        raise argparse.ArgumentTypeError(
            "round_max and score_max must be positive"
        )
    if score_max > round_max:
        raise argparse.ArgumentTypeError(
            "score_max cannot exceed round_max because one offset scores at "
            "most once per round"
        )
    return round_max, score_max


def default_params() -> List[Tuple[int, int]]:
    params = {
        (round_max, score_max)
        for round_max in DEFAULT_ROUND_MAX
        for score_max in DEFAULT_SCORE_MAX
        if score_max <= round_max
    }
    params.add((33, 31))
    params.add((50, 31))
    return sorted(params)


def scheme_name(round_max: int, score_max: int) -> str:
    return f"merged_d2_r{round_max}_s{score_max}"


def find_scheme(document: Mapping, name: str) -> Mapping:
    return next(item for item in document["schemes"] if item["name"] == name)


def find_resource(scheme: Mapping, name: str) -> Mapping:
    return next(
        item for item in scheme.get("resources", []) if item["name"] == name
    )


def load_references(result_dir: pathlib.Path) -> Dict:
    dual_document = load_json(result_dir / "resources_dual.json")
    merged_document = load_json(result_dir / "resources_filter.json")
    dual = find_scheme(dual_document, "dual_current")
    merged = find_scheme(merged_document, BASELINE_SCHEME)
    return {
        "dual": {
            "metrics": dual["metrics"],
            "local_metrics": find_resource(
                dual, "dual_p16e16_strict"
            )["metrics"],
        },
        "merged_baseline": {
            "metrics": merged["metrics"],
            "local_metrics": find_resource(
                merged, RESOURCE_NAME
            )["metrics"],
        },
    }


def stat_distribution(stats: Mapping[str, int], prefix: str) -> Dict[str, int]:
    return {
        name[len(prefix):]: int(value)
        for name, value in sorted(stats.items())
        if name.startswith(prefix) and value
    }


def phase_summary(stats: Mapping[str, int]) -> Dict:
    phases = stats.get("teacher_phases", 0)
    score_phases = stats.get("teacher_score_max_phases", 0)
    issue_enabled = stats.get("teacher_issue_enabled_phases", 0)
    return {
        "phase_count": phases,
        "average_rounds": (
            stats.get("teacher_rounds_sum", 0) / phases if phases else 0.0
        ),
        "average_best_score": (
            stats.get("teacher_best_score_sum", 0) / phases
            if phases
            else 0.0
        ),
        "score_max_end_fraction": score_phases / phases if phases else 0.0,
        "round_max_end_fraction": (
            stats.get("teacher_round_max_phases", 0) / phases
            if phases
            else 0.0
        ),
        "issue_enabled_fraction": issue_enabled / phases if phases else 0.0,
        "teacher_injected": stats.get("teacher_injected", 0),
        "student_issue_phases": stats.get("student_issue_phases", 0),
    }


def replay_config(
    clock_period_ticks: int,
    round_max: int,
    score_max: int,
) -> SchemeTracker:
    name = scheme_name(round_max, score_max)
    config = replace(
        merged_config(2, "greedy_marginal", 0.02, 1),
        name=name,
        round_max=round_max,
        score_max=score_max,
    )
    return SchemeTracker(name, [BopReplay(config, clock_period_ticks)])


def local_sent_count(metrics: Mapping) -> int:
    return sum(metrics.get("sent_by_role", {}).values())


def run_checkpoint(
    entry: Mapping,
    params: Sequence[Tuple[int, int]],
) -> Dict:
    result_dir = pathlib.Path(entry["result_dir"])
    db_path = result_dir / "trace.db"
    clock_period_ticks = read_clock_period(db_path, None)
    connection = sqlite3.connect(str(db_path))
    marker_tick, marker_seq = read_marker(connection)
    trackers = [
        replay_config(clock_period_ticks, round_max, score_max)
        for round_max, score_max in params
    ]
    for tracker in trackers:
        tracker.attach_resources(
            [RESOURCE_CONFIG],
            clock_period_ticks,
            marker_tick,
            marker_seq,
        )

    access_count = 0
    for access in iter_accesses(connection):
        access_count += 1
        measuring = access.seq > marker_seq
        for tracker in trackers:
            tracker.process(access, measuring)
        if access.seq == marker_seq:
            for tracker in trackers:
                tracker.mark_boundary()
    connection.close()

    schemes = {}
    for tracker in trackers:
        replay = tracker.replays[0]
        measurement_stats = tracker.replay_stats()[replay.cfg.name][
            "measurement"
        ]
        local_metrics = tracker.resource_results()[0]["metrics"]
        schemes[tracker.name] = {
            "config": {
                "round_max": replay.cfg.round_max,
                "score_max": replay.cfg.score_max,
                "score_ratio": replay.cfg.score_max / replay.cfg.round_max,
                "degree": replay.cfg.degree,
                "extra_threshold": replay.cfg.extra_threshold,
            },
            "metrics": tracker.metrics.to_dict(),
            "local_metrics": local_metrics,
            "phase": phase_summary(measurement_stats),
            "offset_stats": {
                "teacher_winner": stat_distribution(
                    measurement_stats, "teacher_winner_offset::"
                ),
                "teacher_injected": stat_distribution(
                    measurement_stats, "teacher_injected_offset::"
                ),
                "issued": stat_distribution(
                    measurement_stats, "issued_offset::"
                ),
            },
        }

    return {
        "name": entry["name"],
        "workload": entry["workload"],
        "weight": entry["weight"],
        "result_dir": str(result_dir),
        "db": str(db_path),
        "access_count": access_count,
        "references": load_references(result_dir),
        "schemes": schemes,
    }


def run_checkpoint_param(
    entry: Mapping,
    param: Tuple[int, int],
) -> Dict:
    return run_checkpoint(entry, [param])


def merge_checkpoint_result(
    current: Mapping,
    update: Mapping,
) -> Dict:
    merged = dict(current)
    schemes = dict(current.get("schemes", {}))
    schemes.update(update["schemes"])
    merged["schemes"] = schemes
    return merged


def weighted_mean(
    checkpoints: Sequence[Mapping],
    selector,
    weight_selector,
) -> float:
    weighted_sum = 0.0
    weight_sum = 0.0
    for checkpoint in checkpoints:
        weight = float(weight_selector(checkpoint))
        weighted_sum += float(selector(checkpoint)) * weight
        weight_sum += weight
    return weighted_sum / weight_sum if weight_sum else 0.0


def ratio_aggregate(
    checkpoints: Sequence[Mapping],
    selector,
    numerator: str,
    denominator: str,
) -> Dict[str, float]:
    checkpoint_weighted = weighted_mean(
        checkpoints,
        lambda checkpoint: (
            selector(checkpoint)[numerator]
            / selector(checkpoint)[denominator]
            if selector(checkpoint)[denominator]
            else 0.0
        ),
        lambda checkpoint: checkpoint["weight"],
    )
    numerator_sum = sum(selector(item)[numerator] for item in checkpoints)
    denominator_sum = sum(selector(item)[denominator] for item in checkpoints)
    return {
        "checkpoint_weighted": checkpoint_weighted,
        "access_weighted": (
            numerator_sum / denominator_sum if denominator_sum else 0.0
        ),
    }


def sent_ratio_aggregate(
    checkpoints: Sequence[Mapping],
    selector,
) -> Dict[str, float]:
    checkpoint_weighted = weighted_mean(
        checkpoints,
        lambda checkpoint: (
            local_sent_count(selector(checkpoint))
            / selector(checkpoint)["demand_count"]
            if selector(checkpoint)["demand_count"]
            else 0.0
        ),
        lambda checkpoint: checkpoint["weight"],
    )
    sent_sum = sum(local_sent_count(selector(item)) for item in checkpoints)
    demand_sum = sum(selector(item)["demand_count"] for item in checkpoints)
    return {
        "checkpoint_weighted": checkpoint_weighted,
        "access_weighted": sent_sum / demand_sum if demand_sum else 0.0,
    }


def aggregate_reference(
    checkpoints: Sequence[Mapping],
    reference: str,
) -> Dict:
    def ideal(checkpoint):
        return checkpoint["references"][reference]["metrics"]

    def local(checkpoint):
        return checkpoint["references"][reference]["local_metrics"]

    return {
        "ideal_coverage": ratio_aggregate(
            checkpoints, ideal, "covered_count", "demand_count"
        ),
        "local_coverage": ratio_aggregate(
            checkpoints, local, "covered_count", "demand_count"
        ),
        "generated_per_demand": ratio_aggregate(
            checkpoints, ideal, "generated_count", "demand_count"
        ),
        "local_sent_per_demand": sent_ratio_aggregate(checkpoints, local),
    }


def aggregate_scheme(
    checkpoints: Sequence[Mapping],
    name: str,
) -> Dict:
    def ideal(checkpoint):
        return checkpoint["schemes"][name]["metrics"]

    def local(checkpoint):
        return checkpoint["schemes"][name]["local_metrics"]

    def phase(checkpoint):
        return checkpoint["schemes"][name]["phase"]

    total_phases = sum(phase(item)["phase_count"] for item in checkpoints)
    total_demands = sum(ideal(item)["demand_count"] for item in checkpoints)
    config = checkpoints[0]["schemes"][name]["config"]
    return {
        "config": config,
        "ideal_coverage": ratio_aggregate(
            checkpoints, ideal, "covered_count", "demand_count"
        ),
        "local_coverage": ratio_aggregate(
            checkpoints, local, "covered_count", "demand_count"
        ),
        "generated_per_demand": ratio_aggregate(
            checkpoints, ideal, "generated_count", "demand_count"
        ),
        "local_sent_per_demand": sent_ratio_aggregate(checkpoints, local),
        "phase": {
            "count": total_phases,
            "per_million_demands": (
                total_phases * 1_000_000 / total_demands
                if total_demands
                else 0.0
            ),
            "average_rounds": weighted_mean(
                checkpoints,
                lambda checkpoint: phase(checkpoint)["average_rounds"],
                lambda checkpoint: phase(checkpoint)["phase_count"],
            ),
            "average_best_score": weighted_mean(
                checkpoints,
                lambda checkpoint: phase(checkpoint)["average_best_score"],
                lambda checkpoint: phase(checkpoint)["phase_count"],
            ),
            "score_max_end_fraction": weighted_mean(
                checkpoints,
                lambda checkpoint: phase(checkpoint)[
                    "score_max_end_fraction"
                ],
                lambda checkpoint: phase(checkpoint)["phase_count"],
            ),
            "issue_enabled_fraction": weighted_mean(
                checkpoints,
                lambda checkpoint: phase(checkpoint)["issue_enabled_fraction"],
                lambda checkpoint: phase(checkpoint)["phase_count"],
            ),
        },
    }


def aggregate(
    checkpoints: Sequence[Mapping],
    params: Sequence[Tuple[int, int]],
) -> Dict:
    return {
        "references": {
            name: aggregate_reference(checkpoints, name)
            for name in ("dual", "merged_baseline")
        },
        "schemes": {
            scheme_name(round_max, score_max): aggregate_scheme(
                checkpoints, scheme_name(round_max, score_max)
            )
            for round_max, score_max in params
        },
    }


def write_result(
    path: pathlib.Path,
    manifest: pathlib.Path,
    params: Sequence[Tuple[int, int]],
    checkpoints: Sequence[Mapping],
) -> None:
    result = {
        "manifest": str(manifest),
        "definition": (
            "Open-loop merged VBOP replay varying only Teacher round_max and "
            "score_max; degree=2, greedy marginal threshold=2%, pool=4, and "
            "the fixed local FIFO/filter/arbiter model are unchanged"
        ),
        "resource_config": asdict(RESOURCE_CONFIG),
        "params": [
            {"round_max": round_max, "score_max": score_max}
            for round_max, score_max in params
        ],
        "checkpoints": list(checkpoints),
        "aggregate": aggregate(checkpoints, params),
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Sweep merged TS-BOP Teacher round_max and score_max"
    )
    parser.add_argument(
        "--manifest",
        type=pathlib.Path,
        default=pathlib.Path("util/ts_bop_degree/checkpoints.json"),
    )
    parser.add_argument("--out-json", required=True, type=pathlib.Path)
    parser.add_argument(
        "--param",
        action="append",
        type=parse_param,
        default=[],
        help="ROUND_MAX:SCORE_MAX; may be repeated",
    )
    parser.add_argument("--checkpoint-regex")
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument(
        "--parallel-params",
        action="store_true",
        help="parallelize each checkpoint/parameter pair independently",
    )
    parser.add_argument("--resume", action="store_true")
    args = parser.parse_args()

    if args.jobs <= 0:
        raise ValueError("jobs must be positive")
    params = sorted(set(args.param or default_params()))
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
        previous_params = [
            (item["round_max"], item["score_max"])
            for item in previous.get("params", [])
        ]
        if previous_params != params:
            raise ValueError("resume file uses a different parameter set")
        completed = {
            item["name"]: item for item in previous.get("checkpoints", [])
        }

    expected_schemes = {
        scheme_name(round_max, score_max)
        for round_max, score_max in params
    }
    results = {
        name: result
        for name, result in completed.items()
        if expected_schemes <= set(result.get("schemes", {}))
    }
    pending_entries = [
        entry for entry in selected if entry["name"] not in results
    ]

    if args.parallel_params:
        tasks = [
            (entry, param)
            for entry in pending_entries
            for param in params
        ]
        with concurrent.futures.ProcessPoolExecutor(
            max_workers=min(args.jobs, len(tasks)) if tasks else 1
        ) as executor:
            futures = {
                executor.submit(run_checkpoint_param, entry, param): (
                    entry,
                    param,
                )
                for entry, param in tasks
            }
            partial: Dict[str, Mapping] = {}
            for future in concurrent.futures.as_completed(futures):
                entry, param = futures[future]
                result = future.result()
                current = partial.get(entry["name"], result)
                partial[entry["name"]] = merge_checkpoint_result(
                    current, result
                )
                scheme_count = len(partial[entry["name"]]["schemes"])
                if scheme_count == len(params):
                    results[entry["name"]] = partial[entry["name"]]
                    ordered_results = [
                        results[item["name"]]
                        for item in selected
                        if item["name"] in results
                    ]
                    write_result(
                        args.out_json,
                        args.manifest,
                        params,
                        ordered_results,
                    )
                print(
                    json.dumps({
                        "checkpoint": entry["name"],
                        "round_max": param[0],
                        "score_max": param[1],
                        "parameter_results": scheme_count,
                        "parameter_total": len(params),
                    }),
                    flush=True,
                )
    else:
        with concurrent.futures.ProcessPoolExecutor(
            max_workers=(
                min(args.jobs, len(pending_entries))
                if pending_entries
                else 1
            )
        ) as executor:
            futures = {
                executor.submit(run_checkpoint, entry, params): entry
                for entry in pending_entries
            }
            for future in concurrent.futures.as_completed(futures):
                entry = futures[future]
                result = future.result()
                results[entry["name"]] = result
                ordered_results = [
                    results[item["name"]]
                    for item in selected
                    if item["name"] in results
                ]
                write_result(
                    args.out_json, args.manifest, params, ordered_results
                )
                print(
                    json.dumps({
                        "checkpoint": entry["name"],
                        "completed": len(ordered_results),
                        "selected": len(selected),
                    }),
                    flush=True,
                )

    ordered_results = [results[item["name"]] for item in selected]
    write_result(args.out_json, args.manifest, params, ordered_results)


if __name__ == "__main__":
    main()
