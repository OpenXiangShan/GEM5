#!/usr/bin/env python3
"""Compare paired performance tables using explicit columns and benchmark weights."""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path


def number(value, location, positive=False):
    result = float(value)
    if not math.isfinite(result) or result < 0 or (positive and result == 0):
        raise ValueError(
            f'{location}: expected finite {"positive" if positive else "nonnegative"} number'
        )
    return result


def load_table(spec, base):
    path = (base / spec["path"]).resolve()
    columns = spec["columns"]
    metrics = {**spec.get("states", {}), **spec.get("events", {})}
    if set(spec.get("states", {})) & set(spec.get("events", {})):
        raise ValueError("state and event names must be distinct")
    rows = {}
    with path.open(newline="") as stream:
        reader = csv.DictReader(stream)
        required = set(columns.values()) | set(metrics.values())
        if not required <= set(reader.fieldnames or []):
            raise ValueError(
                f"{path}: missing columns {sorted(required - set(reader.fieldnames or []))}"
            )
        for line, row in enumerate(reader, 2):
            key = tuple(
                row[columns[k]] for k in ["benchmark", "workload", "point"]
            )
            if not all(key) or key in rows:
                raise ValueError(
                    f"{path}:{line}: empty or duplicate identity {key}"
                )
            parsed = {
                k: number(row[columns[k]], f"{path}:{line}:{k}", positive=True)
                for k in ["cycles", "instructions"]
            }
            for k in ["ipc", "weight"]:
                if k in columns:
                    parsed[k] = number(
                        row[columns[k]],
                        f"{path}:{line}:{k}",
                        positive=k == "ipc",
                    )
            parsed["metrics"] = {
                k: number(row[col], f"{path}:{line}:{col}")
                for k, col in metrics.items()
            }
            if spec.get("states") and not math.isclose(
                sum(parsed["metrics"][k] for k in spec["states"]),
                parsed["cycles"],
                rel_tol=1e-8,
                abs_tol=1e-3,
            ):
                raise ValueError(f"{path}:{line}: states do not sum to cycles")
            rows[key] = parsed
    if not rows:
        raise ValueError(f"{path}: no data rows")
    return path, rows


def compare(config, base):
    """Return weighted results and input paths, without launching experiments or writing."""
    specs = [config["baseline"], config["candidate"]]
    for kind in ["states", "events"]:
        if set(specs[0].get(kind, {})) != set(specs[1].get(kind, {})):
            raise ValueError(f"baseline and candidate {kind} must match")
    loaded = [load_table(spec, base) for spec in specs]
    paths = {p for p, _ in loaded}
    before, after = [rows for _, rows in loaded]
    if before.keys() != after.keys():
        raise ValueError(
            f"point coverage differs: baseline-only={len(before.keys() - after.keys())}, "
            f"candidate-only={len(after.keys() - before.keys())}"
        )
    benchmarks = sorted({k[0] for k in before})
    workload_benchmark = {}
    for benchmark, workload, _ in before:
        if (
            workload in workload_benchmark
            and workload_benchmark[workload] != benchmark
        ):
            raise ValueError(
                f"workload {workload} belongs to multiple benchmarks"
            )
        workload_benchmark[workload] = benchmark
    groups = config.get("groups", {"overall": benchmarks})
    for name, members in groups.items():
        if (
            not members
            or len(members) != len(set(members))
            or not set(members) <= set(benchmarks)
        ):
            raise ValueError(
                f"{name}: empty, duplicate or unknown benchmark members"
            )
    weights = {}
    if "profile" in config:
        profile_path = (base / config["profile"]).resolve()
        paths.add(profile_path)
        profile = json.loads(profile_path.read_text())
        expected = {
            (wl, str(pt))
            for wl, item in profile.items()
            for pt in item["points"]
        }
        actual = {(wl, pt) for _, wl, pt in before}
        if actual != expected or set(profile) != set(workload_benchmark):
            raise ValueError(
                "table coverage must exactly match profile workloads and points"
            )
        lengths = {
            wl: number(item["insts"], f"profile:{wl}:insts", positive=True)
            for wl, item in profile.items()
        }
        sums = {
            wl: sum(
                number(v, f"profile:{wl}:weight")
                for v in item["points"].values()
            )
            for wl, item in profile.items()
        }
        if any(v == 0 for v in sums.values()):
            raise ValueError(
                "profile has a workload with zero total point weight"
            )
        for key in before:
            bmk, wl, pt = key
            length = sum(
                v for w, v in lengths.items() if workload_benchmark[w] == bmk
            )
            weights[key] = (
                float(profile[wl]["points"][pt])
                / sums[wl]
                * lengths[wl]
                / length
            )
    else:
        if any("weight" not in spec["columns"] for spec in specs):
            raise ValueError(
                "provide a profile or benchmark-level weight column on both sides"
            )
        for key in before:
            if not math.isclose(
                before[key]["weight"],
                after[key]["weight"],
                rel_tol=1e-12,
                abs_tol=1e-15,
            ):
                raise ValueError(
                    f"{key}: baseline and candidate weights differ"
                )
            weights[key] = before[key]["weight"]
        for bmk in benchmarks:
            total = sum(v for k, v in weights.items() if k[0] == bmk)
            if not math.isclose(total, 1, rel_tol=1e-8, abs_tol=1e-8):
                raise ValueError(
                    f"{bmk}: benchmark weights must sum to 1, got {total}"
                )
            for key in weights:
                if key[0] == bmk:
                    weights[key] /= total
    use_ipc = all("ipc" in spec["columns"] for spec in specs)
    if any("ipc" in spec["columns"] for spec in specs) and not use_ipc:
        raise ValueError("official IPC column must be provided on both sides")
    points = []
    totals = {
        bmk: dict(
            benchmark=bmk,
            points=0,
            weight=0.0,
            before_cpi=0.0,
            after_cpi=0.0,
            before_score_cpi=0.0,
            after_score_cpi=0.0,
        )
        for bmk in benchmarks
    }
    metric_names = list(specs[0].get("states", {})) + list(
        specs[0].get("events", {})
    )
    for key in sorted(before):
        bmk, wl, pt = key
        weight = weights[key]
        a, b = before[key], after[key]
        cpi = [r["cycles"] / r["instructions"] for r in [a, b]]
        row = dict(
            benchmark=bmk,
            workload=wl,
            point=pt,
            weight=weight,
            before_cycles=a["cycles"],
            after_cycles=b["cycles"],
            before_instructions=a["instructions"],
            after_instructions=b["instructions"],
            before_cpi=cpi[0],
            after_cpi=cpi[1],
            speedup_pct=(cpi[0] / cpi[1] - 1) * 100,
            weighted_delta_cpi=weight * (cpi[1] - cpi[0]),
        )
        total = totals[bmk]
        total["points"] += 1
        total["weight"] += weight
        for side, item, measured in zip(["before", "after"], [a, b], cpi):
            total[side + "_cpi"] += weight * measured
            total[side + "_score_cpi"] += weight * (
                1 / item["ipc"] if use_ipc else measured
            )
            for name in metric_names:
                value = item["metrics"][name] / item["instructions"]
                row[side + "_" + name + "_per_inst"] = value
                field = side + "_" + name + "_per_inst"
                total[field] = total.get(field, 0.0) + weight * value
        points.append(row)
    for bmk, total in totals.items():
        total["speedup_pct"] = (
            total["before_cpi"] / total["after_cpi"] - 1
        ) * 100
        total["score_speedup_pct"] = (
            total["before_score_cpi"] / total["after_score_cpi"] - 1
        ) * 100
        total["delta_cpi"] = total["after_cpi"] - total["before_cpi"]
        for name in metric_names:
            total["delta_" + name + "_per_inst"] = (
                total["after_" + name + "_per_inst"]
                - total["before_" + name + "_per_inst"]
            )
    suites = {}
    for name, members in groups.items():
        values = [totals[bmk] for bmk in members]
        suites[name] = dict(
            benchmarks=len(members),
            measured_speedup_pct=math.expm1(
                sum(math.log(v["before_cpi"] / v["after_cpi"]) for v in values)
                / len(values)
            )
            * 100,
            score_speedup_pct=math.expm1(
                sum(
                    math.log(v["before_score_cpi"] / v["after_score_cpi"])
                    for v in values
                )
                / len(values)
            )
            * 100,
        )
    selection = {}
    for bmk in benchmarks:
        rows = [r for r in points if r["benchmark"] == bmk]
        gain = [r for r in rows if r["weighted_delta_cpi"] < 0]
        penalty = [r for r in rows if r["weighted_delta_cpi"] > 0]
        selection[bmk] = dict(
            largest_gain=min(gain, key=lambda r: r["weighted_delta_cpi"])
            if gain
            else None,
            largest_penalty=max(penalty, key=lambda r: r["weighted_delta_cpi"])
            if penalty
            else None,
        )
    paths.update((base / p).resolve() for p in config.get("evidence", []))
    result = dict(
        title=config["title"],
        context=config.get("context", ""),
        baseline_label=specs[0]["label"],
        candidate_label=specs[1]["label"],
        paired_points=len(points),
        benchmarks=[totals[k] for k in benchmarks],
        suites=suites,
        selection=selection,
        states=list(specs[0].get("states", {})),
        events=list(specs[0].get("events", {})),
        score_basis="provided IPC"
        if use_ipc
        else "cycles / retired instructions",
    )
    return result, points, paths


def digest(path):
    hasher = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            hasher.update(block)
    return hasher.hexdigest()


def write_csv(path, rows):
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def report(result):
    lines = [
        "# " + result["title"],
        "",
        f"比较：{result['baseline_label']} → {result['candidate_label']}",
        "",
        result["context"],
        "",
        f"离线配对：{result['paired_points']} 点 / {len(result['benchmarks'])} benchmark。",
        f"分组计分复算口径：{result['score_basis']}。正负选点始终使用实际 cycles / retired instructions。",
        "",
        "此报告核对表内覆盖、权重及已配置状态守恒；来源文件哈希仅用于追溯。",
        "官方 success、正确性、二进制/REF、实际 INI、测量窗口和默认采纳须另查原始审计。",
        "事件可重叠，事件 per_inst 不可求和为总 CPI，也不能据此宣称单项因果收益。",
        "",
        "## 分组收益",
        "",
        "| 分组 | Benchmark 数 | 实测收益 % | 计分口径收益 % |",
        "| --- | ---: | ---: | ---: |",
    ]
    for key, value in result["suites"].items():
        lines.append(
            f"| {key} | {value['benchmarks']} | "
            f"{value['measured_speedup_pct']:+.6f} | "
            f"{value['score_speedup_pct']:+.6f} |"
        )
    lines += [
        "",
        "## Benchmark 与加权正反点",
        "",
        "ΔCPI = candidate − baseline；负值为收益，正值为回退。只在同一 benchmark 内按贡献比较切片。",
        "",
        "| Benchmark | 控制 CPI | 候选 CPI | 收益 % | 最大收益切片 | 最大回退切片 |",
        "| --- | ---: | ---: | ---: | --- | --- |",
    ]
    for row in result["benchmarks"]:
        selected = result["selection"][row["benchmark"]]
        cells = []
        for kind in ["largest_gain", "largest_penalty"]:
            point = selected[kind]
            cells.append(
                f"{point['workload']}_{point['point']} ({point['weighted_delta_cpi']:+.6g})"
                if point
                else "无"
            )
        lines.append(
            f"| {row['benchmark']} | {row['before_cpi']:.9f} | "
            f"{row['after_cpi']:.9f} | {row['speedup_pct']:+.6f} | "
            f"{' | '.join(cells)} |"
        )
    lines += [
        "",
        "## 产物与口径",
        "",
        "- [points.csv](points.csv)：全部配对、真实周期/退休数、权重、CPI、贡献与配置指标。",
        "- [benchmarks.csv](benchmarks.csv)：加权 CPI、计分 CPI、状态和事件的归一化变化。",
        "- [summary.json](summary.json)：分组收益、正反选点和覆盖/口径。",
        "- [audit.json](audit.json)：配置、工具和输入 SHA256，以及本次执行范围。",
        "",
    ]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("config", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    config_path = args.config.resolve()
    config = json.loads(config_path.read_text())
    result, points, sources = compare(config, config_path.parent)
    sources.update([config_path, Path(__file__).resolve()])
    hashes = {str(p): digest(p) for p in sorted(sources)}
    # An output directory is an experiment identity: never overwrite existing evidence.
    args.output.mkdir(parents=True, exist_ok=False)
    write_csv(args.output / "points.csv", points)
    write_csv(args.output / "benchmarks.csv", result["benchmarks"])
    for name, value in [
        ("summary.json", result),
        (
            "audit.json",
            dict(
                table_checks_passed=True,
                paired_points=len(points),
                configuration=config,
                sources_sha256=hashes,
                scope="Offline table checks only; external evidence is hashed, not re-audited.",
            ),
        ),
    ]:
        (args.output / name).write_text(
            json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False)
            + "\n"
        )
    (args.output / "report.md").write_text(report(result))
    print(
        json.dumps(
            dict(
                output=str(args.output),
                paired_points=len(points),
                suites=result["suites"],
            ),
            ensure_ascii=False,
        )
    )


if __name__ == "__main__":
    main()
