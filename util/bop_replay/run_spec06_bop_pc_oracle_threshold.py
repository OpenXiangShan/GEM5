#!/usr/bin/env python3
"""Run stable BOP PC-quality oracle thresholds on certified SPEC06 traces."""

from __future__ import annotations

import argparse
import concurrent.futures
import csv
import json
import logging
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Mapping, Sequence

import analyze_bop_pc_oracle_threshold as oracle


DEFAULT_MANIFEST = Path(__file__).with_name("spec06_bop_policy_manifest.json")
DEFAULT_TRACE_ROOT = Path(
    "/nfs/home/lijiangtao/temp/bop-replay/spec06-bop-v5-certification-20260813"
)
DEFAULT_OUTPUT_ROOT = Path(
    "/nfs/home/lijiangtao/temp/bop-replay/pc-quality-oracle-threshold-20260817"
)
DEFAULT_CONTROLLER_CONFIG = Path(__file__).with_name("producer_consumer_k2.json")


@dataclass(frozen=True)
class CaseResult:
    case: str
    status: str
    detail: str


def load_case_names(manifest_path: Path, selected: set[str]) -> list[str]:
    payload = json.loads(manifest_path.read_text())
    names = [str(item["name"]) for item in payload["cases"]]
    if selected:
        unknown = sorted(selected - set(names))
        if unknown:
            raise ValueError("unknown case(s): " + ", ".join(unknown))
        names = [name for name in names if name in selected]
    if not names:
        raise ValueError("no cases selected")
    return names


def raw_certified(trace_root: Path, case: str) -> bool:
    database = trace_root / case / "trace.db"
    baseline = trace_root / case / "raw-baseline.json"
    if not database.is_file() or not baseline.is_file():
        return False
    try:
        payload = json.loads(baseline.read_text())
    except json.JSONDecodeError:
        return False
    return bool(payload.get("verification", {}).get("pass"))


def valid_report(path: Path, thresholds: Sequence[float]) -> bool:
    if not path.is_file():
        return False
    try:
        payload = json.loads(path.read_text())
    except json.JSONDecodeError:
        return False
    return bool(
        payload.get("owner_reconstruction", {}).get("pass")
        and payload.get("raw_replay_certification", {}).get("pass")
        and all(
            oracle.threshold_name(threshold) in payload.get("oracle_thresholds", {})
            for threshold in thresholds
        )
    )


def run_case(
    case: str, trace_root: Path, output_root: Path,
    controller_overrides: Mapping[str, object], thresholds: Sequence[float],
    evaluation_phase: str, resume: bool,
) -> CaseResult:
    if not raw_certified(trace_root, case):
        return CaseResult(case, "trace_not_certified", "raw trace certification missing")
    output_path = output_root / case / "oracle-threshold.json"
    if resume and valid_report(output_path, thresholds):
        return CaseResult(case, "skipped", "existing certified report")
    try:
        report = oracle.analyze_case(
            trace_root / case / "trace.db", controller_overrides, thresholds,
            evaluation_phase,
        )
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    except Exception as error:
        return CaseResult(case, "failed", str(error))
    return CaseResult(case, "passed", "oracle threshold analysis complete")


def aggregate(metrics: Sequence[Mapping[str, object]]) -> dict[str, object]:
    total = {
        field: sum(int(item[field]) for item in metrics)
        for field in oracle.COUNT_FIELDS
    }
    denominator = total["useful"] + total["unused"] + total["redundant"]
    total["accuracy"] = total["useful"] / denominator if denominator else None
    total["coverage"] = (
        total["covered_demands"] / total["eligible_demands"]
        if total["eligible_demands"] else None
    )
    total["coverage_contribution"] = total["coverage"]
    return total


def build_summary(
    cases: Sequence[str], output_root: Path, thresholds: Sequence[float],
    results: Sequence[CaseResult],
) -> dict[str, object]:
    status = {item.case: item for item in results}
    case_reports: dict[str, object] = {}
    metric_sets: dict[str, list[Mapping[str, object]]] = {
        "raw": [], "current": [],
        **{oracle.threshold_name(threshold): [] for threshold in thresholds},
    }
    for case in cases:
        path = output_root / case / "oracle-threshold.json"
        case_status = status.get(case)
        if not valid_report(path, thresholds):
            case_reports[case] = {
                "status": case_status.status if case_status else "not_run",
                "detail": case_status.detail if case_status else "report_missing",
            }
            continue
        payload = json.loads(path.read_text())
        points = {
            name: point["quality"]["combined"]
            for name, point in payload["oracle_thresholds"].items()
        }
        record = {
            "status": "ready",
            "raw": payload["quality"]["raw"],
            "current": payload["quality"]["current"],
            "oracle_thresholds": points,
        }
        case_reports[case] = record
        metric_sets["raw"].append(record["raw"])
        metric_sets["current"].append(record["current"])
        for threshold in thresholds:
            metric_sets[oracle.threshold_name(threshold)].append(
                points[oracle.threshold_name(threshold)]
            )

    aggregate_points = {
        name: aggregate(items) if items and len(items) == len(metric_sets["raw"])
        else None
        for name, items in metric_sets.items()
    }
    raw = aggregate_points["raw"]
    current = aggregate_points["current"]
    oracle_points = {}
    for threshold in thresholds:
        name = oracle.threshold_name(threshold)
        quality = aggregate_points[name]
        if quality is None or raw is None or current is None:
            oracle_points[name] = None
            continue
        oracle_points[name] = {
            "threshold_fraction": threshold,
            "threshold_percent": threshold * 100.0,
            "quality": quality,
            "delta_vs_raw": oracle._delta(quality, raw),
            "delta_vs_current": oracle._delta(quality, current),
        }
    return {
        "model": {
            "horizon": oracle.HORIZON,
            "quality_window": "stable phase only",
            "oracle_label": "raw combined issuer-PC accuracy",
            "non_causal": True,
        },
        "case_count": len(cases),
        "ready_case_count": len(metric_sets["raw"]),
        "thresholds_percent": [threshold * 100.0 for threshold in thresholds],
        "aggregate": {
            "raw": raw,
            "current": current,
            "oracle_thresholds": oracle_points,
        },
        "cases": case_reports,
        "execution": [
            {"case": item.case, "status": item.status, "detail": item.detail}
            for item in sorted(results, key=lambda item: item.case)
        ],
    }


def write_csv(summary: Mapping[str, object], path: Path) -> None:
    rows = []
    for case, record in summary["cases"].items():
        if record.get("status") != "ready":
            continue
        rows.extend([
            (case, "raw", record["raw"]),
            (case, "current", record["current"]),
            *[
                (case, name, metrics)
                for name, metrics in record["oracle_thresholds"].items()
            ],
        ])
    aggregate = summary["aggregate"]
    if aggregate["raw"] is not None:
        rows.extend([
            ("aggregate", "raw", aggregate["raw"]),
            ("aggregate", "current", aggregate["current"]),
            *[
                ("aggregate", name, point["quality"])
                for name, point in aggregate["oracle_thresholds"].items()
                if point is not None
            ],
        ])
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(
            stream,
            fieldnames=("scope", "point", *oracle.COUNT_FIELDS, "accuracy", "coverage"),
        )
        writer.writeheader()
        for scope, point, metrics in rows:
            writer.writerow({
                "scope": scope,
                "point": point,
                **{field: metrics[field] for field in oracle.COUNT_FIELDS},
                "accuracy": metrics["accuracy"],
                "coverage": metrics["coverage"],
            })


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--trace-root", type=Path, default=DEFAULT_TRACE_ROOT)
    parser.add_argument("--output-root", type=Path, default=DEFAULT_OUTPUT_ROOT)
    parser.add_argument("--controller-config", type=Path, default=DEFAULT_CONTROLLER_CONFIG)
    parser.add_argument("--thresholds", default="5,10,15,20")
    parser.add_argument("--evaluation-phase", default="stable")
    parser.add_argument("--workers", type=int, default=8)
    parser.add_argument("--case", action="append", default=[])
    parser.add_argument("--resume", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.workers <= 0:
        raise ValueError("workers must be positive")
    thresholds = oracle.parse_thresholds(args.thresholds)
    overrides = json.loads(args.controller_config.read_text())
    if not isinstance(overrides, dict):
        raise ValueError("controller config must be a JSON object")
    cases = load_case_names(args.manifest, set(args.case))
    args.output_root.mkdir(parents=True, exist_ok=True)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    logging.info(
        "running %d PC-quality thresholds across %d case(s) with workers=%d",
        len(thresholds), len(cases), args.workers,
    )
    results: list[CaseResult] = []
    # BOP replay is Python CPU work.  Processes give each case its own GIL and
    # SQLite connection, unlike a thread pool which cannot use all workers.
    with concurrent.futures.ProcessPoolExecutor(max_workers=args.workers) as executor:
        futures = {
            executor.submit(
                run_case, case, args.trace_root, args.output_root, overrides,
                thresholds, args.evaluation_phase, args.resume,
            ): case
            for case in cases
        }
        for future in concurrent.futures.as_completed(futures):
            result = future.result()
            results.append(result)
            logging.info("%s: %s", result.case, result.status)

    summary = build_summary(cases, args.output_root, thresholds, results)
    (args.output_root / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n"
    )
    write_csv(summary, args.output_root / "summary.csv")
    failed = [item for item in results if item.status not in ("passed", "skipped")]
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
