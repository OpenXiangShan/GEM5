#!/usr/bin/env python3
"""Compare short instruction windows against paired full-slice analyses."""

import argparse
import csv
import html
import json
import math
from pathlib import Path


from analyze import read_config, read_stats


PC_KEY = ("cache", "context_id", "address_space", "secure", "pc")


def read_json(path):
    with path.open() as stream:
        return json.load(stream)


def load_window(root, spec, reference):
    stats_only = "stats_pair" in spec
    if stats_only == ("report" in spec):
        raise ValueError("each window requires either report or stats_pair")
    source = spec["stats_pair"]["candidate"] if stats_only else spec["report"]
    folder = (root / source).resolve()
    start, end, warmup = (spec[k] for k in ("start", "end", "warmup"))
    if any(type(x) is not int for x in (start, end, warmup)) or not (
        0 <= warmup <= start < end
    ):
        raise ValueError(f"{folder}: invalid checkpoint-relative instruction range")

    if stats_only:
        ref_folder = (root / spec["stats_pair"][reference]).resolve()
        if ref_folder == folder:
            raise ValueError("candidate and reference must be distinct runs")
        configs = []
        runs = []
        for directory in (ref_folder, folder):
            config = directory / "config.ini"
            if not config.is_file():
                raise ValueError(f"{directory}: missing config.ini")
            configs.append(read_config(config))
            runs.append({"stats": read_stats(directory / "stats.txt",
                                             spec.get("cpu", "system.cpu"))})
        if configs[0] != configs[1]:
            raise ValueError(f"{folder}: non-prefetch config differs")
        ref, candidate = runs
    else:
        candidate = read_json(folder / "summary.json")
        ref = read_json(folder / reference / "summary.json")
        key = "versus_original" if reference == "control" else "versus_prefetch_off"
        if not candidate[key]["paired"]:
            raise ValueError(f"{folder}: {candidate[key]['reason']}")
    cpis = []
    for run in (ref, candidate):
        stats = run["stats"]
        if stats["retired_instructions"] != end - start:
            raise ValueError(f"{folder}: retired count differs from declared ROI length")
        cycles = stats["cycles"]
        if not cycles or not math.isfinite(cycles) or cycles <= 0:
            raise ValueError(f"{folder}: missing positive cycle count")
        cpis.append(cycles / (end - start))

    diagnostics = candidate.get("paired_diagnostics", {})
    evidence = diagnostics.get("reason")
    if diagnostics.get("reference") != reference:
        evidence = "缺少指定参考组的配对诊断"
    misses, pcs = {}, {}
    if not evidence:
        misses = {r["metric"]: r["delta_per_ki"] for r in diagnostics["rows"]
                  if r["group"] == "demand_miss"}
        with (folder / "pc-comparison.csv").open(newline="") as stream:
            for row in csv.DictReader(stream):
                if not row["pc"]:
                    continue
                delta = float(row["weighted_wait_delta_ticks_per_inst"]) / candidate["weight"]
                if delta:
                    pcs[tuple(row[k] for k in PC_KEY)] = delta
    audit = [r.get("counter_reconciliation", {}).get("status", "stats_only")
             for r in (ref, candidate)]
    return dict(report=source, start=start, end=end, warmup=warmup,
                before_cpi=cpis[0], after_cpi=cpis[1],
                speedup_pct=100 * (cpis[0] / cpis[1] - 1),
                evidence=evidence, audit=audit), misses, pcs


def pc_comparison(short, full, limit):
    def top(values):
        return sorted(values, key=lambda k: (-abs(values[k]), k))[:limit]

    a, b = top(short), top(full)
    shared = set(a) & set(b)
    rows = [
        {**dict(zip(PC_KEY, k)), "short": short.get(k), "full": full.get(k),
         "short_top": k in a, "full_top": k in b}
        for k in dict.fromkeys(a + b)
    ]
    return dict(short_count=len(a), full_count=len(b), shared=len(shared),
                same_sign=sum(short[k] * full[k] > 0 for k in shared), rows=rows)


def compare_manifest(path, limit=10):
    spec = read_json(path)
    reference = spec["reference"]
    if reference not in ("control", "baseline") or limit < 1:
        raise ValueError("choose control or baseline and positive top-k")
    seen, reports, points = set(), set(), []
    totals = [0., 0.]
    for point in spec["slices"]:
        checkpoint, weight = point["checkpoint"], point["weight"]
        if not checkpoint or checkpoint in seen or not math.isfinite(weight) or not 0 < weight <= 1:
            raise ValueError(f"duplicate checkpoint or invalid profile weight: {checkpoint}")
        seen.add(checkpoint)

        def read(window):
            source = window.get("report") or window["stats_pair"]["candidate"]
            directory = (path.parent / source).resolve()
            if directory in reports:
                raise ValueError(f"report reused: {directory}")
            reports.add(directory)
            return load_window(path.parent, window, reference)

        full, full_misses, full_pcs = read(point["full"])
        totals[0] += weight * full["before_cpi"]
        totals[1] += weight * full["after_cpi"]
        windows = []
        for window in point["short"]:
            short, misses, pcs = read(window)
            if not (full["start"] <= short["start"] < short["end"] <= full["end"]
                    and short["end"] - short["start"] < full["end"] - full["start"]):
                raise ValueError(f"{short['report']}: expected a strict subwindow of full ROI")
            a, b = short["speedup_pct"], full["speedup_pct"]
            short["direction_agrees"] = (a > 0) - (a < 0) == (b > 0) - (b < 0)
            short["speedup_gap_pp"] = a - b
            if not (short["evidence"] or full["evidence"]):
                short["miss_mpki"] = [
                    dict(reason=k, short=misses.get(k, 0), full=full_misses.get(k, 0))
                    for k in sorted(misses.keys() | full_misses.keys(),
                                    key=lambda k: (-abs(full_misses.get(k, 0)), k))
                ]
                short["hot_pcs"] = pc_comparison(pcs, full_pcs, limit)
            windows.append(short)
        points.append(dict(checkpoint=checkpoint, weight=weight, full=full, short=windows))
    coverage = math.fsum(p["weight"] for p in points)
    if not points or coverage > 1 + 1e-8:
        raise ValueError("empty profile or total weight exceeds one")
    complete = math.isclose(coverage, 1, abs_tol=1e-8)
    return dict(benchmark=spec["benchmark"], experiment=spec["experiment"],
                reference=reference, top_k=limit, coverage=coverage, slices=points,
                benchmark_speedup_pct=100 * (totals[0] / totals[1] - 1) if complete else None)


def render(result):
    esc = lambda value: html.escape(str(value))
    def table(labels, rows):
        header = "".join(f"<th>{esc(label)}</th>" for label in labels)
        body = "".join("<tr>" + "".join(f"<td>{esc(v)}</td>" for v in row)
                       + "</tr>" for row in rows)
        return f"<table><tr>{header}</tr>{body}</table>"

    gain = result["benchmark_speedup_pct"]
    score = "权重未覆盖完整 profile" if gain is None else f"完整切片加权收益 {gain:+.3f}%"
    page = ['<!doctype html><meta charset="utf-8"><title>预取短窗验证</title>',
            '<style>body{font:16px system-ui;max-width:1050px;margin:32px auto}table{border-collapse:collapse}td,th{padding:5px 10px;border-bottom:1px solid #ccc;text-align:left}</style>',
            f"<h1>{esc(result['benchmark'])} · {esc(result['experiment'])}</h1>",
            f"<p>参考组 {esc(result['reference'])} · 切片权重覆盖 {result['coverage']:.2%} · {score}</p>"]
    for point in result["slices"]:
        f = point["full"]
        page.append(f"<h2>{esc(point['checkpoint'])}</h2><p>完整切片 [{f['start']}, {f['end']})，预热 {f['warmup']} 条，IPC 变化 {f['speedup_pct']:+.3f}%，权重 {point['weight']:g}。</p>")
        for s in point["short"]:
            direction = "同向" if s["direction_agrees"] else "方向不一致"
            page.append(f"<h3>短窗 [{s['start']}, {s['end']}) · {direction}</h3><p>预热 {s['warmup']} 条，IPC 变化 {s['speedup_pct']:+.3f}%，较完整切片相差 {s['speedup_gap_pp']:+.3f} 个百分点。</p>")
            page.append(f"<p>原生计数核对（参考组 / 候选组）：短窗 {esc(' / '.join(s['audit']))}，完整切片 {esc(' / '.join(f['audit']))}。</p>")
            if reason := s["evidence"] or f["evidence"]:
                page.append(f"<p>机制证据缺失：{esc(reason)}</p>")
                continue
            page.append(table(["残余 Miss 原因", "短窗 ΔMPKI", "完整切片 ΔMPKI"],
                              [(r["reason"], f"{r['short']:+.4f}", f"{r['full']:+.4f}") for r in s["miss_mpki"]]))
            pcs = s["hot_pcs"]
            page.append(f"<p>等待变化热点（按绝对值）：短窗 {pcs['short_count']} 个，完整切片 {pcs['full_count']} 个，重合 {pcs['shared']} 个，其中 {pcs['same_sign']} 个变化同向。单位 tick / 退休指令。</p>")
            def fmt(value):
                return "—" if value is None else f"{value:+.4g}"
            page.append(table(["PC", "Cache", "Context", "地址空间", "Secure", "短窗", "完整切片"],
                              [(r["pc"], r["cache"], r["context_id"], r["address_space"], r["secure"],
                                fmt(r["short"]), fmt(r["full"])) for r in pcs["rows"]]))
    return "\n".join(page)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--top-k", default=10, type=int)
    args = parser.parse_args()
    try:
        result = compare_manifest(args.manifest, args.top_k)
        page = render(result)
        args.output.mkdir(parents=True, exist_ok=False)
        (args.output / "windows.html").write_text(page, encoding="utf-8")
        (args.output / "windows.json").write_text(json.dumps(result, indent=2) + "\n")
    except (ValueError, KeyError, OSError, TypeError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
