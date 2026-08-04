#!/usr/bin/env python3
"""Measure stable-phase PC-quality oracle headroom for raw BOP candidates.

The oracle first labels each issuer PC from its complete selected-window raw
BOP quality.  It then replays the recorded raw candidates and suppresses PCs
below each requested label threshold.  This is deliberately non-causal: it is
an upper-bound analysis against a fixed replayed P/C controller, not an online
prefetch policy or an input to controller training.
"""

from __future__ import annotations

import argparse
import json
import sqlite3
from dataclasses import asdict
from pathlib import Path
from typing import Iterable, Mapping, Sequence

import analyze_bop_pc_quality as pc_quality
import bop_replay as replay


HORIZON = 2048
COUNT_FIELDS = (
    "candidates", "useful", "unused", "redundant", "censored",
    "eligible_demands", "covered_demands",
)
PC_KEY = tuple[bool, int]


def parse_thresholds(value: str) -> tuple[float, ...]:
    """Parse percentage values such as ``5,10,15,20`` into fractions."""
    values: list[float] = []
    for item in value.split(","):
        item = item.strip()
        if not item:
            continue
        parsed = float(item)
        if not 0.0 <= parsed <= 100.0:
            raise ValueError("PC accuracy thresholds must be in [0, 100]")
        fraction = parsed / 100.0 if parsed > 1.0 else parsed
        values.append(fraction)
    if not values:
        raise ValueError("at least one PC accuracy threshold is required")
    if len(values) != len(set(values)):
        raise ValueError("PC accuracy thresholds must be unique")
    return tuple(sorted(values))


def threshold_name(threshold: float) -> str:
    return f"raw_accuracy_lt_{threshold * 100:g}pct"


def _pc_key(row: Mapping[str, object]) -> PC_KEY:
    has_pc = bool(row["issuer_has_pc"])
    pc_text = row["issuer_trigger_pc"]
    return has_pc, int(str(pc_text), 16) if has_pc and pc_text is not None else 0


def _normalize_quality(metrics: Mapping[str, object]) -> dict[str, object]:
    """Give tracker and replay metrics one common coverage field."""
    result = dict(metrics)
    if "coverage" not in result:
        result["coverage"] = result["coverage_contribution"]
    if "coverage_contribution" not in result:
        result["coverage_contribution"] = result["coverage"]
    return result


def _quality_from_metrics(metrics: replay.QualityMetrics) -> dict[str, object]:
    result = asdict(metrics)
    result["coverage_contribution"] = result["coverage"]
    return result


def _delta(
    actual: Mapping[str, object], reference: Mapping[str, object],
) -> dict[str, object]:
    result = {
        field: int(actual[field]) - int(reference[field])
        for field in ("candidates", "useful", "unused", "redundant", "censored")
    }
    for name in ("accuracy", "coverage"):
        actual_value = actual[name]
        reference_value = reference[name]
        result[f"{name}_points"] = (
            (float(actual_value) - float(reference_value)) * 100.0
            if actual_value is not None and reference_value is not None else None
        )
    return result


def _quality_matches(
    actual: Mapping[str, object], expected: Mapping[str, object],
) -> dict[str, object]:
    mismatches = [
        {
            "field": field,
            "actual": actual[field],
            "expected": expected[field],
        }
        for field in COUNT_FIELDS
        if actual[field] != expected[field]
    ]
    for field in ("accuracy", "coverage"):
        actual_value = actual[field]
        expected_value = expected[field]
        if actual_value is None or expected_value is None:
            if actual_value != expected_value:
                mismatches.append({
                    "field": field,
                    "actual": actual_value,
                    "expected": expected_value,
                })
        elif abs(float(actual_value) - float(expected_value)) > 1e-15:
            mismatches.append({
                "field": field,
                "actual": actual_value,
                "expected": expected_value,
            })
    return {"pass": not mismatches, "mismatches": mismatches}


def _sum_pc_raw(rows: Iterable[Mapping[str, object]]) -> dict[str, int]:
    return {
        field: sum(int(row["raw"][field]) for row in rows)
        for field in ("candidates", "useful", "unused", "redundant", "censored")
    }


def pc_threshold_selection(
    rows: Sequence[Mapping[str, object]], threshold: float,
) -> tuple[dict[PC_KEY, bool], dict[str, object]]:
    """Return stable raw-PC admission labels and their traffic accounting."""
    labels: dict[PC_KEY, bool] = {}
    eligible_rows: list[Mapping[str, object]] = []
    suppressed_rows: list[Mapping[str, object]] = []
    unresolved_rows: list[Mapping[str, object]] = []
    for row in rows:
        key = _pc_key(row)
        if not key[0]:
            continue
        accuracy = row["raw"]["accuracy"]
        if accuracy is None:
            labels[key] = True
            unresolved_rows.append(row)
            continue
        eligible_rows.append(row)
        keep = float(accuracy) >= threshold
        labels[key] = keep
        if not keep:
            suppressed_rows.append(row)

    top_suppressed = sorted(
        suppressed_rows,
        key=lambda row: (-int(row["raw"]["candidates"]), row["issuer_trigger_pc"] or ""),
    )[:20]
    return labels, {
        "criterion": "suppress issuer PCs whose stable raw combined accuracy is below threshold",
        "no_pc_policy": "keep",
        "unresolved_only_pc_policy": "keep",
        "eligible_pc_count": len(eligible_rows),
        "suppressed_pc_count": len(suppressed_rows),
        "kept_pc_count": len(eligible_rows) - len(suppressed_rows),
        "unresolved_only_pc_count": len(unresolved_rows),
        "suppressed_raw_pc_sum": _sum_pc_raw(suppressed_rows),
        "top_suppressed_pcs": [
            {
                "issuer_trigger_pc": row["issuer_trigger_pc"],
                "raw_accuracy": row["raw"]["accuracy"],
                "raw_candidates": row["raw"]["candidates"],
                "raw_useful": row["raw"]["useful"],
                "raw_unused": row["raw"]["unused"],
                "current_accuracy": row["current"]["accuracy"],
                "current_candidates": row["current"]["candidates"],
            }
            for row in top_suppressed
        ],
    }


def _pc_admission_masks(
    rows: Sequence[Mapping[str, object]], thresholds: Sequence[float],
) -> tuple[dict[PC_KEY, int], int]:
    """Precompute all threshold decisions as one bitmask per issuer PC.

    Bit *i* represents admission at ``thresholds[i]``.  Missing PCs, no-PC
    candidates, and PCs with unresolved raw quality retain the all-admitted
    default in the replay hot path, matching ``pc_threshold_selection``.
    """
    full_mask = (1 << len(thresholds)) - 1
    masks: dict[PC_KEY, int] = {}
    for row in rows:
        key = _pc_key(row)
        accuracy = row["raw"]["accuracy"]
        if not key[0] or accuracy is None:
            continue
        mask = 0
        for index, threshold in enumerate(thresholds):
            if float(accuracy) >= threshold:
                mask |= 1 << index
        if mask != full_mask:
            masks[key] = mask
    return masks, full_mask


def replay_oracle_thresholds(
    connection: sqlite3.Connection,
    rows: Sequence[Mapping[str, object]],
    thresholds: Sequence[float],
    window: replay.EvaluationWindow,
) -> tuple[dict[float, dict[str, dict[str, object]]], dict[float, dict[str, object]]]:
    """Second bounded pass over native raw candidates for all threshold labels."""
    # _stream_trace_rows addresses fields by name; do not rely on a preceding
    # metadata pass having configured the connection's row factory.
    connection.row_factory = sqlite3.Row
    admission_masks, full_admission_mask = _pc_admission_masks(rows, thresholds)
    raw_accumulator = replay._StreamingMetricSet((HORIZON,))
    threshold_accumulators = tuple(
        replay._StreamingMetricSet((HORIZON,)) for _ in thresholds
    )
    accumulators = {
        0.0: raw_accumulator,
        **{
            threshold: accumulator
            for threshold, accumulator in zip(thresholds, threshold_accumulators)
        },
    }
    all_accumulators = (raw_accumulator, *threshold_accumulators)

    def on_demand(demand: replay.Demand) -> None:
        selected = replay._in_evaluation_window(demand, window)
        for accumulator in all_accumulators:
            accumulator.observe_demand(demand, selected)

    def on_event(event: replay.ReplayEvent) -> None:
        if (not event.raw_candidate_valid
                or not replay._in_evaluation_window(event, window)):
            return
        key = (event.trigger_has_pc, event.trigger_pc if event.trigger_has_pc else 0)
        raw_accumulator.emit(
            event.bop_kind, event.access_seq, event.tick,
            event.raw_candidate_addr, event.phase_id, True,
        )
        admission_mask = (
            admission_masks.get(key, full_admission_mask)
            if key[0] else full_admission_mask
        )
        if admission_mask == full_admission_mask:
            for accumulator in threshold_accumulators:
                accumulator.emit(
                    event.bop_kind, event.access_seq, event.tick,
                    event.raw_candidate_addr, event.phase_id, True,
                )
            return
        for index, accumulator in enumerate(threshold_accumulators):
            if admission_mask & (1 << index):
                accumulator.emit(
                    event.bop_kind, event.access_seq, event.tick,
                    event.raw_candidate_addr, event.phase_id, True,
                )

    replay._stream_trace_rows(connection, on_demand, on_event, lambda: None)
    quality = {
        threshold: {
            kind: _quality_from_metrics(metrics)
            for kind, metrics in accumulator.finish()[HORIZON].items()
        }
        for threshold, accumulator in accumulators.items()
    }
    selection = {
        threshold: pc_threshold_selection(rows, threshold)[1]
        for threshold in thresholds
    }
    return quality, selection


def _per_pc_rows(rows: Sequence[Mapping[str, object]], thresholds: Sequence[float]) -> list[dict[str, object]]:
    labels = {
        threshold: pc_threshold_selection(rows, threshold)[0]
        for threshold in thresholds
    }
    result = []
    for row in rows:
        key = _pc_key(row)
        result.append({
            "issuer_trigger_pc": row["issuer_trigger_pc"],
            "issuer_has_pc": row["issuer_has_pc"],
            "raw": _normalize_quality(row["raw"]),
            "current": _normalize_quality(row["current"]),
            "oracle_admission": {
                threshold_name(threshold): (
                    "keep_no_pc" if not key[0]
                    else "keep" if labels[threshold].get(key, True)
                    else "suppress"
                )
                for threshold in thresholds
            },
        })
    return result


def analyze_case(
    database: Path, controller_overrides: Mapping[str, object],
    thresholds: Sequence[float], evaluation_phase: str,
) -> dict[str, object]:
    """Analyze one certified V5 trace without changing its online state."""
    with sqlite3.connect(database) as connection:
        trace_config, phases = replay._streaming_metadata(connection)
        config = replay._apply_controller_overrides(
            controller_overrides, trace_config,
        )
        if not config.pc_validation_producer_consumer:
            raise ValueError("current oracle baseline requires producer/consumer mode")
        window = replay.resolve_evaluation_window(
            phase_name=evaluation_phase,
            start_tick=None,
            stats_path=None,
            stats_block=None,
            phases=phases,
        )
        first_pass = pc_quality.analyze_pc_quality(connection, config, window, 20)
        raw = _normalize_quality(first_pass["aggregate_quality"]["raw"]["combined"])
        current = _normalize_quality(
            first_pass["aggregate_quality"]["current"]["combined"]
        )
        threshold_quality, selection = replay_oracle_thresholds(
            connection, first_pass["all_pc"], thresholds, window,
        )

    raw_reference = threshold_quality[0.0]["combined"]
    raw_replay_certification = _quality_matches(raw_reference, raw)
    if not raw_replay_certification["pass"]:
        raise ValueError(
            "raw candidate second pass diverged from issuer-PC raw quality: "
            f"{raw_replay_certification}"
        )
    points = {}
    for threshold in thresholds:
        quality = threshold_quality[threshold]
        points[threshold_name(threshold)] = {
            "threshold_fraction": threshold,
            "threshold_percent": threshold * 100.0,
            "selection": selection[threshold],
            "quality": quality,
            "delta_vs_raw": _delta(quality["combined"], raw),
            "delta_vs_current": _delta(quality["combined"], current),
        }
    return {
        "model": {
            "horizon": HORIZON,
            "evaluation_phase": evaluation_phase,
            "state_replay": "full trace for current controller; selected stable window for labels and quality",
            "oracle_label": "stable raw combined issuer-PC accuracy, computed before threshold admission",
            "candidate_owner": "BOPReplayEvent.TriggerPC",
            "current_point": "replayed fixed producer/consumer controller",
            "not_modeled": "local filters, cache fills, MSHRs, residency, bandwidth",
            "non_causal": True,
        },
        "database": str(database),
        "controller_parameters": first_pass["controller_parameters"],
        "evaluation_window": first_pass["evaluation_window"],
        "owner_reconstruction": first_pass["owner_reconstruction"],
        "raw_replay_certification": raw_replay_certification,
        "quality": {"raw": raw, "current": current},
        "oracle_thresholds": points,
        "per_pc": _per_pc_rows(first_pass["all_pc"], thresholds),
        "controller_stats": first_pass["controller_stats"],
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("database", type=Path, help="certified V5 BOP trace DB")
    parser.add_argument(
        "--controller-config", type=Path,
        default=Path(__file__).with_name("producer_consumer_k2.json"),
        help="fixed shared P/C controller override",
    )
    parser.add_argument(
        "--thresholds", default="5,10,15,20",
        help="comma-separated raw PC accuracy percentages, e.g. 5,10,15,20",
    )
    parser.add_argument("--evaluation-phase", default="stable")
    parser.add_argument("--output", type=Path, default=None)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    thresholds = parse_thresholds(args.thresholds)
    overrides = json.loads(args.controller_config.read_text())
    if not isinstance(overrides, dict):
        raise ValueError("controller config must be a JSON object")
    report = analyze_case(
        args.database, overrides, thresholds, args.evaluation_phase,
    )
    encoded = json.dumps(report, indent=2, sort_keys=True)
    if args.output is None:
        print(encoded)
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
