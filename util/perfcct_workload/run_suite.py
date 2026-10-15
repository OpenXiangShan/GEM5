#!/usr/bin/env python3
"""Run paired SE probes and save commands, trace evidence and comparisons."""

import argparse
from collections import Counter
import hashlib
import json
import subprocess
import sqlite3
import time
from pathlib import Path

from analyze import analyze
from check_trace import check_connection


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
CASES = {
    "pointer-cold-30": ("pointer-cold", "30ns", "none"),
    "pointer-cold-90": ("pointer-cold", "90ns", "none"),
    "pointer-warm-30": ("pointer-warm", "30ns", "none"),
    "pointer-warm-90": ("pointer-warm", "90ns", "none"),
    "stlf-short": ("stlf-short", "30ns", "none"),
    "stlf-long": ("stlf-long", "30ns", "none"),
    "group-none": ("group", "30ns", "none"),
    "group-kmhv3": ("group", "30ns", "kmhv3"),
}

RESOURCE_CASES = {
    "lines-mshr2": ("resource-lines", 2, 20),
    "lines-mshr16": ("resource-lines", 16, 20),
    "lines-mshr64": ("resource-lines", 64, 20),
    "lines-mshr256": ("resource-lines", 256, 20),
    "targets-limit2": ("resource-targets", 16, 2),
    "targets-limit20": ("resource-targets", 16, 20),
}

SCHEDULING_CASES = {
    "scheduling-serial": ("scheduling-serial", "30ns", "none"),
    "scheduling-independent": ("scheduling-independent", "30ns", "none"),
}
SUITES = {
    "paired": CASES,
    "resources": RESOURCE_CASES,
    "scheduling": SCHEDULING_CASES,
}


def scheduling_evidence(path, roi):
    with sqlite3.connect(path.resolve().as_uri() + "?mode=ro", uri=True) as db:
        db.row_factory = sqlite3.Row
        meta = dict(db.execute("SELECT Key,Value FROM PerfCCTMeta"))
        if not meta.get("scheduler_scope"):
            raise RuntimeError(
                "Scheduling suite requires scheduler_scope metadata"
            )
        rows = [
            dict(row)
            for row in db.execute(
                "SELECT * FROM PerfCCTEvent WHERE Cpu=? AND TID=? "
                "AND Tick>=? AND Tick<? AND SeqNum>=? AND SeqNum<? "
                "AND (Event GLOB 'iq_*' OR Event IN "
                "('dependency','value_writeback','operand_writeback_observed')) "
                "ORDER BY Tick,ID",
                (
                    roi["cpu"],
                    roi["tid"],
                    roi["start_tick"],
                    roi["end_tick"],
                    roi["seq_begin_inclusive"],
                    roi["seq_end_exclusive"],
                ),
            )
        ]
    counts = Counter(row["Event"] for row in rows)
    if not any(event.startswith("iq_") for event in counts):
        raise RuntimeError("Scheduling suite recorded no IQ events in the ROI")
    examples = {}
    for row in rows:
        entries = examples.setdefault(row["Event"], [])
        if len(entries) < 3:
            entries.append(row)
    return {
        "scheduler_scope": meta["scheduler_scope"],
        "event_counts": dict(counts),
        "event_examples": examples,
        "examples_limit_per_event": 3,
        "examples_truncated": {
            event: count > len(examples[event])
            for event, count in counts.items()
        },
        "examples_omitted": {
            event: count - len(examples[event])
            for event, count in counts.items()
        },
    }


def scheduling_comparisons(evidence):
    return {
        "cases": {
            name: {
                "roi_cycles": item["roi"]["marker_commit_to_commit_cycles"],
                "roi_committed_operations": item["roi"][
                    "committed_operations"
                ],
                "whole_run_stats": item["whole_run_stats"],
                "scheduling": item["scheduling"],
            }
            for name, item in evidence.items()
        },
        "notes": [
            "Both variants execute 128 DIVs and 128 restoring XORs; source dependencies differ.",
            "Every XOR restores the original dividend, so arithmetic operands match between variants.",
            "Each binary warms the shared kernel once before the ROI.",
            "Independent operations may contend for ports; actual event coverage must establish this.",
            "Ready or selection observations are not completed values; summed waits are not ROI cost.",
        ],
    }


def read_stats(path):
    values = {}
    for line in path.read_text().splitlines():
        fields = line.split("#", 1)[0].split()
        if len(fields) < 2 or fields[0].startswith(("----------", "host")):
            continue
        if fields[0] in values:
            raise ValueError(f"Multiple statistics dumps: {path}")
        values[fields[0]] = fields[1]
    return values


def cache_evidence(path, roi):
    """Keep resource state separate from observed rejected admissions."""
    with sqlite3.connect(path.resolve().as_uri() + "?mode=ro", uri=True) as db:
        db.row_factory = sqlite3.Row
        if not db.execute(
            "SELECT 1 FROM sqlite_master WHERE name='PerfCCTCacheEvent'"
        ).fetchone():
            return {"available": False}
        rows = [
            dict(row)
            for row in db.execute(
                "SELECT * FROM PerfCCTCacheEvent ORDER BY Tick,ID"
            )
        ]
    owners, target_owners = {}, {}
    for row in rows:
        if row["Event"] in ("owner", "owner_credit"):
            owners.setdefault(row["ParentID"], []).append(row)
        elif row["Event"] == "target_owner":
            target_owners.setdefault(row["ParentID"], []).append(row)

    def summarize(selected):
        counts = Counter((r["Cache"], r["Event"]) for r in selected)
        rejected = [r for r in selected if r["Event"] == "reject"]
        reasons = Counter(
            (r["Cache"], r["Detail"], r["BlockedMask"]) for r in rejected
        )
        return {
            "event_counts": [
                {"cache": k[0], "event": k[1], "count": v}
                for k, v in sorted(counts.items())
            ],
            "reject_by_detail_and_mask": [
                {
                    "cache": k[0],
                    "detail": k[1],
                    "blocked_mask": k[2],
                    "count": v,
                }
                for k, v in sorted(reasons.items())
            ],
            "reject_count": len(rejected),
            "observed_request_objects": len(
                {r.get("RequestID") for r in selected if r.get("RequestID")}
            ),
            "observed_target_lifetimes": len(
                {
                    (r["Cache"], r.get("TargetID"))
                    for r in selected
                    if r.get("TargetID")
                }
            ),
            "target_owner_rows": sum(
                len(target_owners.get(r["ID"], [])) for r in rejected
            ),
            "target_owner_examples": [
                {
                    "reject_id": r["ID"],
                    "targets": target_owners[r["ID"]][:16],
                    "targets_truncated": len(target_owners[r["ID"]]) > 16,
                }
                for r in [
                    item for item in rejected if target_owners.get(item["ID"])
                ][:4]
            ],
            "rejects_with_owner_snapshot": sum(
                bool(owners.get(r["ID"])) for r in rejected
            ),
            "owner_rows": sum(len(owners.get(r["ID"], [])) for r in rejected),
            "owner_mshr_lifetimes": len(
                {
                    (o["Cache"], o["MSHR"])
                    for r in rejected
                    for o in owners.get(r["ID"], [])
                }
            ),
            "reject_examples": [
                dict(reject=r, owners=owners.get(r["ID"], []))
                for r in rejected[:8]
            ],
            "reject_with_owner_examples": [
                dict(reject=r, owners=owners[r["ID"]])
                for r in [item for item in rejected if owners.get(item["ID"])][
                    :4
                ]
            ],
        }

    return {
        "available": True,
        "whole_run": summarize(rows),
        "roi_tick_window": summarize(
            [
                r
                for r in rows
                if roi["start_tick"] <= r["Tick"] < roi["end_tick"]
            ]
        ),
        "notes": [
            "Cache resource scope is the ROI tick window, not the instruction seq range; "
            "owners may be older or squashed.",
            "ParentID joins owner snapshots to reject.ID; (Cache,MSHR) identifies a resource lifetime.",
            "A full resource state without an observed reject is not evidence of lost cycles.",
            "Reject counts and owner snapshots are observations, not additive performance costs.",
            "Request/target counts cover observed events only, not all cache hits or complete request lifetimes.",
        ],
    }


def resource_comparisons(evidence):
    cases = {}
    for name, item in evidence.items():
        cases[name] = {
            "roi_cycles": item["roi"]["marker_commit_to_commit_cycles"],
            "whole_run_stats": item["whole_run_stats"],
            "roi_cache": item["cache_resources"].get("roi_tick_window"),
        }
    return {
        "cases": cases,
        "notes": [
            "Compare the same resource-lines binary across MSHR capacities and the same "
            "resource-targets binary across target capacities.",
            "ROI cycles and whole-run blockedCycles have different windows; do not subtract them.",
            "The 64/256 pair tests whether removing a full-state observation changes runtime; "
            "inspect actual rejects and owners.",
            "Capacity interventions can expose downstream limits; they do not identify "
            "recoverable cycles from a single trace.",
        ],
    }


def verify_untraced(name, out):
    source = out / name
    folder = out / "untraced" / name
    folder.mkdir(parents=True, exist_ok=True)
    original = json.loads((source / "command.json").read_text())
    command = [
        arg
        for arg in original["command"]
        if arg not in ("--enable-arch-db", "--arch-db-dump-causal")
        and not arg.startswith("--arch-db-file=")
    ]
    command[command.index("-d") + 1] = str(folder)
    with (folder / "run.log").open("w") as log:
        result = subprocess.run(
            command,
            cwd=ROOT,
            stdout=log,
            stderr=subprocess.STDOUT,
            timeout=180,
        )
    text = (folder / "run.log").read_text()
    passed = (
        result.returncode == 0
        and "exiting with last active thread context" in text
        and "Simulated exit code not 0" not in text
    )
    (folder / "command.json").write_text(
        json.dumps(
            {
                "command": command,
                "guest_pass": passed,
                "exit_code": result.returncode,
            },
            indent=2,
        )
        + "\n"
    )
    traced = read_stats(source / "stats.txt")
    untraced = (
        read_stats(folder / "stats.txt")
        if (folder / "stats.txt").exists()
        else {}
    )
    differences = {
        key: {"traced": traced.get(key), "untraced": untraced.get(key)}
        for key in sorted(traced.keys() | untraced.keys())
        if traced.get(key) != untraced.get(key)
    }
    return {
        "case": name,
        "guest_pass": passed,
        "traced_stats_count": len(traced),
        "untraced_stats_count": len(untraced),
        "differences": differences,
        "equivalent": passed and not differences,
    }


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def comparisons(evidence):
    """Report paired observations without turning waits into saved cycles."""
    if not set(CASES).issubset(evidence):
        return {"status": "partial suite; paired comparison omitted"}

    def cycles(name):
        return evidence[name]["roi"]["marker_commit_to_commit_cycles"]

    def cache_wait(name):
        return (
            evidence[name]["wait_summary"]
            .get("cache_refill", {})
            .get("observed_cycles", 0)
        )

    cold_delta = cycles("pointer-cold-90") - cycles("pointer-cold-30")
    wait_delta = cache_wait("pointer-cold-90") - cache_wait("pointer-cold-30")
    return {
        "pointer": {
            "cold_roi_delta_cycles": cold_delta,
            "cold_cache_wait_delta_cycles": wait_delta,
            "delta_residual_cycles": cold_delta - wait_delta,
            "warm_roi_delta_cycles": cycles("pointer-warm-90")
            - cycles("pointer-warm-30"),
            "interpretation": "Serialized cold loads are sensitive to memory latency; warm loads are the control.",
        },
        "stlf": {
            "short_roi_cycles": cycles("stlf-short"),
            "long_roi_cycles": cycles("stlf-long"),
            "short_waits": evidence["stlf-short"]["wait_summary"].get(
                "stlf", {}
            ),
            "long_waits": evidence["stlf-long"]["wait_summary"].get(
                "stlf", {}
            ),
            "interpretation": "Longer producer chains add work; concurrent STLF waits are not additive ROI cost.",
        },
        "group": {
            "none_roi_cycles": cycles("group-none"),
            "kmhv3_roi_cycles": cycles("group-kmhv3"),
            "none_nonhead": evidence["group-none"]["group_nonhead"],
            "kmhv3_nonhead": evidence["group-kmhv3"]["group_nonhead"],
            "interpretation": "This checks blocker identity coverage, not a performance improvement.",
        },
    }


def run_case(name, out, binary):
    resource = name in RESOURCE_CASES
    if resource:
        elf_name, mshrs, targets = RESOURCE_CASES[name]
        latency, policy = "90ns", "none"
    else:
        elf_name, latency, policy = (
            SCHEDULING_CASES if name in SCHEDULING_CASES else CASES
        )[name]
    elf = HERE / elf_name
    folder = out / name
    folder.mkdir(parents=True, exist_ok=True)
    command = [
        str(binary),
        "-d",
        str(folder),
        str(ROOT / "configs/example/se.py"),
        "-c",
        str(elf),
        "--mem-type=SimpleMemory",
        "--no-pf",
        "--no-l3cache",
        "--warmup-insts-no-switch=0",
        "--maxinsts=100000",
        "--param",
        f"system.mem_ctrls[0].latency='{latency}'",
        "--param",
        f"system.cpu[0].RobCompressPolicy='{policy}'",
        "--enable-arch-db",
        "--arch-db-file=" + str(folder / "trace.db"),
        "--arch-db-dump-causal",
    ]
    if resource:
        for key, value in (
            ("mshrs", mshrs),
            ("tgts_per_mshr", targets),
            ("demand_mshr_reserve", 0),
        ):
            command.extend(["--param", f"system.cpu[0].dcache.{key}={value}"])
    manifest = {
        "command": command,
        "elf_sha256": digest(elf),
        "parameters": {"memory_latency": latency, "rob_policy": policy},
    }
    if resource:
        manifest["parameters"].update(
            mshrs=mshrs, tgts_per_mshr=targets, demand_mshr_reserve=0
        )
    (folder / "command.json").write_text(json.dumps(manifest, indent=2) + "\n")
    start = time.monotonic()
    with (folder / "run.log").open("w") as log:
        result = subprocess.run(
            command,
            cwd=ROOT,
            stdout=log,
            stderr=subprocess.STDOUT,
            timeout=180,
        )
    log_text = (folder / "run.log").read_text()
    manifest.update(
        exit_code=result.returncode, wall_seconds=time.monotonic() - start
    )
    manifest["guest_pass"] = (
        result.returncode == 0
        and "exiting with last active thread context" in log_text
        and "Simulated exit code not 0" not in log_text
    )
    (folder / "command.json").write_text(json.dumps(manifest, indent=2) + "\n")
    if not manifest["guest_pass"]:
        raise RuntimeError(f"Guest did not pass: {folder / 'run.log'}")
    with sqlite3.connect(
        (folder / "trace.db").as_uri() + "?mode=ro", uri=True
    ) as db:
        integrity = check_connection(db)
    (folder / "integrity.json").write_text(
        json.dumps(integrity, indent=2) + "\n"
    )
    if not integrity["ok"]:
        raise RuntimeError(
            f"Trace integrity failed: {folder / 'integrity.json'}"
        )
    evidence = analyze(folder / "trace.db", elf)
    stats = read_stats(folder / "stats.txt")
    evidence["whole_run_stats"] = {
        key: value
        for key, value in stats.items()
        if key
        in ("simTicks", "simSeconds", "simInsts", "system.cpu.numCycles")
        or key.startswith("system.cpu.dcache.blocked")
    }
    evidence["cache_resources"] = cache_evidence(
        folder / "trace.db", evidence["roi"]
    )
    if resource and not evidence["cache_resources"]["available"]:
        raise RuntimeError(
            "Resource suite requires a binary with PerfCCTCacheEvent support"
        )
    if name in SCHEDULING_CASES:
        evidence["scheduling"] = scheduling_evidence(
            folder / "trace.db", evidence["roi"]
        )
    (folder / "analysis.json").write_text(
        json.dumps(evidence, indent=2, ensure_ascii=False) + "\n"
    )
    print(f"{name}: guest passed; analysis saved", flush=True)
    return evidence


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--out", type=Path)
    cli.add_argument(
        "--binary", type=Path, default=ROOT / "build/RISCV/gem5.opt"
    )
    cli.add_argument("--suite", choices=SUITES, default="paired")
    cli.add_argument(
        "--case",
        choices=list(CASES) + list(RESOURCE_CASES) + list(SCHEDULING_CASES),
        action="append",
    )
    cli.add_argument("--verify-untraced", action="store_true")
    args = cli.parse_args()
    selected = SUITES[args.suite]
    names = args.case or list(selected)
    if any(name not in selected for name in names):
        cli.error("--case must belong to the selected --suite")
    default_out = {
        "paired": "m5out/perfcct-paired/suite",
        "resources": "m5out/perfcct-resource-probes/suite",
        "scheduling": "m5out/perfcct-scheduling/suite",
    }[args.suite]
    out = (args.out or ROOT / default_out).resolve()
    binary = args.binary.resolve()
    out.mkdir(parents=True, exist_ok=True)
    targets = sorted({selected[name][0] for name in names})
    subprocess.run(["make", "-C", str(HERE)] + targets, check=True)
    manifest = {
        "base_revision": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
        ).strip(),
        "source_diff_sha256": hashlib.sha256(
            subprocess.check_output(
                ["git", "diff", "--", "src", "configs"], cwd=ROOT
            )
        ).hexdigest(),
        "binary": str(binary),
        "binary_sha256": digest(binary),
        "suite": args.suite,
        "cases": names,
        "roi_definition": "Committed roi_begin marker to committed roi_end marker",
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    evidence = {name: run_case(name, out, binary) for name in names}
    (out / "summary.json").write_text(
        json.dumps(evidence, indent=2, ensure_ascii=False) + "\n"
    )
    compare = {
        "paired": comparisons,
        "resources": resource_comparisons,
        "scheduling": scheduling_comparisons,
    }[args.suite]
    (out / "comparisons.json").write_text(
        json.dumps(compare(evidence), indent=2, ensure_ascii=False) + "\n"
    )

    if args.verify_untraced:
        checks = [verify_untraced(name, out) for name in names]
        report = {
            "rule": "Compare every non-host statistic including simSeconds; ignore comments and dump delimiters.",
            "all_equivalent": all(item["equivalent"] for item in checks),
            "cases": checks,
        }
        (out / "trace_equivalence.json").write_text(
            json.dumps(report, indent=2) + "\n"
        )
        if not report["all_equivalent"]:
            raise RuntimeError(
                "Trace equivalence failed; see trace_equivalence.json"
            )


if __name__ == "__main__":
    main()
