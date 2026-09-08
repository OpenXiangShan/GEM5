from __future__ import annotations

import csv
from dataclasses import dataclass
import math
import os
from pathlib import Path
import subprocess
from typing import Sequence


DEFAULT_PANDAS_SITE_PACKAGES = "/nfs/home/yanyue/.local/lib/python3.12/site-packages"
BEGIN_STATS_MARKER = "Begin Simulation Statistics"
END_STATS_MARKER = "End Simulation Statistics"


@dataclass(frozen=True)
class ScoreEvaluationResult:
    return_code: int
    error: str | None = None


class StatsMetricExtractionError(ValueError):
    pass


def _spec_version_from_score_script(score_script: str) -> str:
    return "17" if score_script.endswith("-17.sh") else "06"


def _parse_stats_value(raw_line: str) -> tuple[str, float] | None:
    line = raw_line.split("#", 1)[0].strip()
    if not line or line.startswith("-"):
        return None
    fields = line.split()
    if len(fields) < 2:
        return None
    try:
        value = float(fields[1])
    except ValueError:
        return None
    return fields[0], value


def _read_last_complete_stats_metrics(
    stats_path: Path,
    metric_names: Sequence[str],
) -> dict[str, float]:
    requested = set(metric_names)
    ungrouped: dict[str, float] = {}
    current_block: dict[str, float] | None = None
    last_complete_block: dict[str, float] | None = None
    saw_stats_block = False

    with stats_path.open("r", encoding="utf-8", errors="replace") as handle:
        for raw_line in handle:
            if BEGIN_STATS_MARKER in raw_line:
                saw_stats_block = True
                current_block = {}
                continue
            if END_STATS_MARKER in raw_line:
                if current_block is not None:
                    last_complete_block = current_block
                current_block = None
                continue

            parsed = _parse_stats_value(raw_line)
            if parsed is None:
                continue
            name, value = parsed
            if name not in requested:
                continue
            if current_block is not None:
                current_block[name] = value
            elif not saw_stats_block:
                ungrouped[name] = value

    if saw_stats_block:
        return last_complete_block or {}
    return ungrouped


def _find_workload_stats(raw_spec_dir: Path, workload_name: str) -> Path | None:
    workload_dir = raw_spec_dir / workload_name
    for candidate in (
        workload_dir / "stats.txt",
        workload_dir / "m5out" / "stats.txt",
    ):
        if candidate.is_file():
            return candidate
    return None


def _format_workloads(workloads: Sequence[str]) -> str:
    return ", ".join(sorted(set(workloads)))


def inject_stats_metrics(
    batch_csv: str | Path,
    raw_spec_dir: str | Path,
    metric_names: Sequence[str],
) -> None:
    metrics = tuple(dict.fromkeys(metric for metric in metric_names if metric))
    if not metrics:
        return

    batch_csv = Path(batch_csv)
    raw_spec_dir = Path(raw_spec_dir)
    try:
        with batch_csv.open(
            "r", encoding="utf-8", errors="replace", newline=""
        ) as handle:
            rows = [row for row in csv.reader(handle) if row]
    except OSError as exc:
        raise StatsMetricExtractionError(
            f"failed to read batch csv {batch_csv}: {exc}"
        ) from exc

    if len(rows) < 2:
        raise StatsMetricExtractionError(
            f"batch csv {batch_csv} has no workload rows"
        )

    header = list(rows[0])
    original_width = len(header)
    metric_columns: dict[str, int] = {}
    for metric in metrics:
        occurrences = [index for index, name in enumerate(header) if name == metric]
        if len(occurrences) > 1:
            raise StatsMetricExtractionError(
                f"batch csv {batch_csv} contains duplicate metric column {metric}"
            )
        if occurrences:
            metric_columns[metric] = occurrences[0]
        else:
            metric_columns[metric] = len(header)
            header.append(metric)

    missing_stats_files: list[str] = []
    missing_metrics = {metric: [] for metric in metrics}
    non_finite_metrics = {metric: [] for metric in metrics}
    output_rows = [header]

    for source_row in rows[1:]:
        row = list(source_row)
        if len(row) > original_width:
            raise StatsMetricExtractionError(
                f"batch csv {batch_csv} has an over-wide row: {row[0]!r}"
            )
        row.extend([""] * (len(header) - len(row)))
        workload_name = row[0].strip()
        if not workload_name:
            raise StatsMetricExtractionError(
                f"batch csv {batch_csv} contains a row without a workload name"
            )

        stats_path = _find_workload_stats(raw_spec_dir, workload_name)
        if stats_path is None:
            missing_stats_files.append(workload_name)
            output_rows.append(row)
            continue

        values = _read_last_complete_stats_metrics(stats_path, metrics)
        for metric in metrics:
            value = values.get(metric)
            if value is None:
                missing_metrics[metric].append(workload_name)
                continue
            if not math.isfinite(value):
                non_finite_metrics[metric].append(f"{workload_name}={value}")
                continue
            row[metric_columns[metric]] = repr(value)
        output_rows.append(row)

    issues = []
    if missing_stats_files:
        issues.append(
            "missing stats.txt for workload(s): "
            f"{_format_workloads(missing_stats_files)}"
        )
    for metric in metrics:
        if missing_metrics[metric]:
            issues.append(
                f"metric {metric} missing in workload(s): "
                f"{_format_workloads(missing_metrics[metric])}"
            )
        if non_finite_metrics[metric]:
            issues.append(
                f"metric {metric} is non-finite in workload(s): "
                f"{_format_workloads(non_finite_metrics[metric])}"
            )
    if issues:
        raise StatsMetricExtractionError(
            "dynamic stats extraction failed: " + "; ".join(issues)
        )

    try:
        with batch_csv.open("w", encoding="utf-8", newline="") as handle:
            csv.writer(handle).writerows(output_rows)
    except OSError as exc:
        raise StatsMetricExtractionError(
            f"failed to update batch csv {batch_csv}: {exc}"
        ) from exc


def run_score_evaluator(
    *,
    gem5_data_proc: str | Path,
    score_script: str,
    raw_spec_dir: str | Path,
    cluster_config: str | Path,
    repo_root: str | Path,
    score_path: str | Path,
    score_log: str | Path,
    scratch_dir: str | Path,
    weighted_csv_path: str | Path | None = None,
    emit_score: bool = True,
    stats_metrics: Sequence[str] = (),
) -> ScoreEvaluationResult:
    gem5_data_proc = Path(gem5_data_proc)
    score_path = Path(score_path)
    score_log = Path(score_log)
    scratch_dir = Path(scratch_dir)
    scratch_dir.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    existing_pythonpath = env.get("PYTHONPATH", "")
    env["PYTHONPATH"] = ":".join(
        item
        for item in [str(gem5_data_proc), DEFAULT_PANDAS_SITE_PACKAGES, existing_pythonpath]
        if item
    )
    env["GEM5_HOME"] = str(repo_root)
    results_dir = scratch_dir / "results"
    results_dir.mkdir(parents=True, exist_ok=True)
    batch_csv = results_dir / "gem5-score-example.csv"
    score_csv = results_dir / "gem5-score-example-score.csv" if emit_score else None
    batch_py = gem5_data_proc / "batch.py"
    compute_weighted_py = gem5_data_proc / "simpoint_cpt" / "compute_weighted.py"
    spec_version = _spec_version_from_score_script(score_script)

    with score_path.open("w", encoding="utf-8") as out_handle, score_log.open(
        "w", encoding="utf-8"
    ) as log_handle:
        batch_result = subprocess.run(
            [
                "python3",
                str(batch_py),
                "-s",
                str(raw_spec_dir),
                "-o",
                str(batch_csv),
            ],
            check=False,
            cwd=gem5_data_proc,
            env=env,
            stdout=out_handle,
            stderr=log_handle,
        )
        if batch_result.returncode != 0:
            return ScoreEvaluationResult(batch_result.returncode)

        try:
            inject_stats_metrics(batch_csv, raw_spec_dir, stats_metrics)
        except StatsMetricExtractionError as exc:
            message = str(exc)
            log_handle.write(message + "\n")
            return ScoreEvaluationResult(1, message)

        command = [
            "python3",
            str(compute_weighted_py),
            "-r",
            str(batch_csv),
            "-j",
            str(cluster_config),
            "--out-dir",
            str(results_dir),
            "-v",
            spec_version,
        ]
        if weighted_csv_path is not None:
            command.extend(["-o", str(weighted_csv_path)])
        if score_csv is not None:
            command.extend(["--score", str(score_csv)])
        result = subprocess.run(
            command,
            check=False,
            cwd=gem5_data_proc,
            env=env,
            stdout=out_handle,
            stderr=log_handle,
        )
    return ScoreEvaluationResult(result.returncode)
