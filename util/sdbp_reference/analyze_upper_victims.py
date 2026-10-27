# Copyright (c) 2026 Institute of Computing Technology, CAS
# SPDX-License-Identifier: BSD-3-Clause

"""Validate committed replacement accounting and report upper snoop sources.

Run analyze_load_stalls.py first to export both phase-counter JSON files.
Presence is observed at the existing post-invalidation eviction snoop, rather
than a client bit sampled by the replacement policy. Source masks can overlap;
MSHR early return means WB observations are not an exhaustive occupancy census.
"""

import argparse
import csv
import json
from pathlib import Path


KINDS = ("other", "sdbpLive", "sdbpDead")


def analyze(counters, variant):
    suffix = ".committedReplacementBlocks"
    caches = sorted(
        key.removesuffix(suffix) for key in counters if key.endswith(suffix)
    )
    rows = []
    for cache in caches:
        def value(name):
            return counters[cache + "." + name]

        committed = value("committedReplacementBlocks")
        emitted = value("replacementPackets")
        assert committed == emitted + value("replacementNoPacket"), cache
        assert committed == (
            value("deadBlockReplacements") + value("liveBlockReplacements")
        ), cache
        classified = 0
        for kind in KINDS:
            states = {
                name: value(f"replacementUpperPresence::{kind}_{name}")
                for name in ("absent", "present", "unknown")
            }
            masks = [
                value(f"replacementUpperSourceMask::{kind}_{mask}")
                for mask in range(8)
            ]
            assert masks[0] == states["absent"], (cache, kind)
            assert sum(masks[1:]) == states["present"], (cache, kind)
            total = sum(states.values())
            classified += total
            if variant == "lru" or ".l2" not in cache:
                assert total == 0 or kind == "other", (cache, kind)
            elif total:
                assert kind != "other", cache
            resident = sum(
                count for mask, count in enumerate(masks) if mask & 1
            )
            rows.append({
                "cache": cache,
                "kind": kind,
                "total": total,
                **states,
                "resident": resident,
                "mshr": sum(
                    count for mask, count in enumerate(masks) if mask & 2
                ),
                "writebuffer": sum(
                    count for mask, count in enumerate(masks) if mask & 4
                ),
                "resident_pct": 100 * resident / total if total else 0,
            })
        assert classified == value("replacementUpperPresence::total"), cache
        # Dumps occur between events. No tagged packets should straddle these
        # phase boundaries for the targeted coherent-cache configuration.
        assert emitted == classified, (cache, emitted, classified)
        assert sum(
            value(f"replacementUpperSourceMask::{kind}_{mask}")
            for kind in KINDS for mask in range(8)
        ) == value("replacementUpperSourceMask::total"), cache
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--baseline", type=Path, required=True)
    args = parser.parse_args()
    rows, reproduction = [], {}
    for variant in ("lru", "sdbp"):
        directory = args.output / f"mcf_4541__{variant}"
        for phase in ("warmup", "measurement"):
            counters = json.loads((directory / f"{phase}-counters.json").read_text())
            rows.extend({"variant": variant, "phase": phase, **row} for row in analyze(counters, variant))
            old = json.loads((args.baseline / directory.name / f"{phase}-counters.json").read_text())
            keys = ["simInsts", "system.cpu.numCycles", "system.cpu.scheduler.memstall_any_load",
                    "system.cpu.scheduler.memstall_l1miss",
                    "system.cpu.scheduler.memstall_l2miss",
                    "system.cpu.scheduler.memstall_l3miss"]
            # Cache counters are deterministic; exclude host time and new stats.
            keys += [key for key in old if any(part in key for part in (".l2", ".dcache", ".l3")) and key in counters]
            differences = {
                key: [old[key], counters[key]] for key in keys
                if old[key] != counters[key]
                and not (str(old[key]) == str(counters[key]) == "nan")
            }
            reproduction[f"{variant}/{phase}"] = {"compared": len(keys), "differences": differences}
            assert not differences, (variant, phase, differences)
    with (args.output / "upper-victim-summary.csv").open("w") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    (args.output / "upper-victim-validation.json").write_text(
        json.dumps({"accounting_valid": True,
                    "baseline_reproduction": reproduction,
                    "rows": rows}, indent=2) + "\n"
    )
    for row in rows:
        if ".l2" in row["cache"] and row["total"]:
            print(row)


if __name__ == "__main__":
    main()
