#!/usr/bin/env python3
# Copyright (c) 2026 Institute of Computing Technology, CAS
# SPDX-License-Identifier: BSD-3-Clause

"""Summarize L2 request origins, preserving raw and SimPoint-weighted counts."""

import argparse
from collections import defaultdict
import csv
import json
from pathlib import Path
import re


SOURCES = ("l1Demand", "l1Prefetch", "l2Prefetch", "instruction", "other")
COUNTERS = {
    "lookups": "l2RequestLookups",
    "accesses": "l2RequestAccesses",
    "tag_hits": "l2RequestTagHits",
    "hits": "l2RequestHits",
    "misses": "l2RequestMisses",
    "mshr_hits": "l2RequestMshrHits",
    "mshr_misses": "l2RequestMshrMisses",
    "write_buffer_hits": "l2RequestWriteBufferHits",
    "arb_retries": "l2RequestArbRetries",
    "no_pc": "l2RequestNoPc",
    "hits_on_prefetched_blocks": "l2RequestHitsOnPrefetchedBlocks",
}
PATTERN = re.compile(r"^(.*?)\.(l2Request\w+)::(" + "|".join(SOURCES) + r")$")
REVERSE = {value: key for key, value in COUNTERS.items()}


def final_stats(path):
    text = path.read_text()
    marker = "---------- Begin Simulation Statistics ----------"
    assert marker in text, f"No statistics interval: {path}"
    final = text.rsplit(marker, 1)[-1]
    assert "---------- End Simulation Statistics   ----------" in final, path
    return final


def parse_point(path, point):
    values = {
        source: {counter: 0.0 for counter in COUNTERS} for source in SOURCES
    }
    found = set()
    legacy = defaultdict(float)
    received = 0.0
    for line in final_stats(path).splitlines():
        tokens = line.split(None, 2)
        if len(tokens) < 2:
            continue
        key, number = tokens[:2]
        match = PATTERN.match(key)
        if match and match[2] in REVERSE:
            source, counter = match[3], REVERSE[match[2]]
            values[source][counter] += float(number)
            found.add((source, counter))
        elif key.endswith(".l2PrefetchReceived"):
            received += float(number)
        elif key.startswith("system.l2_wrappers.prefetcher."):
            suffix = key[len("system.l2_wrappers.prefetcher.") :]
            if suffix in (
                "pfIssued",
                "pfHitInCache",
                "pfHitInMSHR",
                "pfHitInWB",
            ):
                legacy[suffix] = float(number)
        elif (
            key.startswith("system.l2_wrappers.slices")
            and ".inner_cache." in key
        ):
            prefix, suffix = key.split(".inner_cache.", 1)
            command = suffix.split(".", 1)[0]
            if command in (
                "ReadReq",
                "ReadSharedReq",
                "ReadExReq",
                "ReadCleanReq",
                "WriteReq",
                "WriteLineReq",
                "UpgradeReq",
                "SCUpgradeReq",
                "LoadLockedReq",
                "StoreCondReq",
                "SwapReq",
                "InvalidateReq",
            ):
                if suffix.endswith((".hits::cpu.data", ".misses::cpu.data")):
                    legacy["cpu_data_accesses"] += float(number)
                if suffix.endswith(".hits::cpu.data"):
                    legacy["cpu_data_hits"] += float(number)
    assert len(found) == len(SOURCES) * len(
        COUNTERS
    ), f"Missing origin counters in {path}; rerun with instrumented binary"
    for source, d in values.items():
        checks = [
            (d["lookups"], d["accesses"] + d["arb_retries"], "lookups"),
            (d["accesses"], d["hits"] + d["misses"], "accesses"),
        ]
        if source in SOURCES[:3]:
            checks.append(
                (
                    d["misses"],
                    d["mshr_hits"] + d["mshr_misses"] + d["write_buffer_hits"],
                    "miss partition",
                )
            )
        for actual, expected, check in checks:
            assert actual == expected, (path, source, check, actual, expected)
        assert d["no_pc"] <= d["accesses"], (path, source, "no_pc")
        assert d["hits_on_prefetched_blocks"] <= d["hits"], (path, source)
    pf = values["l2Prefetch"]
    for field, name in [
        ("accesses", "pfIssued"),
        ("hits", "pfHitInCache"),
        ("mshr_hits", "pfHitInMSHR"),
        ("write_buffer_hits", "pfHitInWB"),
    ]:
        if name in legacy:
            assert pf[field] == legacy[name], (
                path,
                field,
                pf[field],
                legacy[name],
            )
    return {
        "point": point,
        "stats_path": str(path),
        "sources": values,
        "legacy_cross_checks": dict(legacy),
        "l2_prefetch_received": received,
    }


def with_rates(d):
    return {
        **d,
        "hit_rate_pct": 100 * d["hits"] / d["accesses"]
        if d["accesses"]
        else None,
        "tag_hit_rate_pct": 100 * d["tag_hits"] / d["accesses"]
        if d["accesses"]
        else None,
    }


def write_csv(path, rows):
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def summarize(input_path, out_dir):
    out_dir.mkdir(parents=True, exist_ok=True)
    if input_path.is_file():
        point = (
            input_path.parent.parent.name
            if input_path.parent.name == "m5out"
            else input_path.parent.name
        )
        files = [(point, input_path)]
        archive = None
    else:
        archive = input_path if (input_path / "spec_all").is_dir() else None
        point_dir = input_path / "spec_all" if archive else input_path
        if (point_dir / "stats.txt").is_file():
            point = (
                point_dir.parent.name
                if point_dir.name == "m5out"
                else point_dir.name
            )
            files = [(point, point_dir / "stats.txt")]
        else:
            files = [
                (p.name, p / "m5out/stats.txt")
                for p in sorted(point_dir.iterdir())
                if p.is_dir() and (p / "m5out/stats.txt").is_file()
            ]
    assert files, f"No stats files under {input_path}"
    if archive and (archive / "checkpoints.lst").is_file():
        expected = {
            line.split()[0]
            for line in (archive / "checkpoints.lst").read_text().splitlines()
            if line.strip() and not line.lstrip().startswith("#")
        }
        actual = {point for point, _ in files}
        assert actual == expected, (
            "Incomplete archive points",
            sorted(expected - actual),
            sorted(actual - expected),
        )
    points = [parse_point(path, point) for point, path in files]
    raw = {source: defaultdict(float) for source in SOURCES}
    point_csv = []
    for row in points:
        for source, d in row["sources"].items():
            point_csv.append(
                {"point": row["point"], "source": source, **with_rates(d)}
            )
            for key, value in d.items():
                raw[source][key] += value
    result = {
        "input": str(input_path),
        "measured_points": len(points),
        "raw_slice_totals": {s: with_rates(dict(d)) for s, d in raw.items()},
        "l2_prefetch_received": sum(r["l2_prefetch_received"] for r in points),
        "points": points,
    }
    if archive:
        cluster = json.loads((archive / "cluster.json").read_text())
        benchmarks = defaultdict(list)
        for row in points:
            workload, number = row["point"].rsplit("_", 1)
            row["workload"], row["pointnum"] = workload, number
            benchmarks[workload.split("_")[0]].append(row)
        weighted = []
        for bmk, rows in sorted(benchmarks.items()):
            workloads = {r["workload"] for r in rows}
            total_insts = sum(int(cluster[w]["insts"]) for w in workloads)
            agg = {source: defaultdict(float) for source in SOURCES}
            for workload in workloads:
                wr = [r for r in rows if r["workload"] == workload]
                expected = set(cluster[workload]["points"])
                actual = {r["pointnum"] for r in wr}
                assert actual == expected, (
                    workload,
                    "incomplete points",
                    expected - actual,
                )
                coverage = sum(
                    float(cluster[workload]["points"][r["pointnum"]])
                    for r in wr
                )
                for r in wr:
                    weight = (
                        int(cluster[workload]["insts"])
                        / total_insts
                        * float(cluster[workload]["points"][r["pointnum"]])
                        / coverage
                    )
                    for source, d in r["sources"].items():
                        for key, value in d.items():
                            agg[source][key] += value * weight
            for source, d in agg.items():
                weighted.append(
                    {"benchmark": bmk, "source": source, **with_rates(dict(d))}
                )
        result["weighted_benchmarks"] = weighted
        write_csv(out_dir / "weighted.csv", weighted)
    out_dir.mkdir(parents=True, exist_ok=True)
    write_csv(out_dir / "points.csv", point_csv)
    write_csv(
        out_dir / "raw-totals.csv",
        [
            {"source": source, **with_rates(dict(d))}
            for source, d in raw.items()
        ],
    )
    (out_dir / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
    print(
        f"Checked {len(points)} final measured intervals; all counter identities passed."
    )
    print(f'L2 prefetch queue received: {result["l2_prefetch_received"]:,.0f}')
    print("source        accesses        hits      misses   hit rate")
    for source in SOURCES:
        d = with_rates(dict(raw[source]))
        rate = (
            f'{d["hit_rate_pct"]:.2f}%'
            if d["hit_rate_pct"] is not None
            else "n/a"
        )
        print(
            f'{source:12s} {d["accesses"]:11,.0f} {d["hits"]:11,.0f} '
            f'{d["misses"]:11,.0f} {rate:>9s}'
        )
    print(
        f"Raw totals sum the measured slices; benchmark weighted counts are separate: {out_dir}"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "input",
        type=Path,
        help="stats.txt, m5out directory, or full CI archive",
    )
    parser.add_argument("--out-dir", type=Path, required=True)
    args = parser.parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)
    summarize(args.input.resolve(), args.out_dir.resolve())


if __name__ == "__main__":
    main()
