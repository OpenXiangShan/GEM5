#!/usr/bin/env python3
"""Sweep shared BOP PC/global-controller parameters over certified traces.

The input directory is produced by ``run_spec06_bop_trace_batch.py``.  This
tool never changes the online GEM5 trace.  Each policy point is a fresh
offline replay whose global feedback is driven by that point's demand oracle.
Reports are evaluated only in the V5 ``stable`` phase.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import logging
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MANIFEST = Path(__file__).with_name("spec06_bop_policy_manifest.json")
DEFAULT_POINTS = Path(__file__).with_name("spec06_bop_controller_oat_v1.json")


@dataclass(frozen=True)
class Point:
    name: str
    overrides: dict[str, object]


@dataclass(frozen=True)
class SweepResult:
    case: str
    point: str
    status: str
    detail: str


def load_case_names(manifest_path: Path, selected: set[str]) -> list[str]:
    payload = json.loads(manifest_path.read_text())
    names = [str(entry["name"]) for entry in payload["cases"]]
    if selected:
        unknown = sorted(selected - set(names))
        if unknown:
            raise ValueError("unknown case(s): " + ", ".join(unknown))
        names = [name for name in names if name in selected]
    if not names:
        raise ValueError("no cases selected")
    return names


def load_points(points_path: Path, selected: set[str]) -> tuple[list[Point], list[int]]:
    payload = json.loads(points_path.read_text())
    points = [
        Point(str(entry["name"]), dict(entry.get("overrides", {})))
        for entry in payload["points"]
    ]
    if selected:
        unknown = sorted(selected - {point.name for point in points})
        if unknown:
            raise ValueError("unknown point(s): " + ", ".join(unknown))
        points = [point for point in points if point.name in selected]
    names = [point.name for point in points]
    if len(names) != len(set(names)):
        raise ValueError("point names must be unique")
    if not points:
        raise ValueError("no sweep points selected")
    horizons = [int(value) for value in payload.get("horizons", [512, 2048])]
    if not horizons or any(value <= 0 for value in horizons):
        raise ValueError("points file has invalid horizons")
    return points, horizons


def raw_verified(case_dir: Path) -> bool:
    report_path = case_dir / "raw-baseline.json"
    trace_path = case_dir / "trace.db"
    if not trace_path.is_file() or not report_path.is_file():
        return False
    try:
        report = json.loads(report_path.read_text())
        import sqlite3
        with sqlite3.connect(trace_path) as connection:
            row = connection.execute(
                "SELECT SchemaVersion FROM BOPReplayMeta LIMIT 1"
            ).fetchone()
    except json.JSONDecodeError:
        return False
    except sqlite3.Error:
        return False
    return bool(
        row and int(row[0]) in (5, 6)
        and report.get("verification", {}).get("pass")
    )


def valid_policy_report(
    path: Path, horizons: Iterable[int], controller_only: bool,
) -> bool:
    if not path.is_file():
        return False
    try:
        report = json.loads(path.read_text())
    except json.JSONDecodeError:
        return False
    if controller_only:
        if report.get("mode") != "replay-controller":
            return False
        if report.get("replay_engine", {}).get("engine") != "streaming":
            return False
    elif (report.get("mode") != "learner-replay" or
          report.get("candidate_stage") != "policy"):
        return False
    return all(str(horizon) in report.get("horizons", {}) for horizon in horizons)


def run_case(
    case: str, points: list[Point], horizons: list[int], output_root: Path,
    resume: bool, controller_only: bool,
) -> list[SweepResult]:
    case_dir = output_root / case
    if not raw_verified(case_dir):
        return [SweepResult(case, "*", "trace_not_certified", "raw baseline missing or failed")]

    trace_db = case_dir / "trace.db"
    policy_dir = case_dir / "policy"
    config_dir = case_dir / "controller-configs"
    policy_dir.mkdir(exist_ok=True)
    config_dir.mkdir(exist_ok=True)
    results: list[SweepResult] = []
    for point in points:
        config_path = config_dir / f"{point.name}.json"
        output_path = policy_dir / f"{point.name}.json"
        config_path.write_text(json.dumps(point.overrides, indent=2, sort_keys=True) + "\n")
        if resume and valid_policy_report(output_path, horizons, controller_only):
            results.append(SweepResult(case, point.name, "skipped", "existing policy report"))
            continue
        command = [
            sys.executable,
            str(REPO_ROOT / "util" / "bop_replay" / "bop_replay.py"),
            str(trace_db),
            "--mode=replay-controller" if controller_only else "--mode=learner-replay",
            "--evaluation-phase=stable",
            "--replay-engine=streaming",
            f"--horizons={','.join(str(value) for value in horizons)}",
            f"--controller-config={config_path}",
            f"--output={output_path}",
        ]
        if not controller_only:
            command.append("--candidate-stage=policy")
        completed = subprocess.run(command, capture_output=True, text=True)
        (policy_dir / f"{point.name}.stdout").write_text(completed.stdout)
        (policy_dir / f"{point.name}.stderr").write_text(completed.stderr)
        if completed.returncode != 0:
            results.append(SweepResult(case, point.name, "replay_failed", f"exit={completed.returncode}"))
        elif not valid_policy_report(output_path, horizons, controller_only):
            results.append(SweepResult(case, point.name, "report_failed", "missing expected horizon"))
        else:
            results.append(SweepResult(case, point.name, "passed", "policy replay complete"))
    return results


def metrics(report: dict[str, Any], horizon: int) -> dict[str, Any]:
    return dict(report["horizons"][str(horizon)]["combined"])


def pareto_front(entries: list[dict[str, Any]]) -> list[str]:
    names: list[str] = []
    for candidate in entries:
        dominated = any(
            other["accuracy"] >= candidate["accuracy"]
            and other["coverage"] >= candidate["coverage"]
            and (
                other["accuracy"] > candidate["accuracy"]
                or other["coverage"] > candidate["coverage"]
            )
            for other in entries
            if other["point"] != candidate["point"]
        )
        if not dominated:
            names.append(str(candidate["point"]))
    return sorted(names)


def aggregate(metrics_list: list[dict[str, Any]]) -> dict[str, Any]:
    fields = ("candidates", "useful", "unused", "redundant", "censored", "eligible_demands", "covered_demands")
    total = {field: sum(int(item[field]) for item in metrics_list) for field in fields}
    denominator = total["useful"] + total["unused"] + total["redundant"]
    total["accuracy"] = total["useful"] / denominator if denominator else None
    total["coverage"] = (
        total["covered_demands"] / total["eligible_demands"]
        if total["eligible_demands"] else None
    )
    return total


def build_summary(
    case_names: list[str], points: list[Point], horizons: list[int], output_root: Path,
    results: list[SweepResult], controller_only: bool,
) -> dict[str, Any]:
    result_index = {(item.case, item.point): item for item in results}
    cases: dict[str, Any] = {}
    aggregate_by_horizon: dict[str, list[dict[str, Any]]] = {
        str(horizon): [] for horizon in horizons
    }
    for case in case_names:
        case_dir = output_root / case
        case_report: dict[str, Any] = {"status": "missing", "points": {}}
        if not raw_verified(case_dir):
            case_report["status"] = "trace_not_certified"
            cases[case] = case_report
            continue
        raw_report = json.loads((case_dir / "raw-baseline.json").read_text())
        case_report["status"] = "ready"
        case_report["raw"] = {
            str(horizon): metrics(raw_report, horizon) for horizon in horizons
        }
        for point in points:
            result = result_index.get((case, point.name))
            report_path = case_dir / "policy" / f"{point.name}.json"
            point_record: dict[str, Any] = {
                "status": result.status if result else "not_run",
                "overrides": point.overrides,
            }
            if valid_policy_report(report_path, horizons, controller_only):
                report = json.loads(report_path.read_text())
                point_record["metrics"] = {
                    str(horizon): metrics(report, horizon) for horizon in horizons
                }
                if "controller_stats" in report:
                    point_record["controller_stats"] = report["controller_stats"]
                for horizon in horizons:
                    policy_metrics = point_record["metrics"][str(horizon)]
                    raw_metrics = case_report["raw"][str(horizon)]
                    point_record.setdefault("delta", {})[str(horizon)] = {
                        "accuracy": policy_metrics["accuracy"] - raw_metrics["accuracy"],
                        "coverage": policy_metrics["coverage"] - raw_metrics["coverage"],
                        "candidates": policy_metrics["candidates"] - raw_metrics["candidates"],
                        "useful": policy_metrics["useful"] - raw_metrics["useful"],
                    }
            case_report["points"][point.name] = point_record
        cases[case] = case_report

    for horizon in horizons:
        raw_metrics = [
            case_report["raw"][str(horizon)]
            for case_report in cases.values()
            if "raw" in case_report
        ]
        raw_total = aggregate(raw_metrics) if raw_metrics else None
        for point in points:
            policy_metrics = [
                case_report["points"][point.name]["metrics"][str(horizon)]
                for case_report in cases.values()
                if "metrics" in case_report.get("points", {}).get(point.name, {})
            ]
            if len(policy_metrics) != len(raw_metrics) or not policy_metrics:
                continue
            total = aggregate(policy_metrics)
            aggregate_entry = {
                "point": point.name,
                "overrides": point.overrides,
                **total,
                "accuracy_delta_vs_raw": total["accuracy"] - raw_total["accuracy"],
                "coverage_delta_vs_raw": total["coverage"] - raw_total["coverage"],
                "candidate_delta_vs_raw": total["candidates"] - raw_total["candidates"],
            }
            controller_stats = [
                case_report["points"][point.name]
                    .get("controller_stats", {}).get(str(horizon))
                for case_report in cases.values()
                if "metrics" in case_report.get("points", {}).get(point.name, {})
            ]
            if controller_stats and all(stats is not None for stats in controller_stats):
                count_fields = (
                    "table_lookups", "table_hits", "table_misses",
                    "table_replacements", "offset_context_hits",
                    "offset_context_misses", "offset_context_replacements",
                    "epoch_resets", "offset_epoch_changes",
                )
                aggregate_entry["controller_stats"] = {
                    "offset_context_slots": controller_stats[0]["offset_context_slots"],
                    **{
                        field: sum(int(stats[field]) for stats in controller_stats)
                        for field in count_fields
                    },
                }
            aggregate_by_horizon[str(horizon)].append(aggregate_entry)

    aggregates = {}
    for horizon in horizons:
        entries = aggregate_by_horizon[str(horizon)]
        raw_metrics = [
            case_report["raw"][str(horizon)]
            for case_report in cases.values()
            if "raw" in case_report
        ]
        aggregates[str(horizon)] = {
            "raw": aggregate(raw_metrics) if raw_metrics else None,
            "points": sorted(entries, key=lambda entry: entry["point"]),
            "pareto_front": pareto_front(entries),
        }
    return {
        "replay_mode": "controller-only" if controller_only else "learner-policy",
        "horizons": horizons,
        "cases": cases,
        "aggregate": aggregates,
        "execution": [
            {"case": item.case, "point": item.point, "status": item.status, "detail": item.detail}
            for item in sorted(results, key=lambda item: (item.case, item.point))
        ],
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Offline BOP PC/global controller sweep")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--points", type=Path, default=DEFAULT_POINTS)
    parser.add_argument(
        "--output-root", type=Path,
        default=Path("/nfs/home/lijiangtao/temp/bop-replay/spec06-bop-policy-sweep-20260813"),
    )
    parser.add_argument("--workers", type=int, default=8)
    parser.add_argument("--case", action="append", default=[])
    parser.add_argument("--point", action="append", default=[])
    parser.add_argument(
        "--summary-path", type=Path, default=None,
        help="optional summary JSON path; defaults to <output-root>/policy-sweep-summary.json",
    )
    parser.add_argument("--resume", action="store_true")
    parser.add_argument(
        "--controller-only", action="store_true",
        help="reuse recorded raw candidates with bounded streaming controller replay",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.workers <= 0:
        raise ValueError("workers must be positive")
    case_names = load_case_names(args.manifest, set(args.case))
    points, horizons = load_points(args.points, set(args.point))
    summary_path = args.summary_path or args.output_root / "policy-sweep-summary.json"
    summary_path.parent.mkdir(parents=True, exist_ok=True)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    logging.info(
        "sweeping %d point(s) across %d case(s) with workers=%d",
        len(points), len(case_names), args.workers,
    )
    results: list[SweepResult] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as executor:
        futures = {
            executor.submit(
                run_case, case, points, horizons, args.output_root, args.resume,
                args.controller_only,
            ): case
            for case in case_names
        }
        for future in concurrent.futures.as_completed(futures):
            case = futures[future]
            try:
                case_results = future.result()
            except Exception as error:
                case_results = [SweepResult(case, "*", "runner_failed", str(error))]
            results.extend(case_results)
            logging.info("%s: %s", case, ", ".join(
                f"{item.point}={item.status}" for item in case_results
            ))
            summary = build_summary(
                case_names, points, horizons, args.output_root, results,
                args.controller_only,
            )
            summary_path.write_text(
                json.dumps(summary, indent=2, sort_keys=True) + "\n"
            )
    summary = build_summary(
        case_names, points, horizons, args.output_root, results,
        args.controller_only,
    )
    summary_path.write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n"
    )
    failures = [item for item in results if item.status not in {"passed", "skipped"}]
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
