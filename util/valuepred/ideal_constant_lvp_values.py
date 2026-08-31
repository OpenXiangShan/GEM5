#!/usr/bin/env python3
"""Aggregate saturated-value epochs emitted by IdealConstantLVP.

The value profile is intentionally kept separate from the per-PC prediction
profile.  A row describes one raw ``RegVal`` value segment of one saturated
``(tid, PC, saturation_epoch)``.  The online hardware-capacity result always
comes from the predictor's ``profile*PeakDistinctSaturatedValues`` statistic:
it tracks the live set during execution.  A separate sweep of dumped segment
intervals is retained as temporal auxiliary evidence and is never substituted
for that online statistic.  The tool uses only the Python standard library so
it can run on CI workers without the plotting/data-processing environment.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
import statistics
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Set, Tuple


VALUE_FILE = "ideal_constant_lvp_saturated_values.csv"
VALUE_VERSION = "ideal_constant_lvp_saturated_values_v1"
SCOPES = ("lifetime", "roi")

BASE_COLUMNS = (
    "scope",
    "tid",
    "pc",
    "saturation_epoch",
    "value_segment",
    "saturated_value",
)
INTERVAL_COLUMNS = (
    "saturation_start_seq_no",
    "saturation_end_seq_no",
    "open_at_scope_start",
    "open_at_end",
)
USAGE_COLUMNS = (
    "prediction_uses",
    "correct_prediction_uses",
)

STAT_PEAK_SUFFIXES = {
    "roi": "profileRoiPeakDistinctSaturatedValues",
    "lifetime": "profileLifetimePeakDistinctSaturatedValues",
}
STAT_PC_PEAK_SUFFIXES = {
    "roi": "profileRoiPeakSaturatedPcs",
    "lifetime": "profileLifetimePeakSaturatedPcs",
}
STAT_USAGE_FIELDS = {
    "vp_supported": "system.cpu.valuePred.VPsupported",
    "vp_predicted": "system.cpu.valuePred.VPpredicted",
    "vp_corrected": "system.cpu.valuePred.VPcorrected",
}

SLICE_RE = re.compile(r"^(?P<benchmark>.+)_[0-9]+$")


class AnalysisError(ValueError):
    """Input data violates the saturated-value profiling contract."""


@dataclass(frozen=True)
class Segment:
    """One saturated raw-value segment from the emitter CSV."""

    slice_name: str
    source: str
    scope: str
    tid: int
    pc: int
    epoch: int
    value_segment: int
    value: int
    start: Optional[int]
    end: Optional[int]
    open_at_scope_start: Optional[bool]
    open_at_end: Optional[bool]
    prediction_uses: Optional[int]
    correct_prediction_uses: Optional[int]


@dataclass
class SliceInput:
    slice_name: str
    value_path: Path
    stats_path: Optional[Path]


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "inputs",
        nargs="+",
        type=Path,
        help="Archive roots, spec_all roots, slice roots, or value CSV files.",
    )
    parser.add_argument("--scope", choices=SCOPES, default="roi")
    parser.add_argument("--out-dir", type=Path, required=True)
    return parser.parse_args(argv)


def parse_int(value: str, path: Path, line_number: int, field: str) -> int:
    try:
        return int(value, 0)
    except (TypeError, ValueError) as error:
        raise AnalysisError(
            f"{path}:{line_number} has invalid {field}={value!r}"
        ) from error


def parse_bool(value: str, path: Path, line_number: int, field: str) -> bool:
    parsed = parse_int(value, path, line_number, field)
    if parsed not in (0, 1):
        raise AnalysisError(
            f"{path}:{line_number} has invalid boolean {field}={value!r}"
        )
    return bool(parsed)


def benchmark_for(slice_name: str) -> str:
    match = SLICE_RE.match(slice_name)
    return match.group("benchmark") if match else slice_name


def _read_csv_lines(path: Path) -> Tuple[Dict[str, str], str, List[str]]:
    metadata: Dict[str, str] = {}
    version: Optional[str] = None
    data_lines: List[str] = []
    try:
        source = path.open(newline="", errors="replace")
    except OSError:
        raise
    with source:
        for line in source:
            if line.startswith("#"):
                comment = line[1:].strip()
                if comment.startswith("ideal_constant_lvp_saturated_values_"):
                    if version is not None:
                        raise AnalysisError(
                            f"{path} declares multiple value-profile versions"
                        )
                    version = comment
                key, separator, value = comment.partition("=")
                if separator:
                    metadata[key] = value
            else:
                data_lines.append(line)
    if version != VALUE_VERSION:
        raise AnalysisError(
            f"{path} has unsupported value-profile version {version!r}; "
            f"expected {VALUE_VERSION!r}"
        )
    if not data_lines:
        raise AnalysisError(f"{path} has no value-profile CSV header")
    return metadata, version, data_lines


def _optional_column(fieldnames: Sequence[str], names: Sequence[str]) -> Optional[str]:
    for name in names:
        if name in fieldnames:
            return name
    return None


def parse_value_csv(
    path: Path, slice_name: str
) -> Tuple[List[Segment], Dict[str, str], bool, bool]:
    """Parse and validate one emitter CSV.

    Duplicate keys are checked across both scopes even when the caller only
    requests one scope. This catches a corrupt file before a scope filter can
    hide it. Interval and committed-use columns are all-or-nothing; partial
    schemas are rejected rather than silently producing false capacity or
    coverage results.
    """

    metadata, _version, data_lines = _read_csv_lines(path)
    reader = csv.DictReader(data_lines)
    fieldnames = reader.fieldnames or []
    missing = [field for field in BASE_COLUMNS if field not in fieldnames]
    if missing:
        raise AnalysisError(f"{path} is missing value-profile columns: {missing}")

    start_column = _optional_column(
        fieldnames,
        ("saturation_start_seq_no", "interval_start_seq_no", "start_seq_no"),
    )
    end_column = _optional_column(
        fieldnames,
        ("saturation_end_seq_no", "interval_end_seq_no", "end_seq_no"),
    )
    scope_start_column = _optional_column(
        fieldnames, ("open_at_scope_start", "open_at_start")
    )
    scope_end_column = _optional_column(fieldnames, ("open_at_end", "open_at_scope_end"))
    interval_columns_present = any(
        column is not None
        for column in (start_column, end_column, scope_start_column, scope_end_column)
    )
    if interval_columns_present and not all(
        column is not None
        for column in (start_column, end_column, scope_start_column, scope_end_column)
    ):
        raise AnalysisError(f"{path} has a partial interval-column schema")
    usage_columns_present = any(field in fieldnames for field in USAGE_COLUMNS)
    if usage_columns_present and not all(field in fieldnames for field in USAGE_COLUMNS):
        raise AnalysisError(f"{path} has a partial prediction-use column schema")

    rows: List[Segment] = []
    seen: Set[Tuple[str, int, int, int, int]] = set()
    for line_number, raw in enumerate(reader, start=2):
        if None in raw or any(value is None for value in raw.values()):
            raise AnalysisError(f"{path}:{line_number} has the wrong number of columns")
        scope = raw["scope"]
        if scope not in SCOPES:
            raise AnalysisError(f"{path}:{line_number} has scope {scope!r}")
        tid = parse_int(raw["tid"], path, line_number, "tid")
        pc = parse_int(raw["pc"], path, line_number, "pc")
        epoch = parse_int(raw["saturation_epoch"], path, line_number, "saturation_epoch")
        value_segment = parse_int(
            raw["value_segment"], path, line_number, "value_segment"
        )
        if tid < 0 or pc < 0 or epoch <= 0 or value_segment <= 0:
            raise AnalysisError(f"{path}:{line_number} has a non-positive key field")
        key = (scope, tid, pc, epoch, value_segment)
        if key in seen:
            raise AnalysisError(f"{path}:{line_number} duplicates {key!r}")
        seen.add(key)
        value = parse_int(raw["saturated_value"], path, line_number, "saturated_value")
        if value < 0:
            raise AnalysisError(
                f"{path}:{line_number} has a negative saturated_value"
            )

        start: Optional[int] = None
        end: Optional[int] = None
        open_at_scope_start: Optional[bool] = None
        open_at_end: Optional[bool] = None
        if interval_columns_present:
            assert start_column is not None
            assert end_column is not None
            assert scope_start_column is not None
            assert scope_end_column is not None
            start = parse_int(raw[start_column], path, line_number, start_column)
            end = parse_int(raw[end_column], path, line_number, end_column)
            open_at_scope_start = parse_bool(
                raw[scope_start_column], path, line_number, scope_start_column
            )
            open_at_end = parse_bool(raw[scope_end_column], path, line_number, scope_end_column)
            if start < 0 or end < 0:
                raise AnalysisError(f"{path}:{line_number} has a negative interval bound")
            if open_at_scope_start != (start == 0):
                raise AnalysisError(
                    f"{path}:{line_number} has inconsistent scope-start interval"
                )
            if open_at_end != (end == 0):
                raise AnalysisError(
                    f"{path}:{line_number} has inconsistent scope-end interval"
                )
            if start != 0 and end != 0 and end < start:
                raise AnalysisError(f"{path}:{line_number} has end before start")

        prediction_uses: Optional[int] = None
        correct_prediction_uses: Optional[int] = None
        if usage_columns_present:
            prediction_uses = parse_int(
                raw["prediction_uses"], path, line_number, "prediction_uses"
            )
            correct_prediction_uses = parse_int(
                raw["correct_prediction_uses"],
                path,
                line_number,
                "correct_prediction_uses",
            )
            if not 0 <= correct_prediction_uses <= prediction_uses:
                raise AnalysisError(
                    f"{path}:{line_number} violates correct_prediction_uses "
                    "<= prediction_uses"
                )

        rows.append(
            Segment(
                slice_name=slice_name,
                source=str(path),
                scope=scope,
                tid=tid,
                pc=pc,
                epoch=epoch,
                value_segment=value_segment,
                value=value,
                start=start,
                end=end,
                open_at_scope_start=open_at_scope_start,
                open_at_end=open_at_end,
                prediction_uses=prediction_uses,
                correct_prediction_uses=correct_prediction_uses,
            )
        )
    return rows, metadata, interval_columns_present, usage_columns_present


def parse_stats(path: Optional[Path]) -> Dict[str, Optional[int]]:
    """Read online capacity and VP-use counters from the final stats block."""

    result: Dict[str, Optional[int]] = {
        f"{scope}_peak_distinct_saturated_values": None for scope in SCOPES
    }
    result.update({f"{scope}_peak_saturated_pcs": None for scope in SCOPES})
    result.update({key: None for key in STAT_USAGE_FIELDS})
    if path is None or not path.is_file():
        return result
    blocks: List[Dict[str, int]] = []
    current: Optional[Dict[str, int]] = None
    with path.open(errors="replace") as source:
        for line in source:
            if "Begin Simulation Statistics" in line:
                current = {}
                continue
            if "End Simulation Statistics" in line:
                if current is not None:
                    blocks.append(current)
                current = None
                continue
            if current is None:
                continue
            fields = line.split()
            if len(fields) < 2:
                continue
            field_name = fields[0]
            distinct_value_scope = next(
                (
                    candidate
                    for candidate, suffix in STAT_PEAK_SUFFIXES.items()
                    if field_name == suffix or field_name.endswith("." + suffix)
                ),
                None,
            )
            pc_scope = next(
                (
                    candidate
                    for candidate, suffix in STAT_PC_PEAK_SUFFIXES.items()
                    if field_name == suffix or field_name.endswith("." + suffix)
                ),
                None,
            )
            usage_key = next(
                (
                    candidate
                    for candidate, stat_name in STAT_USAGE_FIELDS.items()
                    if field_name == stat_name
                ),
                None,
            )
            if (
                distinct_value_scope is None
                and pc_scope is None
                and usage_key is None
            ):
                continue
            try:
                value = float(fields[1])
            except ValueError as error:
                raise AnalysisError(
                    f"{path} has invalid {field_name} value {fields[1]!r}"
                ) from error
            if not math.isfinite(value) or not value.is_integer() or value < 0:
                raise AnalysisError(f"{path} has invalid {field_name} value {fields[1]!r}")
            if distinct_value_scope is not None:
                current[
                    f"{distinct_value_scope}_peak_distinct_saturated_values"
                ] = int(value)
            elif pc_scope is not None:
                current[f"{pc_scope}_peak_saturated_pcs"] = int(value)
            else:
                assert usage_key is not None
                current[usage_key] = int(value)
    if blocks:
        result.update(blocks[-1])
    return result


def discover_inputs(inputs: Sequence[Path]) -> Dict[str, SliceInput]:
    """Discover one value CSV per slice and reject duplicate slice IDs."""

    discovered: Dict[str, SliceInput] = {}
    for input_path in inputs:
        path = input_path.resolve()
        if path.is_file():
            candidates = [path]
        elif path.is_dir():
            candidates = sorted(path.rglob(VALUE_FILE))
        else:
            raise AnalysisError(f"input does not exist: {path}")
        for candidate in candidates:
            if candidate.name != VALUE_FILE:
                raise AnalysisError(f"expected {VALUE_FILE}, got {candidate}")
            if candidate.parent.name == "m5out":
                slice_root = candidate.parent.parent
                stats_path: Optional[Path] = candidate.parent / "stats.txt"
            else:
                slice_root = candidate.parent
                stats_path = candidate.with_name("stats.txt")
            slice_name = slice_root.name
            if slice_name in discovered:
                raise AnalysisError(
                    f"duplicate slice {slice_name!r}: "
                    f"{discovered[slice_name].value_path} and {candidate}"
                )
            discovered[slice_name] = SliceInput(slice_name, candidate, stats_path)
    if not discovered:
        raise AnalysisError(f"no {VALUE_FILE} files found")
    return discovered


def percentile(values: Iterable[float], percentage: float) -> Optional[float]:
    data = sorted(value for value in values if math.isfinite(value))
    if not data:
        return None
    index = max(0, min(len(data) - 1, math.ceil(len(data) * percentage / 100.0) - 1))
    return data[index]


def mean_or_none(values: Iterable[float]) -> Optional[float]:
    data = [value for value in values if math.isfinite(value)]
    return statistics.mean(data) if data else None


def _fmt_value(value: int) -> str:
    return f"0x{value:x}"


def _sweep_peak(segments: Sequence[Segment]) -> Tuple[Optional[int], Optional[int]]:
    """Return segment-interval peaks using inclusive sequence-number bounds.

    This derives an offline interval view from the dump and intentionally does
    not replace the online live-set peak maintained by IdealConstantLVP.  In
    particular, reset-boundary reconstruction and commit-boundary ordering can
    make this sweep differ from the online hardware counter.
    """

    if not segments or any(segment.start is None for segment in segments):
        return None, None
    starts: Dict[int, List[int]] = defaultdict(list)
    ends: Dict[int, List[int]] = defaultdict(list)
    active: Set[int] = set()
    for index, segment in enumerate(segments):
        assert segment.start is not None
        assert segment.end is not None
        if segment.open_at_scope_start:
            active.add(index)
        else:
            starts[segment.start].append(index)
        if not segment.open_at_end:
            ends[segment.end].append(index)

    peak_values = len({segments[index].value for index in active})
    peak_pcs = len({(segments[index].tid, segments[index].pc) for index in active})
    for position in sorted(set(starts) | set(ends)):
        # Intervals are inclusive.  A new segment beginning at the same
        # sequence number as an old segment's end is counted at the boundary.
        active.update(starts[position])
        values = {segments[index].value for index in active}
        pcs = {(segments[index].tid, segments[index].pc) for index in active}
        peak_values = max(peak_values, len(values))
        peak_pcs = max(peak_pcs, len(pcs))
        for index in ends[position]:
            active.discard(index)
    return peak_values, peak_pcs


def _segment_end_for_output(segment: Segment) -> Optional[int]:
    if segment.end is None or segment.open_at_end:
        return None
    return segment.end


def analyze_slice(
    slice_input: SliceInput,
    scope: str,
) -> Tuple[Dict[str, object], List[Dict[str, object]], List[Dict[str, object]], Dict[str, object]]:
    (
        rows,
        metadata,
        interval_columns_present,
        usage_columns_present,
    ) = parse_value_csv(
        slice_input.value_path, slice_input.slice_name
    )
    selected = [row for row in rows if row.scope == scope]
    stats = parse_stats(slice_input.stats_path)
    stats_peak = stats[f"{scope}_peak_distinct_saturated_values"]
    stats_pc_peak = stats[f"{scope}_peak_saturated_pcs"]
    if (
        stats_peak is not None
        and stats_pc_peak is not None
        and stats_peak > stats_pc_peak
    ):
        raise AnalysisError(
            f"{slice_input.slice_name}: online distinct-value peak {stats_peak} "
            f"exceeds online saturated-PC peak {stats_pc_peak}"
        )

    pcs: Dict[Tuple[int, int], Set[int]] = defaultdict(set)
    pc_segments: Dict[Tuple[int, int], List[Segment]] = defaultdict(list)
    values: Dict[int, Set[Tuple[int, int]]] = defaultdict(set)
    value_segments: Dict[int, List[Segment]] = defaultdict(list)
    for segment in selected:
        key = (segment.tid, segment.pc)
        pcs[key].add(segment.value)
        pc_segments[key].append(segment)
        values[segment.value].add(key)
        value_segments[segment.value].append(segment)

    cumulative_distinct_values = sum(len(value_set) for value_set in pcs.values())
    global_distinct_values = len(values)
    fanouts = [len(owners) for owners in values.values()]
    prediction_uses = (
        sum(segment.prediction_uses or 0 for segment in selected)
        if usage_columns_present
        else None
    )
    correct_prediction_uses = (
        sum(segment.correct_prediction_uses or 0 for segment in selected)
        if usage_columns_present
        else None
    )
    if usage_columns_present:
        assert prediction_uses is not None
        assert correct_prediction_uses is not None
        if stats["vp_predicted"] is not None and prediction_uses != stats["vp_predicted"]:
            raise AnalysisError(
                f"{slice_input.slice_name}: saturated-value prediction uses "
                f"{prediction_uses} != VPpredicted {stats['vp_predicted']}"
            )
        if (
            stats["vp_corrected"] is not None
            and correct_prediction_uses != stats["vp_corrected"]
        ):
            raise AnalysisError(
                f"{slice_input.slice_name}: saturated-value correct uses "
                f"{correct_prediction_uses} != VPcorrected "
                f"{stats['vp_corrected']}"
            )
    interval_peak, interval_pc_peak = (
        _sweep_peak(selected) if interval_columns_present else (None, None)
    )
    # The stats counter is the primary capacity metric.  Do not fall back to
    # interval_peak here: that would conflate an offline sequence-boundary
    # sweep with the online live value set the hardware needs to retain.
    peak = stats_peak
    peak_source = "stats" if stats_peak is not None else "unavailable"

    per_pc: List[Dict[str, object]] = []
    for (tid, pc), value_set in sorted(pcs.items()):
        segments_for_pc = pc_segments[(tid, pc)]
        owner_fanouts = [len(values[value]) for value in value_set]
        pc_prediction_uses = (
            sum(segment.prediction_uses or 0 for segment in segments_for_pc)
            if usage_columns_present
            else None
        )
        pc_correct_prediction_uses = (
            sum(
                segment.correct_prediction_uses or 0
                for segment in segments_for_pc
            )
            if usage_columns_present
            else None
        )
        starts = [segment.start for segment in segments_for_pc if segment.start not in (None, 0)]
        ends = [
            segment.end
            for segment in segments_for_pc
            if segment.end not in (None, 0) and not segment.open_at_end
        ]
        per_pc.append(
            {
                "slice": slice_input.slice_name,
                "benchmark": benchmark_for(slice_input.slice_name),
                "scope": scope,
                "tid": tid,
                "pc": _fmt_value(pc),
                "distinct_saturated_values": len(value_set),
                "saturated_value_segments": len(segments_for_pc),
                "saturation_epochs": len({segment.epoch for segment in segments_for_pc}),
                "prediction_uses": pc_prediction_uses,
                "correct_prediction_uses": pc_correct_prediction_uses,
                "wrong_prediction_uses": (
                    pc_prediction_uses - pc_correct_prediction_uses
                    if pc_prediction_uses is not None
                    and pc_correct_prediction_uses is not None
                    else None
                ),
                "coverage_contribution_pct": (
                    pc_correct_prediction_uses / stats["vp_supported"] * 100.0
                    if pc_correct_prediction_uses is not None
                    and stats["vp_supported"]
                    else None
                ),
                "first_saturation_start_seq_no": min(starts) if starts else 0,
                "last_saturation_end_seq_no": max(ends) if ends else 0,
                "value_sharing_fanout_mean": mean_or_none(owner_fanouts),
                "value_sharing_fanout_max": max(owner_fanouts, default=0),
            }
        )

    per_value: List[Dict[str, object]] = []
    for value, owners in sorted(values.items()):
        segments_for_value = value_segments[value]
        value_prediction_uses = (
            sum(segment.prediction_uses or 0 for segment in segments_for_value)
            if usage_columns_present
            else None
        )
        value_correct_prediction_uses = (
            sum(
                segment.correct_prediction_uses or 0
                for segment in segments_for_value
            )
            if usage_columns_present
            else None
        )
        starts = [segment.start for segment in segments_for_value if segment.start not in (None, 0)]
        ends = [
            segment.end
            for segment in segments_for_value
            if segment.end not in (None, 0) and not segment.open_at_end
        ]
        per_value.append(
            {
                "slice": slice_input.slice_name,
                "benchmark": benchmark_for(slice_input.slice_name),
                "scope": scope,
                "saturated_value": _fmt_value(value),
                "sharing_fanout": len(owners),
                "sharing_pc_fanout": len({pc for _tid, pc in owners}),
                "saturated_value_segments": len(segments_for_value),
                "prediction_uses": value_prediction_uses,
                "correct_prediction_uses": value_correct_prediction_uses,
                "first_saturation_start_seq_no": min(starts) if starts else 0,
                "last_saturation_end_seq_no": max(ends) if ends else 0,
                "open_at_scope_start": int(
                    any(segment.open_at_scope_start for segment in segments_for_value)
                ),
                "open_at_end": int(any(segment.open_at_end for segment in segments_for_value)),
            }
        )

    fanout_stats = {
        "mean": mean_or_none(fanouts),
        "p50": percentile(fanouts, 50),
        "p90": percentile(fanouts, 90),
        "p95": percentile(fanouts, 95),
        "p99": percentile(fanouts, 99),
        "max": max(fanouts, default=0),
    }
    interval_peak_differs_from_online_stats = (
        int(stats_peak is not None and interval_peak is not None and stats_peak != interval_peak)
    )
    per_slice: Dict[str, object] = {
        "slice": slice_input.slice_name,
        "benchmark": benchmark_for(slice_input.slice_name),
        "scope": scope,
        "source": str(slice_input.value_path),
        "stats_path": str(slice_input.stats_path) if slice_input.stats_path else "",
        "profile_version": VALUE_VERSION,
        "segments": len(selected),
        "pc_entries": len(pcs),
        # cumulative_* intentionally counts one value slot per PC.  The
        # global_* field is the union after value sharing is applied.
        "cumulative_distinct_values": cumulative_distinct_values,
        "global_distinct_saturated_values": global_distinct_values,
        "distinct_saturated_values": global_distinct_values,
        "value_sharing_saved_slots": cumulative_distinct_values - global_distinct_values,
        "values_with_fanout_gt1": sum(fanout > 1 for fanout in fanouts),
        "value_sharing_fanout_mean": fanout_stats["mean"],
        "value_sharing_fanout_p50": fanout_stats["p50"],
        "value_sharing_fanout_p90": fanout_stats["p90"],
        "value_sharing_fanout_p95": fanout_stats["p95"],
        "value_sharing_fanout_p99": fanout_stats["p99"],
        "value_sharing_fanout_max": fanout_stats["max"],
        "prediction_use_columns_present": int(usage_columns_present),
        "prediction_uses": prediction_uses,
        "correct_prediction_uses": correct_prediction_uses,
        "wrong_prediction_uses": (
            prediction_uses - correct_prediction_uses
            if prediction_uses is not None and correct_prediction_uses is not None
            else None
        ),
        "stats_vp_supported": stats["vp_supported"],
        "stats_vp_predicted": stats["vp_predicted"],
        "stats_vp_corrected": stats["vp_corrected"],
        "coverage_contribution_pct": (
            correct_prediction_uses / stats["vp_supported"] * 100.0
            if correct_prediction_uses is not None and stats["vp_supported"]
            else None
        ),
        # Main hardware-capacity metric: a live set maintained on the
        # committed update path by IdealConstantLVP.
        "concurrent_saturated_pc_peak": stats_pc_peak,
        "concurrent_saturated_pc_peak_source": (
            "stats" if stats_pc_peak is not None else "unavailable"
        ),
        "concurrent_distinct_value_peak": peak,
        "concurrent_distinct_value_peak_source": peak_source,
        # Auxiliary offline sweep of dumped saturated-value segment spans.
        # It is intentionally not a substitute for the metric above.
        "interval_concurrent_distinct_value_peak": interval_peak,
        "interval_concurrent_saturated_pc_peak": interval_pc_peak,
        "interval_columns_present": int(interval_columns_present),
        "stats_peak_distinct_saturated_values": stats_peak,
        "interval_peak_differs_from_online_stats": interval_peak_differs_from_online_stats,
    }
    audit = {
        "metadata": metadata,
        "interval_columns_present": interval_columns_present,
        "prediction_use_columns_present": usage_columns_present,
        "stats_peak": stats_peak,
        "stats": stats,
        "interval_peak": interval_peak,
        "rows_all_scopes": len(rows),
    }
    return per_slice, per_pc, per_value, audit


def _json_safe(value: object) -> object:
    if isinstance(value, float) and not math.isfinite(value):
        return None
    if isinstance(value, dict):
        return {key: _json_safe(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [_json_safe(item) for item in value]
    return value


def write_csv(path: Path, rows: Sequence[Dict[str, object]], fields: Sequence[str]) -> None:
    with path.open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(fields), extrasaction="ignore")
        writer.writeheader()
        for row in rows:
            writer.writerow({field: _json_safe(row.get(field, "")) for field in fields})


PER_SLICE_FIELDS = (
    "slice",
    "benchmark",
    "scope",
    "source",
    "stats_path",
    "profile_version",
    "segments",
    "pc_entries",
    "cumulative_distinct_values",
    "global_distinct_saturated_values",
    "distinct_saturated_values",
    "value_sharing_saved_slots",
    "values_with_fanout_gt1",
    "value_sharing_fanout_mean",
    "value_sharing_fanout_p50",
    "value_sharing_fanout_p90",
    "value_sharing_fanout_p95",
    "value_sharing_fanout_p99",
    "value_sharing_fanout_max",
    "prediction_use_columns_present",
    "prediction_uses",
    "correct_prediction_uses",
    "wrong_prediction_uses",
    "stats_vp_supported",
    "stats_vp_predicted",
    "stats_vp_corrected",
    "coverage_contribution_pct",
    "concurrent_saturated_pc_peak",
    "concurrent_saturated_pc_peak_source",
    "concurrent_distinct_value_peak",
    "concurrent_distinct_value_peak_source",
    "interval_concurrent_distinct_value_peak",
    "interval_concurrent_saturated_pc_peak",
    "interval_columns_present",
    "stats_peak_distinct_saturated_values",
    "interval_peak_differs_from_online_stats",
)

PER_PC_FIELDS = (
    "slice",
    "benchmark",
    "scope",
    "tid",
    "pc",
    "distinct_saturated_values",
    "saturated_value_segments",
    "saturation_epochs",
    "prediction_uses",
    "correct_prediction_uses",
    "wrong_prediction_uses",
    "coverage_contribution_pct",
    "first_saturation_start_seq_no",
    "last_saturation_end_seq_no",
    "value_sharing_fanout_mean",
    "value_sharing_fanout_max",
)

PER_VALUE_FIELDS = (
    "slice",
    "benchmark",
    "scope",
    "saturated_value",
    "sharing_fanout",
    "sharing_pc_fanout",
    "saturated_value_segments",
    "prediction_uses",
    "correct_prediction_uses",
    "first_saturation_start_seq_no",
    "last_saturation_end_seq_no",
    "open_at_scope_start",
    "open_at_end",
)


def _summary_for(
    rows: Sequence[Dict[str, object]],
    audits: Sequence[Dict[str, object]],
    scope: str,
) -> Dict[str, object]:
    def numeric(field: str) -> List[float]:
        return [float(row[field]) for row in rows if row.get(field) is not None]

    def distribution(field: str) -> Dict[str, object]:
        values = numeric(field)
        return {
            "count": len(values),
            "p50": percentile(values, 50),
            "p90": percentile(values, 90),
            "p95": percentile(values, 95),
            "p99": percentile(values, 99),
            "max": max(values) if values else None,
            "sum_across_slices": sum(values) if values else 0,
        }

    source_counts: Dict[str, int] = defaultdict(int)
    pc_peak_source_counts: Dict[str, int] = defaultdict(int)
    for row in rows:
        source_counts[str(row["concurrent_distinct_value_peak_source"])] += 1
        pc_peak_source_counts[str(row["concurrent_saturated_pc_peak_source"])] += 1
    metadata_values: Dict[str, Set[str]] = defaultdict(set)
    for audit in audits:
        for key, value in audit["metadata"].items():
            metadata_values[key].add(value)
    return {
        "scope": scope,
        "profile_version": VALUE_VERSION,
        "slices": len(rows),
        "segments_sum_across_slices": sum(int(row["segments"]) for row in rows),
        "pc_entries_sum_across_slices": sum(int(row["pc_entries"]) for row in rows),
        "global_distinct_values_sum_across_slices": sum(
            int(row["global_distinct_saturated_values"]) for row in rows
        ),
        "prediction_use_profile_slices": sum(
            int(row["prediction_use_columns_present"]) for row in rows
        ),
        "peak_source_counts": dict(sorted(source_counts.items())),
        "pc_peak_source_counts": dict(sorted(pc_peak_source_counts.items())),
        "slices_with_interval_peak_different_from_online_stats": sum(
            int(row["interval_peak_differs_from_online_stats"]) for row in rows
        ),
        "interval_profile_slices": sum(int(row["interval_columns_present"]) for row in rows),
        "distributions": {
            "cumulative_distinct_values": distribution("cumulative_distinct_values"),
            "global_distinct_saturated_values": distribution(
                "global_distinct_saturated_values"
            ),
            "value_sharing_fanout_max": distribution("value_sharing_fanout_max"),
            "coverage_contribution_pct": distribution(
                "coverage_contribution_pct"
            ),
            "concurrent_distinct_value_peak": distribution(
                "concurrent_distinct_value_peak"
            ),
            "concurrent_saturated_pc_peak": distribution(
                "concurrent_saturated_pc_peak"
            ),
            "interval_concurrent_distinct_value_peak": distribution(
                "interval_concurrent_distinct_value_peak"
            ),
            "interval_concurrent_saturated_pc_peak": distribution(
                "interval_concurrent_saturated_pc_peak"
            ),
        },
        "metadata": {
            key: sorted(values) for key, values in sorted(metadata_values.items())
        },
    }


def run(inputs: Sequence[Path], scope: str, out_dir: Path) -> Dict[str, object]:
    discovered = discover_inputs(inputs)
    per_slice: List[Dict[str, object]] = []
    per_pc: List[Dict[str, object]] = []
    per_value: List[Dict[str, object]] = []
    audits: List[Dict[str, object]] = []
    for slice_name in sorted(discovered):
        row, pc_rows, value_rows, audit = analyze_slice(discovered[slice_name], scope)
        per_slice.append(row)
        per_pc.extend(pc_rows)
        per_value.extend(value_rows)
        audits.append(audit)

    out_dir.mkdir(parents=True, exist_ok=True)
    write_csv(out_dir / "per_slice_values.csv", per_slice, PER_SLICE_FIELDS)
    write_csv(out_dir / "per_pc_values.csv", per_pc, PER_PC_FIELDS)
    write_csv(out_dir / "per_value_sharing.csv", per_value, PER_VALUE_FIELDS)
    summary = _summary_for(per_slice, audits, scope)
    (out_dir / "summary.json").write_text(
        json.dumps(_json_safe(summary), indent=2, ensure_ascii=False) + "\n"
    )
    return summary


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    try:
        summary = run(args.inputs, args.scope, args.out_dir)
    except (OSError, AnalysisError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    print(
        json.dumps(
            {"scope": args.scope, "slices": summary["slices"], "out_dir": str(args.out_dir)},
            ensure_ascii=False,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
