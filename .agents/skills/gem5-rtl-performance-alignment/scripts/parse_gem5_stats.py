#!/usr/bin/env python3
"""Extract only the measurement (second) statistics block from GEM5 stats."""

import argparse
import json
import math
import re
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional


BEGIN = "---------- Begin Simulation Statistics ----------"
END = "---------- End Simulation Statistics"
NUMBER = re.compile(
    r"^\s*(?P<name>\S+)\s+(?P<value>[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?|nan|inf|-inf)\b"
)


def parse_number(value: str) -> Any:
    lower = value.lower()
    if lower in {"nan", "inf", "-inf"}:
        return lower
    number = float(value)
    return int(number) if number.is_integer() else number


def parse_blocks(path: Path) -> List[Dict[str, Any]]:
    blocks: List[Dict[str, Any]] = []
    current: Optional[Dict[str, Any]] = None
    with path.open(encoding="utf-8", errors="replace") as stats_file:
        for line_number, line in enumerate(stats_file, start=1):
            if line.startswith(BEGIN):
                if current is not None:
                    current["warnings"].append("new begin marker before end marker")
                    current["end_line"] = line_number - 1
                    blocks.append(current)
                current = {
                    "start_line": line_number,
                    "end_line": None,
                    "stats": {},
                    "warnings": [],
                }
                continue
            if current is None:
                continue
            if line.startswith(END):
                current["end_line"] = line_number
                blocks.append(current)
                current = None
                continue
            match = NUMBER.match(line)
            if match is not None:
                current["stats"][match.group("name")] = parse_number(
                    match.group("value")
                )
    if current is not None:
        current["warnings"].append("unterminated statistics block")
        blocks.append(current)
    return blocks


def first_value(stats: Dict[str, Any], names: List[str]) -> Optional[Any]:
    for name in names:
        if name in stats:
            return stats[name]
    for name in names:
        matches = sorted(key for key in stats if key.endswith(name))
        if matches:
            return stats[matches[0]]
    return None


def make_measurement(
    block: Dict[str, Any], expected_insts: int, tolerance: float
) -> Dict[str, Any]:
    stats = block["stats"]
    committed = first_value(
        stats,
        [
            "system.cpu.committedInsts",
            "system.cpu.committedInsts::total",
            "committedInsts",
        ],
    )
    cycles = first_value(
        stats,
        ["system.cpu.numCycles", "system.cpu.numCycles::total", "numCycles"],
    )
    reported_ipc = first_value(
        stats,
        ["system.cpu.ipc", "system.cpu.totalIpc", "ipc", "totalIpc"],
    )
    ipc = None
    if isinstance(committed, (int, float)) and isinstance(cycles, (int, float)) and cycles > 0:
        ipc = committed / cycles

    warnings: List[str] = []
    length_ok = False
    if isinstance(committed, (int, float)):
        length_ok = abs(committed - expected_insts) <= expected_insts * tolerance
        if not length_ok:
            warnings.append(
                f"committed instructions {committed} are not within {tolerance:.1%} "
                f"of expected measurement length {expected_insts}"
            )
    else:
        warnings.append("measurement block has no recognized committedInsts counter")
    if not isinstance(cycles, (int, float)) or cycles <= 0:
        warnings.append("measurement block has no positive recognized numCycles counter")
    if ipc is not None and isinstance(reported_ipc, (int, float)):
        if not math.isclose(ipc, reported_ipc, rel_tol=5e-5, abs_tol=5e-8):
            warnings.append(
                f"computed IPC {ipc:.9g} differs from reported IPC {reported_ipc:.9g}"
            )
    return {
        "block_index_zero_based": 1,
        "block_role": "second-statistics-block-post-warmup-measurement",
        "start_line": block["start_line"],
        "end_line": block["end_line"],
        "committed_insts": committed,
        "num_cycles": cycles,
        "computed_ipc": ipc,
        "reported_ipc": reported_ipc,
        "expected_measurement_insts": expected_insts,
        "measurement_length_ok": length_ok,
        "warnings": block["warnings"] + warnings,
    }


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
            "Extract GEM5's second stats block as the post-warmup measurement; "
            "never combines statistics blocks."
        )
    )
    parser.add_argument("stats_file", type=Path, help="path to GEM5 stats.txt")
    parser.add_argument("--output", type=Path, help="optional JSON output path")
    parser.add_argument(
        "--expected-measurement-insts",
        type=int,
        default=20_000_000,
        help="expected committed instructions in block 2 (default: 20M)",
    )
    parser.add_argument(
        "--inst-tolerance",
        type=float,
        default=0.02,
        help="allowed relative instruction-count difference (default: 0.02)",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="return nonzero when the normal two-block / post-warmup contract fails",
    )
    args = parser.parse_args()

    if args.expected_measurement_insts <= 0 or not 0 <= args.inst_tolerance < 1:
        parser.error("expected measurement instructions must be positive and tolerance in [0, 1)")
    if not args.stats_file.is_file():
        parser.error(f"not a readable file: {args.stats_file}")

    blocks = parse_blocks(args.stats_file)
    block_summaries = [
        {
            "index_zero_based": index,
            "start_line": block["start_line"],
            "end_line": block["end_line"],
            "recognized_committed_insts": first_value(
                block["stats"], ["system.cpu.committedInsts", "committedInsts"]
            ),
            "recognized_num_cycles": first_value(
                block["stats"], ["system.cpu.numCycles", "numCycles"]
            ),
            "warnings": block["warnings"],
        }
        for index, block in enumerate(blocks)
    ]
    measurement = (
        make_measurement(blocks[1], args.expected_measurement_insts, args.inst_tolerance)
        if len(blocks) >= 2
        else None
    )
    warnings: List[str] = []
    if len(blocks) != 2:
        warnings.append(
            f"expected exactly two statistics blocks (warmup + measurement), found {len(blocks)}"
        )
    if measurement is None:
        warnings.append("second statistics block is unavailable")
    elif not measurement["measurement_length_ok"]:
        warnings.append("second statistics block does not match expected measurement length")
    valid = len(blocks) == 2 and measurement is not None and measurement["measurement_length_ok"]
    payload = {
        "input": str(args.stats_file.resolve()),
        "comparison_window": "post-warmup measurement only (second stats block)",
        "stats_block_count": len(blocks),
        "blocks": block_summaries,
        "measurement": measurement,
        "valid_for_default_40m_20m_contract": valid,
        "warnings": warnings,
    }
    emit(payload, args.output)
    if args.strict and not valid:
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
