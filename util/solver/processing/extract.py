from __future__ import annotations

import csv
from pathlib import Path


def parse_stats_file(path: str | Path) -> dict[str, float]:
    metrics: dict[str, float] = {}
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for raw_line in handle:
            line = raw_line.split("#", 1)[0].strip()
            if not line or line.startswith("-"):
                continue
            fields = line.split()
            if len(fields) < 2:
                continue
            name = fields[0]
            try:
                value = float(fields[1])
            except ValueError:
                continue
            metrics[name] = value
    return metrics


def parse_score_file(path: str | Path) -> dict[str, float]:
    metrics: dict[str, float] = {}
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for raw_line in handle:
            if ":" not in raw_line:
                continue
            name, value = raw_line.rsplit(":", 1)
            try:
                metrics[name.strip()] = float(value.strip().split()[0])
            except ValueError:
                continue
    return metrics


def parse_weighted_stats_csv(path: str | Path) -> dict[str, dict[str, float]]:
    results: dict[str, dict[str, float]] = {}
    with open(path, "r", encoding="utf-8", errors="replace", newline="") as handle:
        reader = csv.reader(handle)
        header = next(reader, None)
        if not header or len(header) < 2:
            return results
        metric_names = [name.strip() for name in header[1:]]
        for row in reader:
            if not row:
                continue
            benchmark = row[0].strip()
            if not benchmark:
                continue
            metrics: dict[str, float] = {}
            for metric_name, value in zip(metric_names, row[1:]):
                if not metric_name:
                    continue
                try:
                    metrics[metric_name] = float(value)
                except ValueError:
                    continue
            results[benchmark] = metrics
    return results


def collect_workload_stats(spec_dir: str | Path, metric_name: str) -> dict[str, float]:
    spec_dir = Path(spec_dir)
    results = {}
    for workload_dir in sorted(path for path in spec_dir.iterdir() if path.is_dir()):
        candidate_paths = [
            workload_dir / "stats.txt",
            workload_dir / "m5out" / "stats.txt",
        ]
        for stats_path in candidate_paths:
            if not stats_path.is_file():
                continue
            metrics = parse_stats_file(stats_path)
            if metric_name in metrics:
                results[workload_dir.name] = metrics[metric_name]
                break
    return results


def collect_weighted_stats(path: str | Path, metric_name: str) -> dict[str, float]:
    rows = parse_weighted_stats_csv(path)
    results = {}
    for benchmark, metrics in rows.items():
        if metric_name in metrics:
            results[benchmark] = metrics[metric_name]
    return results


def count_abort_files(spec_dir: str | Path) -> int:
    return sum(1 for _ in Path(spec_dir).rglob("abort"))


def count_workload_dirs(spec_dir: str | Path) -> int:
    spec_dir = Path(spec_dir)
    if not spec_dir.exists():
        return 0
    return sum(1 for path in spec_dir.iterdir() if path.is_dir())
