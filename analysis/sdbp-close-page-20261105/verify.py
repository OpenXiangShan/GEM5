#!/usr/bin/env python3
"""Verify the close-page CI runs and compare them with the open-page baseline."""

from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor
import csv
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess
import time

ROOT = Path(__file__).resolve().parent
PLAN = json.loads((ROOT / "dispatch.json").read_text())
BASE = Path("/nfs/home/share/gem5_ci/performance_data") / PLAN["benchmark_type"]
LABELS = ("nopf_lru", "nopf_sdbp", "pf_lru", "pf_sdbp")
PAIRS = (("nopf_lru", "nopf_sdbp"), ("pf_lru", "pf_sdbp"))
BASELINE = {
    "nopf_lru": BASE / "20261102_155509_de1df30a_idealkmhv3_run1861",
    "nopf_sdbp": BASE / "20261102_165612_de1df30a_idealkmhv3_run1863",
    "pf_lru": BASE / "20261102_155054_de1df30a0_idealkmhv3_run1860",
    "pf_sdbp": BASE / "20261102_162920_de1df30a_idealkmhv3_run1862",
}
SDBP_PARAMS = {
    "sampler_num": 128,
    "sampler_assoc": 6,
    "l1_copy_protection": True,
    "sampler_dead_first": True,
    "sampler_protect_youngest": 0,
    "dead_threshold": 8,
    "enable_bypass": False,
    "pc_hash_type": "xor_fold",
    "pc_shift": 1,
}
SCORE_ROW = re.compile(
    r"^\s*(\S+)\s+([0-9.eE+-]+)\s+([0-9.eE+-]+)\s+"
    r"([0-9.eE+-]+)\s+([0-9.eE+-]+)\s*$"
)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def metadata(path):
    return {
        key.strip(): value.strip()
        for key, sep, value in (
            line.partition(":") for line in (path / "metadata.txt").read_text().splitlines()
        )
        if sep
    }


def run_state(label, entry):
    data = json.loads(
        subprocess.check_output(
            ["gh", "run", "view", str(entry["run_id"]), "--repo", "OpenXiangShan/GEM5",
             "--json", "status,conclusion,headSha,url"],
            text=True,
            timeout=60,
        )
    )
    assert data["headSha"] == PLAN["sha"], (label, data["headSha"])
    return label, data


def wait_for_runs():
    while True:
        with ThreadPoolExecutor(max_workers=4) as pool:
            states = list(pool.map(lambda item: run_state(*item), PLAN["variants"].items()))
        print(" ".join(f"{label}={data['status']}" for label, data in states), flush=True)
        if all(data["status"] == "completed" for _, data in states):
            failures = [(label, data["conclusion"]) for label, data in states
                        if data["conclusion"] != "success"]
            assert not failures, failures
            return
        time.sleep(30)


def find_archives():
    found = {}
    for label, entry in PLAN["variants"].items():
        matches = []
        for path in BASE.glob(f"*{PLAN['sha'][:8]}*"):
            if (path / "metadata.txt").exists():
                meta = metadata(path)
                if meta.get("workflow_run_id") == str(entry["run_id"]):
                    matches.append(path)
        assert len(matches) == 1, (label, matches)
        found[label] = matches[0]
    return found


def parse_score(path):
    text = (path / "score.txt").read_text()
    benchmarks = {}
    aggregates = {}
    section = None
    for line in text.splitlines():
        if "================ Int " in line:
            section = "int"
        elif "================ FP " in line:
            section = "fp"
        match = re.search(r"Estimated (Int|FP|overall) score per GHz: ([0-9.eE+-]+)", line)
        if match:
            aggregates[match.group(1).lower() + "_avg"] = float(match.group(2))
            continue
        match = SCORE_ROW.match(line)
        if match and section:
            name, sim_time, ref_time, score, coverage = match.groups()
            benchmarks[name] = {
                "time": float(sim_time),
                "ref_time": float(ref_time),
                "score_per_ghz": float(score),
                "coverage": float(coverage),
                "section": section,
            }
    assert len(benchmarks) == 29, (path, len(benchmarks))
    assert set(aggregates) == {"int_avg", "fp_avg", "overall_avg"}, aggregates
    return benchmarks, aggregates


def check_point(task):
    label, archive, point = task
    parent = archive / "spec_all" / point
    assert (parent / "completed").is_file(), (label, point, "not completed")
    assert not (parent / "abort").exists(), (label, point, "aborted")
    cfg = json.loads((parent / "m5out" / "config.json").read_text())
    assert cfg["system"]["enable_difftest"] and cfg["system"]["enable_mem_dedup"], label
    memory = cfg["system"]["mem_ctrls"][0]
    assert memory["type"] == "DRAMsim3", (label, memory["type"])
    assert memory["configFile"].endswith("_close_page.ini"), (label, memory["configFile"])
    policies = [slice_cfg["inner_cache"]["replacement_policy"]
                for wrapper in cfg["system"]["l2_wrappers"]
                for slice_cfg in wrapper["slices"]]
    expected_type = "LRURP" if label.endswith("lru") else "SDBPRP"
    assert len(policies) == 4
    for policy in policies:
        assert policy["type"] == expected_type, (label, policy["type"])
        if expected_type == "SDBPRP":
            for key, value in SDBP_PARAMS.items():
                assert policy[key] == value, (label, point, key, policy[key], value)
    dram = json.loads((parent / "dramsim3.json").read_text())
    row_hits = sum(channel.get("num_read_row_hits", 0) for channel in dram.values())
    reads = sum(channel.get("num_read_cmds", 0) for channel in dram.values())
    assert row_hits == 0, (label, point, reads, row_hits)
    return {"label": label, "point": point, "used": Path(__file__).resolve()}


def main():
    wait_for_runs()
    archives = find_archives()
    checks = {}
    expected_points = None
    for label, archive in archives.items():
        meta = metadata(archive)
        assert meta["commit"] == PLAN["sha"], (label, meta["commit"])
        assert meta["resolved_extra_args"].find("_close_page.ini") >= 0, label
        points = {line.split()[0] for line in
                  (archive / "checkpoints.lst").read_text().splitlines() if line.strip()}
        actual = {path.name for path in (archive / "spec_all").iterdir() if path.is_dir()}
        assert points == actual, (label, points - actual, actual - points)
        if expected_points is None:
            expected_points = points
        assert points == expected_points
        checks[label] = {
            "archive": str(archive),
            "metadata": meta,
            "checkpoints_sha256": sha(archive / "checkpoints.lst"),
            "cluster_sha256": sha(archive / "cluster.json"),
            "ref_manifest_sha256": sha(archive / "ref-manifest.json"),
        }
    for field in ("checkpoints_sha256", "cluster_sha256", "ref_manifest_sha256"):
        assert len({item[field] for item in checks.values()}) == 1, field

    tasks = [(label, archives[label], point) for label in LABELS
             for point in sorted(expected_points)]
    with ThreadPoolExecutor(max_workers=16) as pool:
        rows = list(pool.map(check_point, tasks))
    assert len(rows) == 4 * len(expected_points)

    scores = {}
    aggregates = {}
    baseline_scores = {}
    baseline_aggregates = {}
    for label in LABELS:
        scores[label], aggregates[label] = parse_score(archives[label])
        baseline_scores[label], baseline_aggregates[label] = parse_score(BASELINE[label])
    comparisons = {}
    for base, candidate in PAIRS:
        comparisons[f"{candidate}_vs_{base}_close_page_pct"] = 100 * (
            aggregates[candidate]["overall_avg"] / aggregates[base]["overall_avg"] - 1
        )
        comparisons[f"{candidate}_vs_{base}_open_page_pct"] = 100 * (
            baseline_aggregates[candidate]["overall_avg"] /
            baseline_aggregates[base]["overall_avg"] - 1
        )
    for mode in ("nopf", "pf"):
        comparisons[f"{mode}_sdbp_vs_lru_shift_pp"] = (
            comparisons[f"{mode}_sdbp_vs_{mode}_lru_close_page_pct"] -
            comparisons[f"{mode}_sdbp_vs_{mode}_lru_open_page_pct"]
        )

    result = {
        "sha": PLAN["sha"],
        "points_per_run": len(expected_points),
        "benchmarks": len(scores[LABELS[0]]),
        "archives": checks,
        "scores_3ghz": {label: 3 * aggregates[label]["overall_avg"] for label in LABELS},
        "baseline_scores_3ghz": {label: 3 * baseline_aggregates[label]["overall_avg"]
                                 for label in LABELS},
        "comparisons": comparisons,
        "row_hits_verified_zero": True,
    }
    (ROOT / "verification.json").write_text(json.dumps(result, indent=2) + "\n")
    with (ROOT / "benchmark-comparison.csv").open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["benchmark", "section", "open_nopf_sdbp_lru_pct",
                         "close_nopf_sdbp_lru_pct", "open_pf_sdbp_lru_pct",
                         "close_pf_sdbp_lru_pct"])
        for benchmark in scores[LABELS[0]]:
            def pct(mode):
                return 100 * (scores[f"{mode}_sdbp"][benchmark]["score_per_ghz"] /
                              scores[f"{mode}_lru"][benchmark]["score_per_ghz"] - 1)
            def open_pct(mode):
                return 100 * (baseline_scores[f"{mode}_sdbp"][benchmark]["score_per_ghz"] /
                              baseline_scores[f"{mode}_lru"][benchmark]["score_per_ghz"] - 1)
            writer.writerow([benchmark, scores[f"pf_lru"][benchmark]["section"],
                             open_pct("nopf"), pct("nopf"), open_pct("pf"), pct("pf")])
    print(json.dumps(result["scores_3ghz"], indent=2))
    print(json.dumps(comparisons, indent=2))


if __name__ == "__main__":
    main()
