#!/usr/bin/env python3
"""Compare complete, paired TEA CI archives for one checkpoint profile."""

import argparse
import configparser
import csv
import json
import math
from collections import Counter, defaultdict
from pathlib import Path


BEGIN = "---------- Begin Simulation Statistics ----------"
END = "---------- End Simulation Statistics"
STATES = ("Computing", "Stalled", "Flushed", "Drained")
EVENTS = (
    "DR_L1", "DR_TLB", "DR_SQ", "FL_MB", "FL_EX", "FL_MO", "ST_L1", "ST_TLB"
)
UNRESOLVED = {-1: "cancelled", -2: "pending", -3: "unknown"}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def close(actual, expected, label):
    require(
        math.isclose(actual, expected, rel_tol=1e-8, abs_tol=1e-3),
        f"{label}: {actual} != {expected}",
    )


def read_config(path):
    ini = configparser.ConfigParser(interpolation=None)
    ini.optionxform = str
    ini.read_string(path.read_text())
    values = {}
    for section in ini.sections():
        for key, value in ini[section].items():
            if key == "configFile" and "/ext/dramsim3/" in value:
                value = "ext/dramsim3/" + value.split("/ext/dramsim3/", 1)[1]
            values[f"{section}.{key}"] = value
    return values


def read_point(directory, offload):
    label = str(directory)
    require((directory / "completed").is_file(), f"{label}: incomplete run")
    require(
        not any((directory / m).exists() for m in ("abort", "running")),
        f"{label}: abort/running marker",
    )
    log = (directory / "log.txt").read_text()
    require(
        "exit_status: 0" in log
        and "because a thread reached the max instruction count" in log,
        f"{label}: missing successful max-instruction exit",
    )
    cfg = read_config(directory / "m5out/config.ini")
    require(cfg["system.cpu.enableTea"] == "true", f"{label}: TEA disabled")
    require(cfg["system.cpu.teaSamplePeriod"] == "1", f"{label}: period != 1")
    require(
        cfg["system.cpu.enable_difftest"] == "true",
        f"{label}: difftest disabled"
    )
    flags = [k for k in cfg if k.endswith(".offload_low_accuracy")]
    require(bool(flags), f"{label}: missing offload configuration")
    for key in flags:
        require(cfg.pop(key) == str(offload).lower(), f"{label}: wrong {key}")
        prefix = key.rsplit(".", 1)[0]
        for name, value in (
            ("offload_stream_only", "true"),
            ("cdp_ratio_threshold", "0.2"),
            ("accuracy_threshold", "0.8"),
        ):
            require(cfg[f"{prefix}.{name}"] == value, f"{label}: wrong {name}")

    text = (directory / "m5out/stats.txt").read_text()
    blocks = text.split(BEGIN)[1:]
    require(
        len(blocks) == 2 and all(END in b for b in blocks),
        f"{label}: expected two complete statistics windows",
    )
    stats = {}
    for line in blocks[-1].splitlines():
        fields = line.split()
        if len(fields) > 1 and fields[0].startswith(
            ("system.cpu.", "simInsts")
        ):
            try:
                stats[fields[0]] = float(fields[1])
            except ValueError:
                continue
    insts = stats["system.cpu.committedInsts"]
    samples = stats["system.cpu.tea.samples"]
    cycles = stats["system.cpu.numCycles"]
    require(insts > 0 and cycles > 0, f"{label}: empty measurement window")
    close(samples, cycles, f"{label}: samples/cycles")
    close(
        stats["system.cpu.tea.committedByThread"], insts, f"{label}: commits"
    )
    metrics = {"cycles": cycles, "data_memory_union": 0.0}
    for state in STATES:
        metrics[state] = stats[f"system.cpu.tea.samplesByState::{state}"]
    close(sum(metrics[s] for s in STATES), cycles, f"{label}: states")
    pcs = Counter()
    psvs = Counter()
    events = Counter()
    total = 0.0
    with (directory / "m5out/system.cpu.tea.csv").open() as stream:
        reader = csv.DictReader(stream)
        require(
            reader.fieldnames == ["thread", "pc", "psv", "sample_weight"],
            f"{label}: unexpected TEA CSV columns",
        )
        for row in reader:
            psv = int(row["psv"])
            weight = float(row["sample_weight"])
            require(
                row["thread"] == "0"
                and math.isfinite(weight) and weight >= 0
                and (-3 <= psv <= 255),
                f"{label}: invalid TEA row",
            )
            total += weight
            pcs[row["pc"], psv] += weight / insts
            psvs[psv] += weight
            if psv >= 0:
                for bit, event in enumerate(EVENTS):
                    if psv & (1 << bit):
                        events[event] += weight
                if psv & 192:
                    metrics["data_memory_union"] = (
                        metrics.get("data_memory_union", 0) + weight
                    )
    close(total, samples, f"{label}: CSV samples")
    close(
        sum(w for p, w in psvs.items() if p >= 0),
        stats["system.cpu.tea.attributedSamples"], f"{label}: attributed",
    )
    for event in EVENTS:
        close(
            events[event], stats[f"system.cpu.tea.samplesByEvent::{event}"],
            f"{label}: {event}",
        )
        metrics[event] = events[event]
    for psv, name in UNRESOLVED.items():
        stat = {
            -1: "cancelledTargets",
            -2: "pendingSamples",
            -3: "unresolvedTargets",
        }[psv]
        close(psvs[psv], stats[f"system.cpu.tea.{stat}"], f"{label}: {name}")
        metrics[name] = psvs[psv]
    return {k: v / insts for k, v in metrics.items()}, pcs, cfg, insts


def write_csv(path, columns, rows):
    with path.open("w") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)


def compare(baseline, offload, output):
    cluster = json.loads((baseline / "cluster.json").read_text())
    require(
        cluster == json.loads((offload / "cluster.json").read_text()),
        "cluster/weights differ between archives",
    )
    require(
        (baseline / "checkpoints.lst").read_bytes()
        == (offload / "checkpoints.lst").read_bytes(),
        "checkpoint lists differ between archives",
    )
    benchmark_insts = Counter()
    for workload, body in cluster.items():
        benchmark_insts[workload.split("_", 1)[0]] += int(body["insts"])
    point_rows = []
    aggregates = defaultdict(lambda: [Counter(), Counter()])
    pc_aggregates = defaultdict(lambda: [Counter(), Counter()])
    for workload, body in sorted(cluster.items()):
        benchmark = workload.split("_", 1)[0]
        coverage = sum(float(w) for w in body["points"].values())
        require(coverage > 0, f"{workload}: zero profile weight")
        for point, weight in body["points"].items():
            name = f"{workload}_{point}"
            runs = [
                read_point(root / "spec_all" / name, bool(index))
                for index, root in enumerate((baseline, offload))
            ]
            require(runs[0][2] == runs[1][2], f"{name}: paired configs differ")
            q = float(weight) / coverage
            for index, (metrics, pcs, _, insts) in enumerate(runs):
                for metric, value in metrics.items():
                    aggregates[benchmark][index][metric] += (
                        q * int(body["insts"])
                        / benchmark_insts[benchmark] * value
                    )
                for key, value in pcs.items():
                    pc_aggregates[workload][index][key] += q * value
            for metric in sorted(runs[0][0].keys() | runs[1][0].keys()):
                a, b = (run[0].get(metric, 0.0) for run in runs)
                point_rows.append({
                    "workload": workload, "point": point, "metric": metric,
                    "baseline_cpi": a, "offload_cpi": b, "saved_cpi": a - b,
                })
            print(f"Validated {name}", flush=True)

    benchmark_rows = []
    for benchmark, (a, b) in sorted(aggregates.items()):
        saved = a["cycles"] - b["cycles"]
        for metric in sorted(a.keys() | b.keys()):
            benchmark_rows.append({
                "benchmark": benchmark, "metric": metric,
                "baseline_cpi": a[metric], "offload_cpi": b[metric],
                "saved_cpi": a[metric] - b[metric],
                "speedup_pct": 100 * (a["cycles"] / b["cycles"] - 1),
            })
        close(
            sum(a[s] - b[s] for s in STATES), saved,
            f"{benchmark}: additive state savings",
        )
    pc_rows = []
    for workload, (a, b) in sorted(pc_aggregates.items()):
        for pc, psv in sorted(a.keys() | b.keys()):
            pc_rows.append({
                "workload": workload, "pc": pc, "psv": psv,
                "events": UNRESOLVED[psv] if psv < 0 else "|".join(
                    e for bit, e in enumerate(EVENTS) if psv & (1 << bit)
                ) or "none",
                "baseline_cpi": a[pc, psv], "offload_cpi": b[pc, psv],
                "saved_cpi": a[pc, psv] - b[pc, psv],
            })
    output.mkdir(parents=True, exist_ok=True)
    columns = ["baseline_cpi", "offload_cpi", "saved_cpi"]
    write_csv(
        output / "points.csv",
        ["workload", "point", "metric"] + columns, point_rows,
    )
    write_csv(
        output / "benchmarks.csv",
        ["benchmark", "metric"] + columns + ["speedup_pct"], benchmark_rows,
    )
    write_csv(
        output / "pc-psv.csv",
        ["workload", "pc", "psv", "events"] + columns,
        sorted(pc_rows, key=lambda r: -r["saved_cpi"]),
    )
    (output / "comparison.json").write_text(json.dumps({
        "baseline": str(baseline), "offload": str(offload),
        "points": sum(len(b["points"]) for b in cluster.values()),
        "benchmarks": len(benchmark_insts),
        "note": (
            "Positive saved_cpi means fewer cycles per instruction. "
            "States and exact PSV partitions are additive; event bits overlap. "
            "PC attribution includes computing cycles and does not establish "
            "independent causality."
        ),
    }, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("offload", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    compare(
        args.baseline.resolve(), args.offload.resolve(), args.output.resolve()
    )


if __name__ == "__main__":
    main()
