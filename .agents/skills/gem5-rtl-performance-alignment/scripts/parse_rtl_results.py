#!/usr/bin/env python3
"""Extract RTL PERF samples without assuming their warmup/reset semantics."""

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple


PERF = re.compile(
    r"^\[PERF\s*\]\[time=\s*(?P<time>\d+)\]\s*"
    r"(?P<unit>.*?):\s*(?P<counter>[^,]+),\s*(?P<value>[-+]?\d+(?:\.\d+)?)\s*$"
)
DEFAULT_LOG_NAMES = (
    "simulator_err.txt",
    "simulator_out.txt",
    "run.log",
    "stdout.log",
    "stderr.log",
)


def is_rtl_log_name(name: str) -> bool:
    return name in DEFAULT_LOG_NAMES or any(name.endswith(f"_{suffix}") for suffix in DEFAULT_LOG_NAMES)


def number(value: str) -> Any:
    parsed = float(value)
    return int(parsed) if parsed.is_integer() else parsed


def candidate_logs(source: Path) -> List[Path]:
    if source.is_file():
        return [source]
    if not source.is_dir():
        raise FileNotFoundError(source)
    logs = [
        path
        for path in source.iterdir()
        if path.is_file() and is_rtl_log_name(path.name)
    ]
    if logs:
        return logs
    return sorted(
        path
        for path in source.iterdir()
        if path.is_file() and path.suffix.lower() in {".log", ".out", ".err", ".txt"}
    )


def read_samples(paths: Iterable[Path]) -> Tuple[List[Dict[str, Any]], List[str]]:
    samples: List[Dict[str, Any]] = []
    warnings: List[str] = []
    for path in paths:
        try:
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError as error:
            warnings.append(f"could not read {path}: {error}")
            continue
        for line_number, line in enumerate(lines, start=1):
            match = PERF.match(line)
            if match is None:
                continue
            samples.append(
                {
                    "source": str(path.resolve()),
                    "line": line_number,
                    "time": int(match.group("time")),
                    "unit": match.group("unit"),
                    "counter": match.group("counter").strip(),
                    "value": number(match.group("value")),
                }
            )
    return samples, warnings


def matching(samples: List[Dict[str, Any]], counter_name: str) -> List[Dict[str, Any]]:
    exact = [sample for sample in samples if sample["counter"] == counter_name]
    if exact:
        return exact
    return [sample for sample in samples if sample["counter"].endswith(counter_name)]


def at_occurrence(
    samples: List[Dict[str, Any]], occurrence: int, label: str, warnings: List[str]
) -> Optional[Dict[str, Any]]:
    if not samples:
        warnings.append(f"no PERF samples found for {label}")
        return None
    try:
        return samples[occurrence]
    except IndexError:
        warnings.append(
            f"{label} occurrence {occurrence} is out of range for {len(samples)} samples"
        )
        return None


def parse_weight(source: Path, explicit_weight: Optional[float]) -> Dict[str, Any]:
    if explicit_weight is not None:
        return {"value": explicit_weight, "source": "--weight"}
    name = source.name
    match = re.match(r"^.+_\d{3,}_(?P<weight>[-+]?\d+(?:\.\d+)?(?:[eE][-+]?\d+)?)$", name)
    if match is None:
        return {"value": None, "source": "unavailable"}
    value = float(match.group("weight"))
    return {"value": value, "source": "directory-basename-final-token"}


def emit(payload: Dict[str, Any], output: Optional[Path]) -> None:
    text = json.dumps(payload, indent=2, sort_keys=True) + "\n"
    if output is None:
        print(text, end="")
        return
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(text, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Extract XiangShan RTL PERF samples. The default 'unknown' window "
            "does not claim that a counter is post-warmup."
        )
    )
    parser.add_argument("source", type=Path, help="RTL result directory or PERF log")
    parser.add_argument("--output", type=Path, help="optional JSON output path")
    parser.add_argument(
        "--window-semantics",
        choices=["unknown", "reset-after-warmup", "cumulative-at-boundaries"],
        default="unknown",
        help="proven counter-window semantics; default does not calculate measurement IPC",
    )
    parser.add_argument("--commit-counter", default="commitInstr")
    parser.add_argument("--cycle-counter", default="clock_cycle")
    parser.add_argument(
        "--measurement-commit-occurrence",
        type=int,
        default=-1,
        help="0-based sample occurrence after a verified reset (default: last)",
    )
    parser.add_argument(
        "--measurement-cycle-occurrence",
        type=int,
        default=-1,
        help="0-based sample occurrence after a verified reset (default: last)",
    )
    parser.add_argument("--warmup-commit-occurrence", type=int)
    parser.add_argument("--warmup-cycle-occurrence", type=int)
    parser.add_argument("--weight", type=float, help="explicit RTL slice weight")
    parser.add_argument(
        "--strict",
        action="store_true",
        help="return nonzero unless a valid post-warmup IPC can be computed",
    )
    args = parser.parse_args()

    try:
        logs = candidate_logs(args.source)
    except FileNotFoundError:
        parser.error(f"not a readable file or directory: {args.source}")
    samples, warnings = read_samples(logs)
    commit_samples = matching(samples, args.commit_counter)
    cycle_samples = matching(samples, args.cycle_counter)
    measurement = None
    window_valid = False
    if args.window_semantics == "reset-after-warmup":
        commit = at_occurrence(
            commit_samples, args.measurement_commit_occurrence, "measurement commit", warnings
        )
        cycles = at_occurrence(
            cycle_samples, args.measurement_cycle_occurrence, "measurement cycle", warnings
        )
        if commit and cycles and cycles["value"] > 0:
            measurement = {
                "committed_insts": commit["value"],
                "num_cycles": cycles["value"],
                "computed_ipc": commit["value"] / cycles["value"],
                "counter_window": "values after verified warmup reset",
                "commit_sample": commit,
                "cycle_sample": cycles,
            }
            window_valid = True
    elif args.window_semantics == "cumulative-at-boundaries":
        required = [
            args.warmup_commit_occurrence,
            args.measurement_commit_occurrence,
            args.warmup_cycle_occurrence,
            args.measurement_cycle_occurrence,
        ]
        if any(value is None for value in required):
            warnings.append(
                "cumulative-at-boundaries requires all warmup and measurement occurrence indices"
            )
        else:
            warmup_commit = at_occurrence(
                commit_samples, args.warmup_commit_occurrence, "warmup commit", warnings
            )
            measurement_commit = at_occurrence(
                commit_samples, args.measurement_commit_occurrence, "measurement commit", warnings
            )
            warmup_cycles = at_occurrence(
                cycle_samples, args.warmup_cycle_occurrence, "warmup cycle", warnings
            )
            measurement_cycles = at_occurrence(
                cycle_samples, args.measurement_cycle_occurrence, "measurement cycle", warnings
            )
            if all((warmup_commit, measurement_commit, warmup_cycles, measurement_cycles)):
                committed = measurement_commit["value"] - warmup_commit["value"]
                cycles = measurement_cycles["value"] - warmup_cycles["value"]
                if committed >= 0 and cycles > 0:
                    measurement = {
                        "committed_insts": committed,
                        "num_cycles": cycles,
                        "computed_ipc": committed / cycles,
                        "counter_window": "measurement boundary minus warmup boundary",
                        "warmup_commit_sample": warmup_commit,
                        "measurement_commit_sample": measurement_commit,
                        "warmup_cycle_sample": warmup_cycles,
                        "measurement_cycle_sample": measurement_cycles,
                    }
                    window_valid = True
                else:
                    warnings.append("counter differences are not a positive measurement window")
    else:
        warnings.append(
            "window semantics are unknown; PERF samples are diagnostic only, not comparable"
        )

    root_for_weight = args.source if args.source.is_dir() else args.source.parent
    payload = {
        "input": str(args.source.resolve()),
        "perf_logs": [str(path.resolve()) for path in logs],
        "window_semantics": args.window_semantics,
        "window_valid_for_comparison": window_valid,
        "commit_counter": args.commit_counter,
        "cycle_counter": args.cycle_counter,
        "commit_samples": commit_samples,
        "cycle_samples": cycle_samples,
        "measurement": measurement,
        "weight": parse_weight(root_for_weight, args.weight),
        "all_perf_sample_count": len(samples),
        "warnings": warnings,
    }
    emit(payload, args.output)
    if args.strict and not window_valid:
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
