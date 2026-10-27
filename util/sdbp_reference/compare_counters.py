# Copyright (c) 2026 Institute of Computing Technology, CAS
# SPDX-License-Identifier: BSD-3-Clause

"""Merge all numeric gem5 counters, sorted by name, into a comparison CSV.

Accept phase-specific stats.txt files or counter JSON objects. Multi-block
stats require --block (zero-based) so warmup and measurement are not mixed.
Percent change is 100 * (SDBP - LRU) / LRU; it does not imply improvement.
"""

import argparse
import csv
from decimal import Decimal, InvalidOperation, localcontext
import json
from pathlib import Path


def read_counters(path, block=None):
    if path.suffix == ".json":
        data = json.loads(
            path.read_text(), parse_float=Decimal, parse_int=Decimal
        )
        if not isinstance(data, dict):
            raise ValueError(f"Expected a counter object: {path}")
        return {key: (Decimal(str(value)), "") for key, value in data.items()}

    blocks = []
    current = None
    for line in path.read_text().splitlines():
        if "Begin Simulation Statistics" in line:
            current = {}
        elif "End Simulation Statistics" in line:
            if current is not None:
                blocks.append(current)
            current = None
        elif current is not None:
            values, _, description = line.partition("#")
            fields = values.split()
            if len(fields) < 2:
                continue
            try:
                value = Decimal(fields[1])
            except InvalidOperation:
                continue
            if fields[0] in current:
                raise ValueError(f"Duplicate counter {fields[0]} in {path}")
            current[fields[0]] = (value, description.strip())
    if not blocks:
        raise ValueError(f"No complete stats block: {path}")
    if block is None:
        if len(blocks) != 1:
            raise ValueError(
                f"{path}: found {len(blocks)} blocks; select one with --block"
            )
        block = 0
    try:
        return blocks[block]
    except IndexError as error:
        raise ValueError(f"Invalid block {block} for {path}") from error


def format_number(value):
    if value is None:
        return ""
    return str(value)


def compare(lru, sdbp):
    for name in sorted(lru.keys() | sdbp.keys()):
        old, old_desc = lru.get(name, (None, ""))
        new, new_desc = sdbp.get(name, (None, ""))
        delta = percent = None
        ratio = None
        if old is None:
            status = "missing_lru"
        elif new is None:
            status = "missing_sdbp"
        elif not old.is_finite() or not new.is_finite():
            status = "non_finite"
        else:
            delta = new - old
            if old == 0:
                status = "both_zero" if new == 0 else "zero_baseline"
            else:
                status = "ok"
                percent = delta / old * 100
                ratio = new / old
        yield {
            "counter": name,
            "lru": format_number(old),
            "sdbp": format_number(new),
            "delta_sdbp_minus_lru": format_number(delta),
            "change_pct": "" if percent is None else f"{percent:.8f}",
            "sdbp_over_lru": "" if ratio is None else f"{ratio:.8f}",
            "status": status,
            "description": old_desc or new_desc,
        }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("lru", type=Path)
    parser.add_argument("sdbp", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--block", type=int)
    args = parser.parse_args()
    try:
        lru = read_counters(args.lru, args.block)
        sdbp = read_counters(args.sdbp, args.block)
    except (ValueError, InvalidOperation) as error:
        parser.error(str(error))
    with localcontext() as context:
        context.prec = 50
        rows = list(compare(lru, sdbp))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="", encoding="utf-8-sig") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]) if rows else [
            "counter", "lru", "sdbp", "delta_sdbp_minus_lru", "change_pct",
            "sdbp_over_lru", "status", "description",
        ])
        writer.writeheader()
        writer.writerows(rows)
    statuses = {}
    for row in rows:
        statuses[row["status"]] = statuses.get(row["status"], 0) + 1
    print(json.dumps({"output": str(args.output), "rows": len(rows),
                      "statuses": statuses}, indent=2))


if __name__ == "__main__":
    main()
