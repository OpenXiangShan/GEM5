"""Separate display insensitivity from missing DIP/TEA cycle budgets."""

import argparse
import csv
import json
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("before", type=Path)
parser.add_argument("after", type=Path)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
proofs = [
    json.loads((folder / "audit.json").read_text())
    for folder in [args.before, args.after]
]
assert all(proof["passed"] for proof in proofs)
assert proofs[0]["cluster_sha256"] == proofs[1]["cluster_sha256"]
assert proofs[0]["configuration"] == proofs[1]["configuration"]
DIP = ["Base", "Stall", "FrontEnd", "Misspeculation", "pending"]
TEA = ["tea_Computing", "tea_Stalled", "tea_Flushed", "tea_Drained"]


def table(folder, scope):
    with (folder / (scope + ".csv")).open() as stream:
        rows = list(csv.DictReader(stream))
    key = (
        (lambda row: row["workload"] + "_" + row["point"])
        if scope == "points"
        else (lambda row: row["benchmark"])
    )
    result = {key(row): row for row in rows}
    assert len(result) == len(rows)
    return result


results = {}
for scope in ["points", "benchmarks"]:
    before, after = [table(folder, scope) for folder in [args.before, args.after]]
    assert before.keys() == after.keys()
    rows = []
    for name in sorted(before):
        a, b = before[name], after[name]
        cpi0, cpi1 = float(a["CPI"]), float(b["CPI"])
        changes = {
            kind: {field: float(b[field]) - float(a[field]) for field in fields}
            for kind, fields in [("DIP", DIP), ("TEA", TEA)]
        }
        residuals = {
            kind: sum(values.values()) - (cpi1 - cpi0)
            for kind, values in changes.items()
        }
        assert all(abs(value) < 1e-12 for value in residuals.values())
        shares = {
            kind: max(
                abs(float(b[field]) / cpi1 - float(a[field]) / cpi0) * 100
                for field in fields
            )
            for kind, fields in [("DIP", DIP), ("TEA", TEA)]
        }
        speedup = (cpi0 / cpi1 - 1) * 100
        rows.append(
            dict(
                name=name,
                speedup_pct=speedup,
                before_CPI=cpi0,
                after_CPI=cpi1,
                delta_CPI=cpi1 - cpi0,
                delta_budget_CPI=changes,
                delta_residual_CPI=residuals,
                max_state_share_change_pp=shares,
                share_display_flag=abs(speedup) >= 0.5 and max(shares.values()) < 0.5,
            )
        )
    results[scope] = dict(
        rows=rows,
        share_display_flags=[row["name"] for row in rows if row["share_display_flag"]],
    )
result = dict(
    passed=True,
    points=len(results["points"]["rows"]),
    benchmarks=len(results["benchmarks"]["rows"]),
    flag_definition=(
        "|speedup| >= 0.5%, both maximum state-share changes < 0.5 percentage "
        "points. Display threshold only, not a sampler-bug or significance test."
    ),
    interpretation=(
        "A stable percentage can conceal a changed absolute budget. Closure tests "
        "missing/duplicated weight; it does not prove correct attribution."
    ),
    comparisons=results,
)
args.output.write_text(json.dumps(result, indent=2))
print(
    json.dumps(
        dict(
            passed=True,
            flags={
                scope: data["share_display_flags"] for scope, data in results.items()
            },
        ),
        indent=2,
    )
)
