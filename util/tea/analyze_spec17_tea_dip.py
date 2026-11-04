#!/usr/bin/env python3
"""Audit SPEC17 windows with independent TEA and DIP budgets, using bounded I/O."""
import argparse
from collections import Counter
import configparser
import csv
import hashlib
import json
import math
from pathlib import Path
import re
from statistics import geometric_mean

DIP_STATES = ("Base", "Stall", "FrontEnd", "Misspeculation")
TEA_STATES = ("Computing", "Stalled", "Flushed", "Drained")
CAUSES = (
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
)
INT = {
    "perlbench",
    "gcc",
    "mcf",
    "omnetpp",
    "xalancbmk",
    "x264",
    "deepsjeng",
    "leela",
    "exchange2",
    "xz",
}


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def stats_blocks(path):
    """Read both CPU statistics blocks once without retaining unrelated stats."""
    blocks, current = [], None
    with path.open() as stream:
        for line in stream:
            if "Begin Simulation Statistics" in line:
                current = {}
            elif "End Simulation Statistics" in line:
                assert current is not None, path
                blocks.append(current)
                current = None
            elif current is not None and line.startswith("system.cpu."):
                fields = line.split()
                if len(fields) > 1:
                    try:
                        current[fields[0]] = float(fields[1])
                    except ValueError:
                        pass
    assert current is None and len(blocks) == 2, path
    return blocks


def config(path):
    result = configparser.ConfigParser(interpolation=None)
    result.optionxform = str
    with path.open() as stream:
        result.read_file(stream)
    return result


def resources(ini):
    """Record actual modeled capacities and ports consumed by this comparison."""
    cpu_keys = {
        "fetchWidth",
        "fetchQueueSize",
        "decodeWidth",
        "renameWidth",
        "commitWidth",
        "squashWidth",
        "phyregReleaseWidth",
        "numROBEntries",
        "numPhysIntRegs",
        "numPhysFloatRegs",
        "LQEntries",
        "SQEntries",
        "RARQEntries",
        "RAWQEntries",
        "SbufferEntries",
    }
    object_keys = {
        "size",
        "assoc",
        "mshrs",
        "numEntries",
        "inports",
        "oports",
        "tableSizes",
        "numWays",
        "data_sram_banks",
        "dir_sram_banks",
        "numPredictionPorts",
        "numUpdatePorts",
    }
    return {
        f"{section}.{key}": value
        for section in ini.sections()
        for key, value in ini[section].items()
        if (section == "system.cpu" and key in cpu_keys)
        or (section.startswith("system.") and key in object_keys)
    }


def window(folder, collect_pc=False):
    ini = config(folder / "config.ini")
    cpu = ini["system.cpu"]
    assert cpu.getboolean("enableDip") and cpu.getboolean("enableTea"), folder
    assert cpu.getboolean("enable_difftest") and cpu.getboolean(
        "enable_mem_dedup"
    ), folder
    assert cpu.getint("numThreads") == 1, folder
    assert ini[cpu["clk_domain"]].getint("clock") == 333, folder
    # Sparse samples require a different estimator; never call them all-cycle CPI.
    assert (
        cpu.getint("dipSamplePeriod") == cpu.getint("teaSamplePeriod") == 1
    ), folder
    width = cpu.getint("renameWidth")
    warmup, roi = stats_blocks(folder / "stats.txt")
    n, cycles = roi["system.cpu.committedInsts"], roi["system.cpu.numCycles"]
    assert (
        abs(
            warmup["system.cpu.committedInsts"] - cpu.getint("warmupInstCount")
        )
        < 16
    ), folder
    assert (
        abs(
            n
            - (
                cpu.getint("max_insts_any_thread")
                - cpu.getint("warmupInstCount")
            )
        )
        < 16
    ), folder
    states, totals, causes, contexts = (
        Counter(),
        Counter(),
        Counter(),
        Counter(),
    )
    dip_pc, tea_pc, tea_no_pc = Counter(), Counter(), Counter()
    tea_csv_states, tea_quality = Counter(), Counter()
    with (folder / "system.cpu.dip.csv").open() as stream:
        reader = csv.DictReader(stream)
        assert {"iq", "iq_flags", "frontend_reason"} <= set(
            reader.fieldnames
        ), folder
        for row in reader:
            slots = int(row["slot_weight"])
            assert math.isclose(
                float(row["cycle_weight"]), slots / width, rel_tol=1e-14
            ), folder
            resolution = row["resolution"]
            assert resolution in ("resolved", "unresolved", "pending"), row
            totals[resolution] += slots
            if resolution == "pending":
                continue
            assert row["state"] in DIP_STATES, row
            states[row["state"]] += slots
            mask = int(row["cause"])
            for i, cause in enumerate(CAUSES):
                if mask & (1 << i):
                    causes[cause] += slots
            contexts[
                row["state"],
                row["iq"],
                row["iq_flags"],
                row["frontend_reason"],
            ] += slots
            if collect_pc:
                dip_pc[int(row["pc"], 0), row["state"]] += slots / width / n
    assert (
        sum(totals.values())
        == cycles * width
        == roi["system.cpu.dip.sampledSlots"]
    ), folder
    for resolution, field in [
        ("resolved", "attributedSlots"),
        ("unresolved", "unresolvedSlots"),
        ("pending", "pendingSlots"),
    ]:
        assert totals[resolution] == roi["system.cpu.dip." + field], folder
    for state in DIP_STATES:
        assert (
            states[state] == roi["system.cpu.dip.slotsByState::" + state]
        ), folder
    tea = {
        state: roi["system.cpu.tea.samplesByState::" + state]
        for state in TEA_STATES
    }
    assert sum(tea.values()) == cycles == roi["system.cpu.tea.samples"], folder
    with (folder / "system.cpu.tea.csv").open() as stream:
        reader = csv.DictReader(stream)
        stateful = "state" in reader.fieldnames
        # Legacy archives have no state column; expose that information gap.
        def weights():
            for row in reader:
                value = float(row["sample_weight"])
                tea_quality[
                    "attributed" if int(row["psv"]) >= 0 else row["psv"]
                ] += value
                if stateful:
                    assert row["state"] in TEA_STATES, row
                    tea_csv_states[row["state"]] += value
                if not row["pc"]:
                    tea_no_pc[row["psv"]] += value
                elif collect_pc:
                    pc = int(row["pc"], 0)
                    tea_pc[pc] += value / n
                    if stateful:
                        tea_pc[pc, row["state"]] += value / n
                yield value

        total_tea = math.fsum(weights())
    assert math.isclose(total_tea, cycles, rel_tol=1e-9, abs_tol=0.001), folder
    for key, field in [
        ("attributed", "attributedSamples"),
        ("-1", "cancelledTargets"),
        ("-2", "pendingSamples"),
        ("-3", "unresolvedTargets"),
    ]:
        assert math.isclose(
            tea_quality[key],
            roi["system.cpu.tea." + field],
            rel_tol=1e-9,
            abs_tol=0.001,
        ), (folder, field)
    if stateful:
        assert all(
            math.isclose(
                tea_csv_states[k], tea[k], rel_tol=1e-9, abs_tol=0.001
            )
            for k in TEA_STATES
        ), folder
    data = dict(
        cycles=cycles,
        instructions=n,
        CPI=cycles / n,
        dispatch_width=width,
        DIP_CPI={k: states[k] / width / n for k in DIP_STATES},
        DIP_pending_CPI=totals["pending"] / width / n,
        DIP_overlapping_causes_CPI={
            k: v / width / n for k, v in causes.items()
        },
        TEA_state_PC_available=stateful,
        TEA_CPI={k: v / n for k, v in tea.items()},
        TEA_no_PC_CPI=sum(tea_no_pc.values()) / n,
        contexts=[
            dict(
                state=k[0],
                iq=int(k[1]),
                flags=int(k[2]),
                frontend_reason=int(k[3]),
                CPI=v / width / n,
            )
            for k, v in contexts.items()
        ],
        auxiliary_per_instruction={
            k: v / n
            for k, v in roi.items()
            if "dip.readyBranchWaits" in k or "dip.decodeStrandedSlots" in k
        },
        branch_MPKI=roi["system.cpu.commit.branchMispredicts"] * 1000 / n,
        sha256={
            name: sha256(folder / name)
            for name in [
                "config.ini",
                "stats.txt",
                "system.cpu.dip.csv",
                "system.cpu.tea.csv",
            ]
        },
    )
    assert math.isclose(
        sum(data["DIP_CPI"].values()) + data["DIP_pending_CPI"],
        data["CPI"],
        abs_tol=1e-12,
    )
    return data, ini, dip_pc, tea_pc


def screen(root):
    manifest = json.loads((root / "manifest.json").read_text())
    points = list(dict.fromkeys(t["point"] for t in manifest["tasks"]))
    variants = list(
        dict.fromkeys(
            t["variant"]
            for t in manifest["tasks"]
            if t["variant"] != "control"
        )
    )
    expected_changes = {
        "branch": [
            (
                f"system.cpu.scheduler.IQs{i:02}",
                "prioritizeReadyBranches",
                "false",
                "true",
            )
            for i in range(6)
        ],
        "fold": [("system.cpu", "enableConstantFolding", "false", "true")],
        "scalar": [
            (
                f"system.cpu.scheduler.IQs{i:02}",
                "deferNewEnqueueSelection",
                "true",
                "false",
            )
            for i in range(13)
        ],
        "tage4096": [
            (
                "system.cpu.branchPred.tage",
                "tableSizes",
                " ".join(["2048"] * 8),
                " ".join(["4096"] * 8),
            )
        ],
    }
    rows, pending = [], []
    cached_point, control_data = None, None
    for point, variant in [
        (point, variant) for point in points for variant in variants
    ]:
        folders = [
            root / (point + "-" + name) for name in ["control", variant]
        ]
        records = [
            json.loads((p / "run.json").read_text())
            if (p / "run.json").exists()
            else {}
            for p in folders
        ]
        if any("returncode" not in r for r in records):
            pending.append(point + "-" + variant)
            continue
        assert all(r["returncode"] == 0 for r in records), records
        if cached_point != point:
            control_data = window(folders[0] / "m5out", True)
            cached_point = point
        before, a, pc0, tea0 = control_data
        after, b, pc1, tea1 = window(folders[1] / "m5out", True)
        assert set(a.sections()) == set(b.sections()), point
        changes = sorted(
            (s, k, a[s].get(k), b[s].get(k))
            for s in a.sections()
            for k in a[s].keys() | b[s].keys()
            if a[s].get(k) != b[s].get(k)
        )
        assert changes == expected_changes[variant], changes
        delta = {k: pc1[k] - pc0[k] for k in pc0.keys() | pc1.keys()}
        ordered = sorted(delta, key=lambda key: (delta[key], key))
        selected = list(dict.fromkeys(ordered[:8] + ordered[-8:]))
        rows.append(
            dict(
                point=point,
                variant=variant,
                speedup_pct=(before["CPI"] / after["CPI"] - 1) * 100,
                CPI_delta=after["CPI"] - before["CPI"],
                before=before,
                after=after,
                DIP_delta_CPI={
                    k: after["DIP_CPI"][k] - before["DIP_CPI"][k]
                    for k in DIP_STATES
                },
                TEA_delta_CPI={
                    k: after["TEA_CPI"][k] - before["TEA_CPI"][k]
                    for k in TEA_STATES
                },
                PCs=[
                    dict(
                        pc=hex(k[0]),
                        DIP_state=k[1],
                        DIP_delta_CPI=delta[k],
                        TEA_PC_all_states_delta_CPI=tea1[k[0]] - tea0[k[0]],
                        TEA_PC_delta_CPI_by_state={
                            state: tea1[k[0], state] - tea0[k[0], state]
                            for state in TEA_STATES
                        }
                        if before["TEA_state_PC_available"]
                        and after["TEA_state_PC_available"]
                        else None,
                    )
                    for k in selected
                ],
                config_changes=changes,
            )
        )
    return dict(
        passed=True,
        complete=not pending,
        completed_pairs=len(rows),
        expected_pairs=len(points) * len(variants),
        pending=pending,
        rows=rows,
        manifest_sha256=sha256(root / "manifest.json"),
        scope=(
            "Representative windows only. TEA and DIP are independent "
            "partitions; matching PC is not dynamic causality. Causes and "
            "stage contexts overlap; never sum them as recoverable CPI."
        ),
    )


def suite(root, run_path, references_path, dispatch_path):
    run = json.loads(run_path.read_text())
    assert run["status"] == "completed" and run["conclusion"] == "success", run
    dispatch = json.loads(dispatch_path.read_text())
    assert (
        dispatch["run_id"] == run["databaseId"]
        and dispatch["head"] == run["headSha"]
    )
    metadata = (root / "metadata.txt").read_text()
    metadata_fields = dict(
        line.split(": ", 1) for line in metadata.splitlines() if ": " in line
    )
    assert (
        metadata_fields["config_path"]
        == "configs/example/" + dispatch["inputs"]["configuration"]
    )
    assert metadata_fields["extra_args"] == dispatch["inputs"]["extra_args"]
    early_iqs = [
        int(i)
        for i in re.findall(
            r"--param=system\.cpu\[0\]\.scheduler\.IQs\[(\d+)\]\.deferNewEnqueueSelection=False",
            dispatch["inputs"]["extra_args"],
        )
    ]
    expected_tage = (
        " ".join(["4096"] * 8)
        if "branchPred.tage.tableSizes=[4096]*8"
        in dispatch["inputs"]["extra_args"]
        else None
    )
    checkpoint_root = Path(metadata_fields["checkpoint_root"])
    ref = Path(metadata_fields["ref_so"])
    ref_sha256 = sha256(ref)
    assert f"{ref_sha256}  {ref}\n" in metadata
    assert sha256(root / "cluster.json") == dispatch["profile_sha256"]
    assert f'workflow_run_id: {run["databaseId"]}\n' in metadata
    assert f'commit: {run["headSha"]}\n' in metadata
    profile = json.loads((root / "cluster.json").read_text())
    expected = {
        f"{w}_{p}" for w, entry in profile.items() for p in entry["points"]
    }
    listed = {
        line.split()[0]
        for line in (root / "checkpoints.lst").read_text().splitlines()
        if line.strip()
    }
    actual = {p.name for p in (root / "spec_all").iterdir() if p.is_dir()}
    assert expected == listed == actual and len(expected) == dispatch["points"]
    references = json.loads(references_path.read_text())
    points, workloads = [], []
    baseline_resources = None
    for workload, entry in profile.items():
        weighted = 0
        total_weight = math.fsum(map(float, entry["points"].values()))
        for point, weight in entry["points"].items():
            name = f"{workload}_{point}"
            folder = root / "spec_all" / name
            assert (folder / "completed").is_file() and not (
                folder / "abort"
            ).exists(), folder
            data, ini, _, _ = window(folder / "m5out")
            if expected_tage:
                assert (
                    ini["system.cpu.branchPred.tage"]["tableSizes"]
                    == expected_tage
                ), folder
            assert all(
                not ini[f"system.cpu.scheduler.IQs{i:02}"].getboolean(
                    "deferNewEnqueueSelection"
                )
                for i in early_iqs
            ), folder
            for section, parameters in dispatch.get(
                "expected_ini", {}
            ).items():
                assert all(
                    ini[section].get(key) == value
                    for key, value in parameters.items()
                ), (folder, section)
            assert ini["system.cpu"]["difftest_ref_so"] == str(ref), folder
            assert (
                Path(ini["system"]["gcpt_file"]).parent
                == checkpoint_root / workload / point
            ), folder
            assert ini["system"]["gcpt_restorer_file"] == "", folder
            assert (
                ini["system.cpu"].getint("warmupInstCount") == 20000000
            ), folder
            assert (
                ini["system.cpu"].getint("max_insts_any_thread") == 40000000
            ), folder
            current_resources = resources(ini)
            if baseline_resources is None:
                baseline_resources = current_resources
            assert current_resources == baseline_resources, folder
            q = float(weight) / total_weight
            weighted += q * data["CPI"]
            points.append(
                dict(
                    point=name, workload=workload, normalized_weight=q, **data
                )
            )
        workloads.append(
            dict(
                workload=workload,
                benchmark=workload.split("_rate_", 1)[0].split(".", 1)[1],
                CPI=weighted,
                instructions=int(entry["insts"]),
            )
        )
    benchmarks = []
    for benchmark in sorted(
        {w["benchmark"] for w in workloads} - {"specrand_i", "specrand_f"}
    ):
        group = [w for w in workloads if w["benchmark"] == benchmark]
        cycles = sum(w["CPI"] * w["instructions"] for w in group)
        benchmarks.append(
            dict(
                benchmark=benchmark,
                CPI=cycles / sum(w["instructions"] for w in group),
                score_per_GHz=references[benchmark] * 1e9 / cycles,
            )
        )
    assert len(benchmarks) == 23 and len(workloads) == 38
    scores = {
        group: geometric_mean(
            b["score_per_GHz"]
            for b in benchmarks
            if group == "overall"
            or (b["benchmark"] in INT) == (group == "Int")
        )
        for group in ["Int", "FP", "overall"]
    }
    official = {
        k: float(v)
        for k, v in re.findall(
            r"Estimated (Int|FP|overall) score per GHz:\s*([0-9.eE+-]+)",
            (root / "score.txt").read_text(),
        )
    }
    assert official.keys() == scores.keys()
    assert all(
        math.isclose(scores[k], official[k], rel_tol=2e-6) for k in scores
    ), (scores, official)
    return dict(
        passed=True,
        run_id=run["databaseId"],
        head=run["headSha"],
        points=points,
        workloads=workloads,
        benchmarks=benchmarks,
        resources=baseline_resources,
        scores_per_GHz=scores,
        official_scores_per_GHz=official,
        ref_sha256=ref_sha256,
        profile_sha256=sha256(root / "cluster.json"),
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=["screen", "suite"])
    parser.add_argument("root", type=Path)
    parser.add_argument("--run-json", type=Path)
    parser.add_argument("--references", type=Path)
    parser.add_argument("--dispatch-json", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.mode == "suite" and (
        args.run_json is None
        or args.references is None
        or args.dispatch_json is None
    ):
        parser.error(
            "suite requires --run-json, --dispatch-json and --references"
        )
    tool_sha256 = sha256(Path(__file__))
    data = (
        screen(args.root)
        if args.mode == "screen"
        else suite(
            args.root, args.run_json, args.references, args.dispatch_json
        )
    )
    data["tool_sha256"] = tool_sha256
    args.output.write_text(json.dumps(data, indent=2) + "\n")
    print(
        json.dumps(
            {
                k: v
                for k, v in data.items()
                if k
                not in [
                    "rows",
                    "points",
                    "workloads",
                    "benchmarks",
                    "resources",
                ]
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
