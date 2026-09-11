#!/usr/bin/env python3
"""Compute a compact, complete GEM5/RTL slice gap contribution report."""

import argparse
import csv
import json
import math
import sys
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional


def load(path: Path) -> Any:
    with path.open(encoding="utf-8") as source:
        return json.load(source)


def records_from(payload: Any) -> List[Dict[str, Any]]:
    if isinstance(payload, list):
        return payload
    if isinstance(payload, dict) and isinstance(payload.get("slices"), list):
        return payload["slices"]
    raise ValueError("input must be a JSON list or an object with a 'slices' list")


def read_measurement(value: Any) -> Dict[str, Any]:
    if isinstance(value, str):
        value = load(Path(value))
    if not isinstance(value, dict):
        return {}
    measurement = value.get("measurement")
    return measurement if isinstance(measurement, dict) else value


def numeric(mapping: Dict[str, Any], *names: str) -> Optional[float]:
    for name in names:
        value = mapping.get(name)
        if isinstance(value, (int, float)) and math.isfinite(float(value)):
            return float(value)
    return None


def identity(record: Dict[str, Any]) -> str:
    if record.get("identity"):
        return str(record["identity"])
    benchmark = record.get("benchmark", "unknown")
    slice_id = record.get("slice", record.get("slice_id", "unknown"))
    return f"{benchmark}:{slice_id}"


def make_row(record: Dict[str, Any]) -> Dict[str, Any]:
    row: Dict[str, Any] = {
        "identity": identity(record),
        "benchmark": record.get("benchmark"),
        "slice": record.get("slice", record.get("slice_id")),
        "weight": numeric(record, "weight"),
        "weight_source": record.get("weight_source", "unavailable"),
        "status": "included",
        "exclusion_reason": "",
    }
    gem5 = read_measurement(record.get("gem5"))
    rtl = read_measurement(record.get("rtl"))
    row.update(
        {
            "gem5_committed_insts": numeric(gem5, "committed_insts", "committedInsts"),
            "gem5_cycles": numeric(gem5, "num_cycles", "cycles", "clock_cycle"),
            "gem5_ipc": numeric(gem5, "computed_ipc", "ipc"),
            "rtl_committed_insts": numeric(rtl, "committed_insts", "commitInstr"),
            "rtl_cycles": numeric(rtl, "num_cycles", "cycles", "clock_cycle"),
            "rtl_ipc": numeric(rtl, "computed_ipc", "ipc"),
        }
    )
    if isinstance(record.get("weight"), (int, float)):
        row["weight"] = float(record["weight"])
    valid_flags = [record.get("gem5_valid", True), record.get("rtl_valid", True)]
    if isinstance(record.get("gem5"), dict) and "valid_for_default_40m_20m_contract" in record["gem5"]:
        valid_flags.append(record["gem5"]["valid_for_default_40m_20m_contract"])
    if isinstance(record.get("rtl"), dict) and "window_valid_for_comparison" in record["rtl"]:
        valid_flags.append(record["rtl"]["window_valid_for_comparison"])
    missing = []
    if row["weight"] is None:
        missing.append("weight")
    if row["gem5_cycles"] is None:
        missing.append("GEM5 cycles")
    if row["rtl_cycles"] is None:
        missing.append("RTL cycles")
    if not all(flag is True for flag in valid_flags):
        missing.append("measurement window")
    if missing:
        row["status"] = "excluded"
        row["exclusion_reason"] = "; ".join(missing)
        return row
    row["extra_cycles"] = row["gem5_cycles"] - row["rtl_cycles"]
    row["weighted_extra_cycles"] = row["weight"] * row["extra_cycles"]
    return row


def write_csv(rows: Iterable[Dict[str, Any]], path: Path) -> None:
    fields = [
        "rank", "identity", "benchmark", "slice", "weight", "weight_source",
        "gem5_committed_insts", "gem5_cycles", "gem5_ipc", "rtl_committed_insts",
        "rtl_cycles", "rtl_ipc", "extra_cycles", "weighted_extra_cycles",
        "signed_contribution_pct", "absolute_contribution_pct", "status", "exclusion_reason",
    ]
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        writer.writerows({field: row.get(field, "") for field in fields} for row in rows)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Emit complete weighted GEM5/RTL slice-gap contribution CSV and summary JSON."
    )
    parser.add_argument("input", type=Path, help="normalized JSON list; see analysis-rules.md")
    parser.add_argument("--output-csv", type=Path, required=True)
    parser.add_argument("--output-summary", type=Path, required=True)
    parser.add_argument(
        "--unstable-ratio",
        type=float,
        default=0.01,
        help="mark signed percentages unstable when abs(net)/abs(total) is below this ratio",
    )
    args = parser.parse_args()
    if not args.input.is_file():
        parser.error(f"not a readable input: {args.input}")
    if args.unstable_ratio < 0:
        parser.error("--unstable-ratio must be non-negative")
    try:
        rows = [make_row(record) for record in records_from(load(args.input))]
    except (OSError, ValueError, json.JSONDecodeError) as error:
        parser.error(str(error))
    included = [row for row in rows if row["status"] == "included"]
    included.sort(key=lambda row: row["weighted_extra_cycles"], reverse=True)
    net = sum(row["weighted_extra_cycles"] for row in included)
    absolute = sum(abs(row["weighted_extra_cycles"]) for row in included)
    unstable = absolute == 0 or abs(net) / absolute < args.unstable_ratio
    for rank, row in enumerate(included, start=1):
        row["rank"] = rank
        row["signed_contribution_pct"] = "unstable" if unstable else 100.0 * row["weighted_extra_cycles"] / net
        row["absolute_contribution_pct"] = (
            None
            if absolute == 0
            else 100.0 * abs(row["weighted_extra_cycles"]) / absolute
        )
    excluded = [row for row in rows if row["status"] == "excluded"]
    for row in excluded:
        row["rank"] = ""
    ordered = included + excluded
    write_csv(ordered, args.output_csv)
    summary = {
        "input": str(args.input.resolve()),
        "output_csv": str(args.output_csv.resolve()),
        "common_slice_count": len(rows),
        "included_slice_count": len(included),
        "excluded_slice_count": len(excluded),
        "net_weighted_cycle_gap": net,
        "absolute_weighted_cycle_gap": absolute,
        "signed_percentages": "unstable" if unstable else "defined",
        "unstable_ratio_threshold": args.unstable_ratio,
        "weight_coverage": (
            sum(row["weight"] for row in included)
            / sum(row["weight"] for row in rows if row["weight"] is not None)
            if any(row["weight"] is not None for row in rows)
            else None
        ),
        "excluded_reasons": {
            reason: sum(1 for row in excluded if row["exclusion_reason"] == reason)
            for reason in sorted({row["exclusion_reason"] for row in excluded})
        },
        "positive_contribution_top": [row["identity"] for row in included if row["weighted_extra_cycles"] > 0][:10],
        "negative_contribution_top": [
            row["identity"]
            for row in sorted(included, key=lambda row: row["weighted_extra_cycles"])
            if row["weighted_extra_cycles"] < 0
        ][:10],
    }
    args.output_summary.parent.mkdir(parents=True, exist_ok=True)
    args.output_summary.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(args.output_csv)
    print(args.output_summary)
    return 0


if __name__ == "__main__":
    sys.exit(main())
