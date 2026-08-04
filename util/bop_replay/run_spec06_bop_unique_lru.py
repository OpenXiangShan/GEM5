#!/usr/bin/env python3
"""Evaluate bounded unique-address RR evidence on certified SPEC06 traces.

Each process replays the complete V5 trace to preserve warmup controller and
LRU state, but reports quality only for the stable phase at Horizon 2,048.
The raw baseline and native-RR producer/consumer controller report already
stored beside each trace are certification oracles.  A case is excluded from
aggregation unless its replayed ``current`` point matches that native P/C
report exactly.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import csv
import json
import logging
import sqlite3
from dataclasses import dataclass
from pathlib import Path
from typing import Mapping, Sequence

import bop_replay as replay
import replay_bop_pc_counterfactual as counterfactual


HORIZON = 2048
DEFAULT_MANIFEST = Path(__file__).with_name("spec06_bop_policy_manifest.json")
DEFAULT_TRACE_ROOT = Path(
    "/nfs/home/lijiangtao/temp/bop-replay/spec06-bop-v5-certification-20260813"
)
DEFAULT_OUTPUT_ROOT = Path(
    "/nfs/home/lijiangtao/temp/bop-replay/pc-validation-unique-lru-20260817"
)
DEFAULT_CONTROLLER_CONFIG = Path(__file__).with_name("producer_consumer_k2.json")
COUNT_FIELDS = (
    "candidates",
    "censored",
    "redundant",
    "unused",
    "useful",
    "covered_demands",
    "eligible_demands",
)
QUALITY_FIELDS = (*COUNT_FIELDS, "accuracy", "coverage")
VALIDATION_FIELDS = (
    "checks",
    "recorded_hits",
    "recorded_misses",
    "counterfactual_hits",
    "counterfactual_misses",
    "recovered_hits",
    "recovered_unique_lru_hits",
    "recovered_conflict_hits",
    "recovered_delay_drop_hits",
    "recovered_other_hits",
    "lost_recorded_hits",
    "stale_age_misses",
)
EVIDENCE_FIELDS = (
    "mature_insertions",
    "duplicate_refreshes",
    "capacity_evictions",
    "delay_pending",
    "delay_dequeues",
    "delay_enqueues",
    "delay_drops",
)


@dataclass(frozen=True)
class CaseResult:
    case: str
    status: str
    detail: str


def load_case_names(manifest: Path, selected: set[str]) -> list[str]:
    payload = json.loads(manifest.read_text())
    names = [str(case["name"]) for case in payload["cases"]]
    if selected:
        unknown = sorted(selected - set(names))
        if unknown:
            raise ValueError("unknown case(s): " + ", ".join(unknown))
        names = [name for name in names if name in selected]
    if not names:
        raise ValueError("no cases selected")
    return names


def _load_json(path: Path) -> dict[str, object]:
    try:
        payload = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read {path}: {error}") from error
    if not isinstance(payload, dict):
        raise ValueError(f"{path} must contain a JSON object")
    return payload


def _trace_inputs(trace_root: Path, case: str) -> tuple[Path, Path, Path]:
    case_root = trace_root / case
    database = case_root / "trace.db"
    raw = case_root / "raw-baseline.json"
    current = case_root / "policy" / "producer_consumer_k2.json"
    if not database.is_file() or not raw.is_file() or not current.is_file():
        raise ValueError("trace DB, raw baseline, or P/C baseline is missing")
    return database, raw, current


def _quality_equal(actual: Mapping[str, object], expected: Mapping[str, object]) -> bool:
    return all(actual.get(field) == expected.get(field) for field in QUALITY_FIELDS)


def _certify_current(
    report: Mapping[str, object], expected_current: Mapping[str, object],
) -> dict[str, object]:
    actual = report["points"]["current"]["quality"]["combined"]
    expected = expected_current["horizons"][str(HORIZON)]["combined"]
    return {
        "pass": _quality_equal(actual, expected),
        "actual": actual,
        "expected": expected,
    }


def _report_is_valid(path: Path, unique_lru_entries: int) -> bool:
    if not path.is_file():
        return False
    try:
        report = _load_json(path)
        return bool(
            report.get("owner_reconstruction", {}).get("pass")
            and report.get("current_certification", {}).get("pass")
            and report.get("model", {}).get("unique_lru_entries")
            == unique_lru_entries
            and set(report.get("points", {})) == {"current", "unique_lru"}
        )
    except (KeyError, TypeError, ValueError):
        return False


def _reuse_standalone_report(
    output_path: Path, raw_baseline: Mapping[str, object],
    expected_current: Mapping[str, object], unique_lru_entries: int,
) -> bool:
    """Attach certification to a complete standalone replay without rerunning it."""
    try:
        report = _load_json(output_path)
        if (
            not report.get("owner_reconstruction", {}).get("pass")
            or report.get("model", {}).get("unique_lru_entries")
            != unique_lru_entries
            or set(report.get("points", {})) != {"current", "unique_lru"}
        ):
            return False
        report["raw_baseline"] = raw_baseline["horizons"][str(HORIZON)]
        report["current_certification"] = _certify_current(
            report, expected_current,
        )
        if not report["current_certification"]["pass"]:
            return False
        output_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    except (KeyError, TypeError, ValueError):
        return False
    return True


def run_case(
    case: str, trace_root: Path, output_root: Path,
    controller_overrides: Mapping[str, object], unique_lru_entries: int,
    evaluation_phase: str, resume: bool,
) -> CaseResult:
    output_path = output_root / case / "unique-lru.json"
    try:
        database, raw_path, current_path = _trace_inputs(trace_root, case)
        raw_baseline = _load_json(raw_path)
        if not raw_baseline.get("verification", {}).get("pass"):
            raise ValueError("raw learner baseline is not certified")
        expected_current = _load_json(current_path)
        if resume and _report_is_valid(output_path, unique_lru_entries):
            return CaseResult(case, "skipped", "existing certified report")
        if resume and _reuse_standalone_report(
                output_path, raw_baseline, expected_current, unique_lru_entries):
            return CaseResult(case, "skipped", "standalone replay certified")
        with sqlite3.connect(database) as connection:
            config, phases = replay._streaming_metadata(connection)
            config = replay._apply_controller_overrides(
                controller_overrides, config,
            )
            if not config.pc_validation_producer_consumer:
                raise ValueError("unique-LRU batch requires producer/consumer mode")
            window = replay.resolve_evaluation_window(
                phase_name=evaluation_phase,
                start_tick=None,
                stats_path=None,
                stats_block=None,
                phases=phases,
            )
            report = counterfactual.replay_counterfactuals(
                connection, config, window,
                points=("current", "unique_lru"),
                unique_lru_entries=unique_lru_entries,
            )
        report["raw_baseline"] = raw_baseline["horizons"][str(HORIZON)]
        report["current_certification"] = _certify_current(
            report, expected_current,
        )
        if not report["current_certification"]["pass"]:
            raise ValueError("replayed native P/C current point diverged")
        report["database"] = str(database)
        report["schema_version"] = config.schema_version
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    except Exception as error:
        return CaseResult(case, "failed", str(error))
    return CaseResult(case, "passed", "current certified; unique-LRU replay complete")


def _aggregate_quality(metrics: Sequence[Mapping[str, object]]) -> dict[str, object]:
    total = {field: sum(int(item[field]) for item in metrics) for field in COUNT_FIELDS}
    denominator = total["useful"] + total["unused"] + total["redundant"]
    total["accuracy"] = total["useful"] / denominator if denominator else None
    total["coverage"] = (
        total["covered_demands"] / total["eligible_demands"]
        if total["eligible_demands"] else None
    )
    return total


def _aggregate_fields(
    values: Sequence[Mapping[str, object]], fields: Sequence[str],
) -> dict[str, int]:
    return {field: sum(int(value.get(field, 0)) for value in values) for field in fields}


def _delta(actual: Mapping[str, object], base: Mapping[str, object]) -> dict[str, float | int]:
    return {
        "useful": int(actual["useful"]) - int(base["useful"]),
        "unused": int(actual["unused"]) - int(base["unused"]),
        "candidates": int(actual["candidates"]) - int(base["candidates"]),
        "accuracy": float(actual["accuracy"]) - float(base["accuracy"]),
        "coverage": float(actual["coverage"]) - float(base["coverage"]),
    }


def build_summary(
    cases: Sequence[str], output_root: Path, results: Sequence[CaseResult],
    unique_lru_entries: int,
) -> dict[str, object]:
    status = {result.case: result for result in results}
    records: dict[str, object] = {}
    raw_metrics: list[Mapping[str, object]] = []
    current_metrics: list[Mapping[str, object]] = []
    lru_metrics: list[Mapping[str, object]] = []
    lru_validation: list[Mapping[str, object]] = []
    lru_evidence: list[Mapping[str, object]] = []
    for case in cases:
        path = output_root / case / "unique-lru.json"
        result = status.get(case)
        if not _report_is_valid(path, unique_lru_entries):
            records[case] = {
                "status": result.status if result else "not_run",
                "detail": result.detail if result else "report_missing",
            }
            continue
        report = _load_json(path)
        raw = report["raw_baseline"]["combined"]
        current = report["points"]["current"]["quality"]["combined"]
        lru = report["points"]["unique_lru"]["quality"]["combined"]
        validation = report["points"]["unique_lru"]["validation"]
        evidence = report["evidence_state"]["unique_lru"]
        records[case] = {
            "status": "ready",
            "raw": raw,
            "current": current,
            "unique_lru": lru,
            "delta_vs_current": _delta(lru, current),
            "validation": validation,
            "evidence_by_bop": evidence,
        }
        raw_metrics.append(raw)
        current_metrics.append(current)
        lru_metrics.append(lru)
        lru_validation.append(validation)
        lru_evidence.extend(evidence.values())

    ready = len(raw_metrics)
    aggregate = None
    if ready == len(cases):
        raw = _aggregate_quality(raw_metrics)
        current = _aggregate_quality(current_metrics)
        lru = _aggregate_quality(lru_metrics)
        aggregate = {
            "raw": raw,
            "current": current,
            "unique_lru": lru,
            "unique_lru_delta_vs_current": _delta(lru, current),
            "unique_lru_validation_full_trace": _aggregate_fields(
                lru_validation, VALIDATION_FIELDS,
            ),
            "unique_lru_evidence_full_trace_both_bops": _aggregate_fields(
                lru_evidence, EVIDENCE_FIELDS,
            ),
        }
        checks = aggregate["unique_lru_validation_full_trace"]["checks"]
        recovered = aggregate["unique_lru_validation_full_trace"][
            "recovered_unique_lru_hits"
        ]
        aggregate["unique_lru_validation_full_trace"]["recovered_hit_rate"] = (
            recovered / checks if checks else 0.0
        )
    return {
        "model": {
            "horizon": HORIZON,
            "quality_window": "stable phase only; controller/LRU state is full trace",
            "controller": "fixed P/C-K2/global-bypass",
            "unique_lru_entries_per_bop": unique_lru_entries,
            "unique_lru_semantics": (
                "exact mature demand lines; native RR hit wins; LRU queried only "
                "on native RR miss"
            ),
            "not_modeled": "local filters, cache fills, MSHRs, residency, bandwidth",
        },
        "case_count": len(cases),
        "ready_case_count": ready,
        "aggregate": aggregate,
        "cases": records,
        "execution": [
            {"case": result.case, "status": result.status, "detail": result.detail}
            for result in sorted(results, key=lambda value: value.case)
        ],
    }


def write_csv(summary: Mapping[str, object], path: Path) -> None:
    rows = []
    for case, record in summary["cases"].items():
        if record.get("status") != "ready":
            continue
        rows.extend((case, point, record[point]) for point in ("raw", "current", "unique_lru"))
    aggregate = summary.get("aggregate")
    if aggregate is not None:
        rows.extend(("aggregate", point, aggregate[point]) for point in ("raw", "current", "unique_lru"))
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(
            stream, fieldnames=("scope", "point", *COUNT_FIELDS, "accuracy", "coverage"),
        )
        writer.writeheader()
        for scope, point, metrics in rows:
            writer.writerow({
                "scope": scope,
                "point": point,
                **{field: metrics[field] for field in COUNT_FIELDS},
                "accuracy": metrics["accuracy"],
                "coverage": metrics["coverage"],
            })


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--trace-root", type=Path, default=DEFAULT_TRACE_ROOT)
    parser.add_argument("--output-root", type=Path, default=DEFAULT_OUTPUT_ROOT)
    parser.add_argument("--controller-config", type=Path, default=DEFAULT_CONTROLLER_CONFIG)
    parser.add_argument("--unique-lru-entries", type=int, default=2048)
    parser.add_argument("--evaluation-phase", default="stable")
    parser.add_argument("--workers", type=int, default=31)
    parser.add_argument("--case", action="append", default=[])
    parser.add_argument("--resume", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.workers <= 0:
        raise ValueError("workers must be positive")
    if args.unique_lru_entries <= 0:
        raise ValueError("unique-LRU entries must be positive")
    controller_overrides = _load_json(args.controller_config)
    cases = load_case_names(args.manifest, set(args.case))
    args.output_root.mkdir(parents=True, exist_ok=True)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    logging.info(
        "running unique-LRU entries=%d over %d case(s) with workers=%d",
        args.unique_lru_entries, len(cases), args.workers,
    )
    results: list[CaseResult] = []
    with concurrent.futures.ProcessPoolExecutor(max_workers=args.workers) as executor:
        futures = {
            executor.submit(
                run_case, case, args.trace_root, args.output_root,
                controller_overrides, args.unique_lru_entries,
                args.evaluation_phase, args.resume,
            ): case
            for case in cases
        }
        for future in concurrent.futures.as_completed(futures):
            result = future.result()
            results.append(result)
            logging.info("%s: %s", result.case, result.status)
    summary = build_summary(cases, args.output_root, results, args.unique_lru_entries)
    (args.output_root / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n"
    )
    write_csv(summary, args.output_root / "summary.csv")
    failed = [result for result in results if result.status not in ("passed", "skipped")]
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
