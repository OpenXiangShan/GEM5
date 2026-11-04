"""Audit full DIP/TEA SPEC06 outputs and aggregate measured cycle stacks.

Input weights and source metadata must come from the actual CI archive. Official
Actions success is checked separately; this script validates archived evidence.
"""

import argparse
import collections
import configparser
import csv
import hashlib
import json
import math
from pathlib import Path
import re

STATES = ["Base", "Stall", "FrontEnd", "Misspeculation"]
TEA_EVENTS = [
    "DR_L1",
    "DR_TLB",
    "DR_SQ",
    "FL_MB",
    "FL_EX",
    "FL_MO",
    "ST_L1",
    "ST_TLB",
    "ST_LLC",
]
CAUSES = [
    "S_ROB",
    "S_ISB",
    "S_LDQ",
    "S_STQ",
    "S_REN",
    "S_SER",
    "F_BUB",
    "F_CTM",
    "M_SWE",
    "M_FL",
    "M_UNKNOWN",
]


def stats(path):
    blocks = re.findall(
        r"Begin Simulation Statistics(.*?)End Simulation Statistics",
        path.read_text(),
        re.S,
    )
    rows = []
    for block in blocks:
        values = {}
        for line in block.splitlines():
            fields = line.split()
            if len(fields) > 1:
                try:
                    values[fields[0]] = float(fields[1])
                except ValueError:
                    pass
        rows.append(values)
    return rows


def table(path, rows):
    assert rows
    with path.open("w") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("archive", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--width", type=int, required=True)
    parser.add_argument("--receipts", type=Path, required=True)
    args = parser.parse_args()
    archive = args.archive
    output = args.output
    receipt_audit = json.loads(args.receipts.read_text())
    assert receipt_audit["passed"] and receipt_audit["points"] == len(
        receipt_audit["receipts"]
    )
    folders = {
        r["point"]: Path(
            next(p for p in r["hashes"] if p.endswith("/m5out/stats.txt"))
        ).parent.parent
        for r in receipt_audit["receipts"]
    }
    output.mkdir(exist_ok=False, parents=True)
    metadata = dict(
        line.split(": ", 1)
        for line in (archive / "metadata.txt").read_text().splitlines()
        if ": " in line
    )
    weightfile = archive / "cluster.json"
    profile = json.loads(weightfile.read_text())
    expected = {
        f"{workload}_{point}"
        for workload, data in profile.items()
        for point in data["points"]
    }
    listing = [
        line.split()
        for line in (archive / "checkpoints.lst").read_text().splitlines()
        if line.strip() and not line.startswith("#")
    ]
    assert {row[0] for row in listing} == expected and len(listing) == len(expected)
    assert all(row[2:] == ["0", "0", "20", "20"] for row in listing)
    actual = set(folders)
    assert actual == expected, dict(
        missing=sorted(expected - actual), unexpected=sorted(actual - expected)
    )
    points = []
    workloads = []
    pcrows = []
    tea_pcrows = []
    flushrows = []
    for workload, data in profile.items():
        insts = float(data["insts"])
        weights = {p: float(w) for p, w in data["points"].items()}
        mass = sum(weights.values())
        aggregate = collections.defaultdict(float)
        pcs = collections.defaultdict(float)
        flush_pcs = collections.defaultdict(float)
        tea_pcs = collections.defaultdict(lambda: collections.defaultdict(float))
        for point, weight in weights.items():
            weight /= mass
            folder = folders[f"{workload}_{point}"]
            out = folder / "m5out"
            # Correctness and native/recovered completion are established by the
            # passed raw audit. Its exact per-point receipt chooses this folder;
            # recovered runs have different log and completion marker layouts.
            cfg = configparser.ConfigParser(interpolation=None)
            cfg.optionxform = str
            cfg.read(out / "config.ini")
            cpu = cfg["system.cpu"]
            assert cpu.getboolean("enableDip") and cpu.getboolean("enableTea")
            assert (
                cpu.getint("renameWidth") == args.width
                and cpu.getint("numThreads") == 1
            )
            assert cfg[cpu["clk_domain"]].getint("clock") == 333
            assert cpu["difftest_ref_so"] == metadata["ref_so"]
            assert cpu["RobCompressPolicy"] == "none" and not cpu.getboolean(
                "enableDispatchStage"
            )
            assert not cpu.getboolean("enable_loadFusion")
            assert (
                cpu.getint("dipSamplePeriod") == 1
                and cpu.getint("teaSamplePeriod") == 1
            )
            assert cpu.getboolean("enable_difftest") and cpu.getboolean(
                "enable_mem_dedup"
            )
            assert (
                cpu.getint("max_insts_any_thread") == 40000000
                and cpu.getint("warmupInstCount") == 20000000
            )
            blocks = stats(out / "stats.txt")
            assert len(blocks) == 2
            s = blocks[-1]
            retired = s["system.cpu.committedInsts"]
            cycles = s["system.cpu.numCycles"]
            assert (
                abs(retired - 20000000) < 16
                and abs(blocks[0]["system.cpu.committedInsts"] - 20000000) < 16
            )
            width = cpu.getint("renameWidth")
            n = lambda k: int(s["system.cpu.dip." + k])
            assert n("samples") == cycles and n("sampledSlots") == cycles * width
            totals = collections.Counter()
            states = collections.Counter()
            causes = collections.Counter()
            with (out / "system.cpu.dip.csv").open() as f:
                for row in csv.DictReader(f):
                    assert row["thread"] == "0"
                    slots = int(row["slot_weight"])
                    state = row["state"]
                    cause = int(row["cause"])
                    assert (
                        slots > 0
                        and state in STATES
                        and math.isclose(
                            float(row["cycle_weight"]), slots / width, rel_tol=1e-14
                        )
                    )
                    resolution = row["resolution"]
                    assert resolution in ["resolved", "unresolved", "pending"]
                    allowed = {
                        "Base": 0,
                        "Stall": 63,
                        "FrontEnd": 192,
                        "Misspeculation": 1792,
                    }[state]
                    assert cause & ~allowed == 0
                    totals[resolution] += slots
                    if "flush_reason" in row and state == "Misspeculation":
                        reason = int(row["flush_reason"])
                        assert 0 <= reason <= 7
                        flush_pcs[row["pc"], resolution, reason] += (
                            weight * slots / width / retired
                        )
                    if resolution == "pending":
                        continue
                    assert bool(row["pc"]) == (resolution == "resolved")
                    states[state] += slots
                    for i, name in enumerate(CAUSES):
                        if cause & (1 << i):
                            causes[name] += slots
                    if resolution == "resolved":
                        pcs[row["pc"], state, cause] += weight * slots / width / retired
            assert totals["resolved"] == n("attributedSlots")
            assert totals["unresolved"] == n("unresolvedSlots") and totals[
                "pending"
            ] == n("pendingSlots")
            assert sum(totals.values()) == n("sampledSlots")
            for state in STATES:
                assert states[state] == n("slotsByState::" + state)
            for name in CAUSES:
                assert causes[name] == n("slotsByCause::" + name)
            tea = {
                state: s["system.cpu.tea.samplesByState::" + state]
                for state in ["Computing", "Stalled", "Flushed", "Drained"]
            }
            assert sum(tea.values()) == s["system.cpu.tea.samples"] == cycles
            assert s["system.cpu.tea.committedByThread"] == retired
            tea_totals = collections.Counter()
            tea_events = collections.Counter()
            memory_union = 0
            with (out / "system.cpu.tea.csv").open() as f:
                for row in csv.DictReader(f):
                    assert row["thread"] == "0"
                    psv = int(row["psv"])
                    value = float(row["sample_weight"])
                    assert math.isfinite(value) and value >= 0
                    tea_totals["total"] += value
                    if psv < 0:
                        assert psv in [-1, -2, -3]
                        tea_totals[psv] += value
                    else:
                        assert psv < (1 << len(TEA_EVENTS))
                        assert row["pc"]
                        tea_totals["attributed"] += value
                        tea_pcs[row["pc"]]["total"] += weight * value / retired
                        if psv & ((1 << 6) | (1 << 7) | (1 << 8)):
                            tea_pcs[row["pc"]]["memory_union"] += (
                                weight * value / retired
                            )
                            memory_union += value
                        if psv & (1 << 3):
                            tea_pcs[row["pc"]]["branch_event"] += (
                                weight * value / retired
                            )
                        for i, event in enumerate(TEA_EVENTS):
                            if psv & (1 << i):
                                tea_events[event] += value
            expected_tea = {
                "total": "samples",
                "attributed": "attributedSamples",
                -1: "cancelledTargets",
                -2: "pendingSamples",
                -3: "unresolvedTargets",
            }
            for key, field in expected_tea.items():
                assert math.isclose(
                    tea_totals[key],
                    s["system.cpu.tea." + field],
                    rel_tol=1e-9,
                    abs_tol=1e-3,
                )
            for event in TEA_EVENTS:
                assert math.isclose(
                    tea_events[event],
                    s["system.cpu.tea.samplesByEvent::" + event],
                    rel_tol=1e-9,
                    abs_tol=1e-3,
                )
            row = dict(
                workload=workload,
                point=point,
                weight=weight,
                cycles=int(cycles),
                instructions=int(retired),
                CPI=cycles / retired,
                **{state: states[state] / width / retired for state in STATES},
                pending=totals["pending"] / width / retired,
                unresolved=totals["unresolved"] / width / retired,
                **{name: causes[name] / width / retired for name in CAUSES},
                **{"tea_" + state: value / retired for state, value in tea.items()},
                tea_memory_union=memory_union / retired,
                branch_MPKI=s["system.cpu.commit.branchMispredicts"] * 1000 / retired,
                dcache_MPKI=s.get(
                    "system.cpu.dcache.overallMisses::total", float("nan")
                )
                * 1000
                / retired,
                L3_MPKI=s.get("system.l3.overallMisses::total", float("nan"))
                * 1000
                / retired,
            )
            assert math.isclose(
                sum(row[state] for state in STATES) + row["pending"],
                row["CPI"],
                rel_tol=1e-12,
            )
            points.append(row)
            for name, value in row.items():
                if name not in [
                    "workload",
                    "point",
                    "weight",
                    "cycles",
                    "instructions",
                ]:
                    aggregate[name] += weight * value
        workloads.append(
            dict(
                workload=workload,
                benchmark=workload.split("_")[0],
                full_instructions=insts,
                weight_sum=mass,
                estimated_cycles=aggregate["CPI"] * insts,
                **aggregate,
            )
        )
        print(
            json.dumps(dict(workload=workload, done=len(points), total=len(expected))),
            flush=True,
        )
        for (pc, state, cause), cpi in pcs.items():
            pcrows.append(
                dict(
                    workload=workload, pc=pc, state=state, cause=cause, weighted_CPI=cpi
                )
            )
        for (pc, resolution, reason), cpi in flush_pcs.items():
            flushrows.append(
                dict(
                    workload=workload,
                    pc=pc,
                    resolution=resolution,
                    flush_reason=reason,
                    weighted_CPI=cpi,
                )
            )
        for pc, counts in tea_pcs.items():
            tea_pcrows.append(
                dict(
                    workload=workload,
                    pc=pc,
                    weighted_CPI=counts["total"],
                    memory_event_CPI=counts["memory_union"],
                    branch_event_CPI=counts["branch_event"],
                )
            )
    benchmarks = []
    for bench in sorted({r["benchmark"] for r in workloads}):
        rows = [r for r in workloads if r["benchmark"] == bench]
        instructions = sum(r["full_instructions"] for r in rows)
        aggregate = {
            name: sum(r[name] * r["full_instructions"] for r in rows) / instructions
            for name in [
                "CPI",
                *STATES,
                "pending",
                "unresolved",
                *CAUSES,
                "tea_Computing",
                "tea_Stalled",
                "tea_Flushed",
                "tea_Drained",
                "tea_memory_union",
                "branch_MPKI",
                "dcache_MPKI",
                "L3_MPKI",
            ]
        }
        benchmarks.append(
            dict(
                benchmark=bench,
                inputs=len(rows),
                estimated_cycles=aggregate["CPI"] * instructions,
                **aggregate,
            )
        )
    table(output / "points.csv", points)
    table(output / "workloads.csv", workloads)
    table(output / "benchmarks.csv", benchmarks)
    table(
        output / "pcs.csv",
        sorted(pcrows, key=lambda r: (r["workload"], -r["weighted_CPI"])),
    )
    table(
        output / "tea-pcs.csv",
        sorted(tea_pcrows, key=lambda r: (r["workload"], -r["weighted_CPI"])),
    )
    if flushrows:
        table(output / "flush-reasons.csv", flushrows)
    proof = dict(
        passed=True,
        archive=str(archive),
        points=len(points),
        workloads=len(workloads),
        benchmarks=len(benchmarks),
        cluster_sha256=hashlib.sha256(weightfile.read_bytes()).hexdigest(),
        configuration=dict(
            width=args.width,
            threads=1,
            clock_ps=333,
            ROB="none",
            dispatch_queue=False,
            load_fusion=False,
            ref_so=metadata["ref_so"],
        ),
        benchmark_aggregation=(
            "Per-input SimPoint CPI, weighted by full input instruction counts for "
            "aggregate benchmark CPI; this is not the official SPEC score formula."
        ),
        official_success_verified=False,
        raw_audit=str(args.receipts),
        raw_audit_sha256=hashlib.sha256(args.receipts.read_bytes()).hexdigest(),
    )
    (output / "audit.json").write_text(json.dumps(proof, indent=2))
    print(json.dumps(proof))


if __name__ == "__main__":
    main()
