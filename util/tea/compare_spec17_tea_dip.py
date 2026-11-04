#!/usr/bin/env python3
"""Compare audited SPEC17 suites using profile-weighted absolute CPI budgets."""
import argparse
from collections import Counter
import json
import math
from pathlib import Path
from statistics import geometric_mean

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


def compare(before, after):
    assert before["passed"] and after["passed"]
    assert before["profile_sha256"] == after["profile_sha256"]
    assert before["ref_sha256"] == after["ref_sha256"]
    a = {r["point"]: r for r in before["points"]}
    b = {r["point"]: r for r in after["points"]}
    assert a.keys() == b.keys()
    inputs = {r["workload"]: r for r in before["workloads"]}
    assert inputs.keys() == {r["workload"] for r in after["workloads"]}
    point_rows = []
    for point, left in a.items():
        right = b[point]
        workload = left["workload"]
        assert (
            workload == right["workload"]
            and left["normalized_weight"] == right["normalized_weight"]
        )
        assert inputs[workload]["instructions"] == next(
            r["instructions"]
            for r in after["workloads"]
            if r["workload"] == workload
        )
        benchmark = inputs[workload]["benchmark"]
        instruction_total = sum(
            r["instructions"]
            for r in inputs.values()
            if r["benchmark"] == benchmark
        )
        effective_weight = (
            left["normalized_weight"]
            * inputs[workload]["instructions"]
            / instruction_total
        )
        weighted = (right["CPI"] - left["CPI"]) * effective_weight
        point_rows.append(
            dict(
                point=point,
                workload=workload,
                benchmark=benchmark,
                effective_benchmark_weight=effective_weight,
                weighted_delta_CPI=weighted,
                speedup_pct=(left["CPI"] / right["CPI"] - 1) * 100,
            )
        )
    rows = []
    for left in before["benchmarks"]:
        benchmark = left["benchmark"]
        right = next(
            r for r in after["benchmarks"] if r["benchmark"] == benchmark
        )
        relevant = [p for p in point_rows if p["benchmark"] == benchmark]
        metrics = {side: Counter() for side in ["before", "after"]}
        for point in relevant:
            for side, data in [
                ("before", a[point["point"]]),
                ("after", b[point["point"]]),
            ]:
                q = point["effective_benchmark_weight"]
                metrics[side]["CPI"] += q * data["CPI"]
                metrics[side]["DIP_pending"] += q * data["DIP_pending_CPI"]
                metrics[side]["TEA_no_PC"] += q * data["TEA_no_PC_CPI"]
                for family in [
                    "DIP_CPI",
                    "TEA_CPI",
                    "DIP_overlapping_causes_CPI",
                ]:
                    for key, value in data[family].items():
                        metrics[side][family + "." + key] += q * value
        for side in ["before", "after"]:
            data = metrics[side]
            assert math.isclose(
                sum(
                    data["TEA_CPI." + k]
                    for k in ["Computing", "Stalled", "Flushed", "Drained"]
                ),
                data["CPI"],
                abs_tol=1e-11,
            )
            assert math.isclose(
                sum(
                    data["DIP_CPI." + k]
                    for k in ["Base", "Stall", "FrontEnd", "Misspeculation"]
                )
                + data["DIP_pending"],
                data["CPI"],
                abs_tol=1e-11,
            )
        assert math.isclose(
            metrics["before"]["CPI"], left["CPI"], abs_tol=1e-11
        )
        assert math.isclose(
            metrics["after"]["CPI"], right["CPI"], abs_tol=1e-11
        )
        delta = {
            k: metrics["after"][k] - metrics["before"][k]
            for k in metrics["before"].keys() | metrics["after"].keys()
        }
        assert math.isclose(
            sum(p["weighted_delta_CPI"] for p in relevant),
            delta["CPI"],
            abs_tol=1e-11,
        )
        ranked = sorted(
            relevant, key=lambda p: (p["weighted_delta_CPI"], p["point"])
        )
        rows.append(
            dict(
                benchmark=benchmark,
                speedup_pct=(left["CPI"] / right["CPI"] - 1) * 100,
                before=dict(metrics["before"]),
                after=dict(metrics["after"]),
                delta=delta,
                largest_weighted_improvement=ranked[0]
                if ranked[0]["weighted_delta_CPI"] < 0
                else None,
                largest_weighted_regression=ranked[-1]
                if ranked[-1]["weighted_delta_CPI"] > 0
                else None,
                suite_log_contribution=math.log(left["CPI"] / right["CPI"])
                / 23,
            )
        )
    groups = {}
    for name in ["Int", "FP", "overall"]:
        chosen = [
            r
            for r in rows
            if name == "overall" or (r["benchmark"] in INT) == (name == "Int")
        ]
        gain = geometric_mean(
            r["before"]["CPI"] / r["after"]["CPI"] for r in chosen
        )
        assert math.isclose(
            gain,
            after["scores_per_GHz"][name] / before["scores_per_GHz"][name],
            rel_tol=1e-12,
        )
        groups[name] = dict(
            speedup_pct=(gain - 1) * 100,
            before_score_per_GHz=before["scores_per_GHz"][name],
            after_score_per_GHz=after["scores_per_GHz"][name],
        )
    resource_changes = {
        key: dict(
            before=before["resources"].get(key),
            after=after["resources"].get(key),
        )
        for key in before["resources"].keys() | after["resources"].keys()
        if before["resources"].get(key) != after["resources"].get(key)
    }
    return dict(
        passed=True,
        before_run=before["run_id"],
        after_run=after["run_id"],
        profile_sha256=before["profile_sha256"],
        points=len(point_rows),
        groups=groups,
        resource_changes=resource_changes,
        benchmarks=rows,
        weighted_points=point_rows,
        scope=(
            "Audited identical-profile suites. Absolute state CPI uses "
            "each window retired instructions, normalized SimPoint "
            "weights, then input instruction weights. TEA/DIP are "
            "independent partitions; cause bits overlap. Clock/PPA "
            "outside simulation evidence."
        ),
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = compare(
        json.loads(args.before.read_text()), json.loads(args.after.read_text())
    )
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(
        json.dumps(
            {
                k: result[k]
                for k in [
                    "passed",
                    "before_run",
                    "after_run",
                    "points",
                    "groups",
                    "resource_changes",
                ]
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
