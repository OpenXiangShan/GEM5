#!/usr/bin/env python3

from __future__ import annotations

import argparse
import concurrent.futures
import csv
import json
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, List, Tuple

BEGIN = "---------- Begin Simulation Statistics ----------"
END = "---------- End Simulation Statistics   ----------"
DEFAULT_COUNTERS = Path(__file__).resolve().parent.parent / "configs" / "bpu_counters.txt"


@dataclass
class CaseRecord:
    case_path: str
    stats_path: str
    values: Dict[str, float]
    missing: List[str]
    errors: List[str]


def now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def parse_last_stats_block(path: Path) -> Dict[str, float]:
    lines = path.read_text(encoding="utf-8", errors="ignore").splitlines()
    blocks: List[List[str]] = []
    in_block = False
    current: List[str] = []

    for line in lines:
        stripped = line.strip()
        if stripped == BEGIN:
            in_block = True
            current = []
            continue
        if stripped == END and in_block:
            blocks.append(current)
            in_block = False
            continue
        if in_block:
            current.append(line)

    target = blocks[-1] if blocks else lines
    stats: Dict[str, float] = {}
    for line in target:
        if not line or line.startswith("-"):
            continue
        parts = line.split()
        if len(parts) < 2:
            continue
        key, value = parts[0], parts[1]
        try:
            stats[key] = float(value)
        except ValueError:
            continue
    return stats


def load_counters(path: Path) -> List[str]:
    suffix = path.suffix.lower()
    if suffix in {".txt", ""}:
        counters = [line.strip() for line in path.read_text(encoding="utf-8").splitlines()]
        counters = [c for c in counters if c and not c.startswith("#")]
        if not counters:
            raise ValueError(f"no counters found in {path}")
        return counters

    if suffix in {".yml", ".yaml"}:
        import yaml

        payload = yaml.safe_load(path.read_text(encoding="utf-8"))
        if isinstance(payload, list):
            counters = [str(x).strip() for x in payload if str(x).strip()]
        elif isinstance(payload, dict):
            raw = payload.get("counters", [])
            counters = [str(x).strip() for x in raw if str(x).strip()]
        else:
            raise ValueError("yaml must be list or object with counters")
        if not counters:
            raise ValueError(f"no counters found in {path}")
        return counters

    if suffix == ".csv":
        counters: List[str] = []
        with path.open(encoding="utf-8", newline="") as fp:
            reader = csv.DictReader(fp)
            if reader.fieldnames is None:
                raise ValueError(f"invalid csv with no header: {path}")
            column = "counter" if "counter" in reader.fieldnames else reader.fieldnames[0]
            for row in reader:
                value = str(row.get(column, "")).strip()
                if value:
                    counters.append(value)
        if not counters:
            raise ValueError(f"no counters found in {path}")
        return counters

    raise ValueError("counter file must be .txt/.yml/.yaml/.csv")


def analyze_one(stats_path: Path, debug_dir: Path, counters: List[str]) -> CaseRecord:
    case_rel = stats_path.parent.relative_to(debug_dir)
    record = CaseRecord(
        case_path=str(case_rel),
        stats_path=str(stats_path),
        values={},
        missing=[],
        errors=[],
    )

    try:
        stats = parse_last_stats_block(stats_path)
    except Exception as exc:
        record.errors.append(f"parse stats failed: {exc}")
        return record

    values: Dict[str, float] = {}
    missing: List[str] = []
    for counter in counters:
        if counter in stats:
            values[counter] = stats[counter]
        else:
            missing.append(counter)

    record.values = values
    record.missing = missing
    return record


def write_outputs(debug_dir: Path, counters_file: Path, counters: List[str],
                  records: List[CaseRecord]) -> Tuple[Path, Path]:
    summary_json = debug_dir / "bpu_counters_summary.json"
    summary_csv = debug_dir / "bpu_counters_summary.csv"

    payload = {
        "generated_at": now_iso(),
        "debug_dir": str(debug_dir),
        "counters_file": str(counters_file),
        "counters": counters,
        "cases": [
            {
                "case_path": r.case_path,
                "stats_path": r.stats_path,
                "values": r.values,
                "missing": r.missing,
                "errors": r.errors,
            }
            for r in sorted(records, key=lambda x: x.case_path)
        ],
    }
    summary_json.write_text(json.dumps(payload, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    headers = ["case_path", "stats_path", "missing_count", "error_count", *counters]
    with summary_csv.open("w", encoding="utf-8", newline="") as fp:
        writer = csv.DictWriter(fp, fieldnames=headers)
        writer.writeheader()
        for record in sorted(records, key=lambda x: x.case_path):
            row = {
                "case_path": record.case_path,
                "stats_path": record.stats_path,
                "missing_count": len(record.missing),
                "error_count": len(record.errors),
            }
            for counter in counters:
                row[counter] = record.values.get(counter, "")
            writer.writerow(row)

    return summary_json, summary_csv


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Extract raw BPU counters from gem5 stats.txt")
    parser.add_argument("--debug-dir", type=str, required=True, help="Root directory to scan")
    parser.add_argument(
        "--counters-file",
        type=str,
        default=str(DEFAULT_COUNTERS),
        help="Counter list file (.txt/.yml/.yaml/.csv)",
    )
    parser.add_argument(
        "--stats-glob",
        type=str,
        default="**/stats.txt",
        help="Glob under debug-dir to find stats files",
    )
    parser.add_argument("--max-workers", type=int, default=8)
    return parser


def main() -> int:
    args = build_parser().parse_args()

    debug_dir = Path(args.debug_dir).resolve()
    counters_file = Path(args.counters_file).resolve()

    if not debug_dir.exists():
        raise FileNotFoundError(f"debug dir not found: {debug_dir}")
    if not counters_file.is_file():
        raise FileNotFoundError(f"counters file not found: {counters_file}")

    counters = load_counters(counters_file)
    stats_files = sorted(debug_dir.glob(args.stats_glob))

    records: List[CaseRecord] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.max_workers) as executor:
        future_map = {
            executor.submit(analyze_one, stats_path, debug_dir, counters): stats_path
            for stats_path in stats_files
            if stats_path.is_file()
        }
        for future in concurrent.futures.as_completed(future_map):
            stats_path = future_map[future]
            try:
                records.append(future.result())
            except Exception as exc:
                case_rel = stats_path.parent.relative_to(debug_dir)
                records.append(
                    CaseRecord(
                        case_path=str(case_rel),
                        stats_path=str(stats_path),
                        values={},
                        missing=counters,
                        errors=[f"unhandled analysis exception: {exc}"],
                    )
                )

    summary_json, summary_csv = write_outputs(debug_dir, counters_file, counters, records)
    print(f"wrote: {summary_json}")
    print(f"wrote: {summary_csv}")
    print(f"stats files: {len(records)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
