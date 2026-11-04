"""Summarize measured DIP contexts without interpreting them as recoverable CPI."""

import argparse
import collections
import csv
import hashlib
import json
from pathlib import Path

from analyze_ci_profile import audited_folders, stats

parser = argparse.ArgumentParser()
parser.add_argument("archive", type=Path)
parser.add_argument("--raw-audit", type=Path, required=True)
parser.add_argument("--analysis", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
audit = json.loads(args.raw_audit.read_text())
assert audit["passed"] and Path(audit["archive"]) == args.archive
profile = json.loads((args.archive / "cluster.json").read_text())
proof = json.loads((args.analysis / "audit.json").read_text())
assert (
    proof["passed"]
    and proof["raw_audit_sha256"]
    == hashlib.sha256(args.raw_audit.read_bytes()).hexdigest()
    and proof["cluster_sha256"]
    == hashlib.sha256((args.archive / "cluster.json").read_bytes()).hexdigest()
)
width = proof["configuration"]["width"]
folders = audited_folders(audit)
with (args.analysis / "benchmarks.csv").open() as stream:
    reference = {row["benchmark"]: row for row in csv.DictReader(stream)}
weighted = collections.defaultdict(collections.Counter)
full_instructions = collections.Counter()
flush_available = set()
for workload, values in profile.items():
    bench = workload.split("_")[0]
    insts = int(values["insts"])
    full_instructions[bench] += insts
    mass = sum(float(weight) for weight in values["points"].values())
    for point, weight in values["points"].items():
        folder = folders[workload + "_" + point]
        counters = stats(folder / "stats.txt")[-1]
        retired = counters["system.cpu.committedInsts"]
        scale = insts * float(weight) / mass / width / retired
        iq_slots = 0
        with (folder / "system.cpu.dip.csv").open() as stream:
            reader = csv.DictReader(stream)
            flush_available.add("flush_reason" in reader.fieldnames)
            for row in reader:
                if row["resolution"] == "pending":
                    continue
                slots = int(row["slot_weight"])
                if row["state"] == "Stall" and int(row["cause"]) & 2:
                    iq_slots += slots
                    weighted[bench]["iq", int(row["iq"]), int(row["iq_flags"])] += (
                        scale * slots
                    )
                if row["state"] == "FrontEnd":
                    weighted[bench]["frontend", int(row["frontend_reason"])] += (
                        scale * slots
                    )
                if row["state"] == "Misspeculation" and "flush_reason" in row:
                    weighted[bench]["flush", int(row["flush_reason"])] += scale * slots
        assert iq_slots == counters["system.cpu.dip.slotsByCause::S_ISB"]
        for name in ["decodeStrandedSlots", "readyBranchWaits"]:
            key = "system.cpu.dip." + name
            if key in counters:
                weighted[bench]["aux", name] += scale * width * counters[key]
assert len(flush_available) == 1
rows = []
for bench, counts in sorted(weighted.items()):
    norm = full_instructions[bench]
    queues = [
        dict(iq=key[1], flags=key[2], CPI=value / norm)
        for key, value in counts.items()
        if key[0] == "iq"
    ]
    queues.sort(key=lambda row: -row["CPI"])
    isb = sum(row["CPI"] for row in queues)
    assert abs(isb - float(reference[bench]["S_ISB"])) < 1e-12
    peer_ready = sum(row["CPI"] for row in queues if row["flags"] & 16)
    entry_full_peer = sum(
        row["CPI"] for row in queues if row["flags"] & 16 and row["flags"] & 2
    )
    front = {
        str(key[1]): value / norm
        for key, value in counts.items()
        if key[0] == "frontend"
    }
    assert abs(sum(front.values()) - float(reference[bench]["FrontEnd"])) < 1e-12
    row = dict(
        benchmark=bench,
        S_ISB_CPI=isb,
        selected_gate_with_ready_peer_CPI=peer_ready,
        selected_entries_full_with_ready_peer_CPI=entry_full_peer,
        iq_gate_partitions=queues,
        frontend_propagated_reason_CPI=front,
        auxiliary_counts_per_retired={
            key[1]: value / norm for key, value in counts.items() if key[0] == "aux"
        },
    )
    if True in flush_available:
        row["flush_reason_CPI"] = {
            str(key[1]): value / norm
            for key, value in counts.items()
            if key[0] == "flush"
        }
        assert (
            abs(
                sum(row["flush_reason_CPI"].values())
                - float(reference[bench]["Misspeculation"])
            )
            < 1e-12
        )
    rows.append(row)
result = dict(
    passed=True,
    rows=rows,
    profile_points=audit["points"],
    workloads=len(profile),
    flush_reason_available=True in flush_available,
    method=(
        "Normalize weights within each input; scale by full input instructions; "
        "divide by benchmark full instructions. Only finalized slots."
    ),
    limitation=(
        "IQ flags partition observed S_ISB marginal, not extra budget or guaranteed "
        "recoverable CPI. Split-store ready peer applies to the checked half. "
        "Frontend reasons are propagated context, not precise origins. Auxiliary "
        "counts are separate stage observations."
    ),
    raw_audit_sha256=hashlib.sha256(args.raw_audit.read_bytes()).hexdigest(),
    cluster_sha256=hashlib.sha256(
        (args.archive / "cluster.json").read_bytes()
    ).hexdigest(),
)
args.output.write_text(json.dumps(result, indent=2))
print(
    json.dumps(
        dict(
            passed=True,
            top_ready_peer_context=sorted(
                [
                    (
                        r["benchmark"],
                        r["selected_entries_full_with_ready_peer_CPI"],
                        r["S_ISB_CPI"],
                    )
                    for r in rows
                ],
                key=lambda row: -row[1],
            )[:8],
        ),
        indent=2,
    )
)
