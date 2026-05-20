#!/usr/bin/env python3

import json
import math
import re
from pathlib import Path


RUNS = {
    "all_base": Path("/nfs/home/share/gem5_ci/performance_data/gcc12-spec06-0.8c/20260401_125045_94e484f_run134"),
    "all_opt": Path("/nfs/home/share/gem5_ci/performance_data/gcc12-spec06-0.8c/20260401_125104_191cebc_run135"),
    "all_hash": Path("/nfs/home/share/gem5_ci/performance_data/gcc12-spec06-0.8c/20260401_191015_df2a322_run143"),
    "vbop_base": Path("/nfs/home/share/gem5_ci/performance_data/gcc12-spec06-0.8c/20260401_140142_93accab_run137"),
    "vbop_opt": Path("/nfs/home/share/gem5_ci/performance_data/gcc12-spec06-0.8c/20260401_140149_4088f34_run138"),
    "vbop_hash": Path("/nfs/home/share/gem5_ci/performance_data/gcc12-spec06-0.8c/20260401_174858_fc2c7c0_run141"),
    "pbop_base": Path("/nfs/home/share/gem5_ci/performance_data/gcc12-spec06-0.8c/20260401_151316_ed94f52_run139"),
    "pbop_opt": Path("/nfs/home/share/gem5_ci/performance_data/gcc12-spec06-0.8c/20260401_151242_cac779f_run140"),
    "pbop_hash": Path("/nfs/home/share/gem5_ci/performance_data/gcc12-spec06-0.8c/20260401_174921_e761286_run142"),
}

GROUPS = {
    "all": ("all_base", "all_opt"),
    "vbop": ("vbop_base", "vbop_opt"),
    "pbop": ("pbop_base", "pbop_opt"),
}

CLUSTER_JSON = Path(
    "/nfs/share/zyy/spec06_rv64gcb_O3_20m_gcc12.2.0-intFpcOff-jeMalloc/"
    "zstd-checkpoint-0-0-0/cluster-0-0.json"
)

INT_BENCHES = [
    "perlbench", "bzip2", "gcc", "mcf", "gobmk", "hmmer",
    "sjeng", "libquantum", "h264ref", "omnetpp", "astar", "xalancbmk",
]

STAT_KEYS = {
    "accuracy": "system.l2_wrappers.prefetcher.accuracy",
    "coverage": "system.l2_wrappers.prefetcher.coverage",
    "bop_issued": "system.l2_wrappers.prefetcher.pfIssued_srcs::4",
    "bop_useful": "system.l2_wrappers.prefetcher.pfUseful_srcs::4",
}

SCORE_RE = re.compile(r"^([A-Za-z0-9._-]+)\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)$")
STAT_RE = re.compile(r"^(\S+)\s+([-+0-9.eEinanfINFNaN]+)")


def pct(new, old):
    if old == 0:
        return None
    return (new - old) / old * 100.0


def parse_score_coverage(run_dir: Path):
    cov = {}
    in_int = False
    for line in (run_dir / "score.txt").read_text().splitlines():
        if line.strip() == "================ Int =================":
            in_int = True
            continue
        if line.startswith("Estimated Int score per GHz:"):
            in_int = False
        if not in_int:
            continue
        m = SCORE_RE.match(line.strip())
        if m and m.group(1) in INT_BENCHES:
            cov[m.group(1)] = float(m.group(5))
    return cov


def parse_stats(path: Path):
    vals = {}
    with path.open() as f:
        for line in f:
            m = STAT_RE.match(line.strip())
            if not m:
                continue
            key, val = m.group(1), m.group(2)
            if key in STAT_KEYS.values():
                vals[key] = float(val)
    return vals


def workload_point_from_dirname(dirname: str):
    workload, point = dirname.rsplit("_", 1)
    return workload, int(point)


def weighted_avg(entries):
    total_w = sum(w for w, _ in entries)
    if total_w == 0:
        return None
    return sum(w * v for w, v in entries) / total_w


def finite_or_none(x):
    if x is None:
        return None
    if isinstance(x, float) and not math.isfinite(x):
        return None
    return x


def main():
    with CLUSTER_JSON.open() as f:
        cluster = json.load(f)

    result = {"groups": {}}
    for group, (base_tag, opt_tag) in GROUPS.items():
        group_out = {}
        for tag in [base_tag, opt_tag]:
            run_dir = RUNS[tag]
            score_cov = parse_score_coverage(run_dir)
            tag_out = {}
            for bench in INT_BENCHES:
                bench_dirs = sorted((run_dir / "spec_all").glob(f"{bench}_*"))
                wl_map = {}
                for d in bench_dirs:
                    workload, point = workload_point_from_dirname(d.name)
                    if workload not in cluster:
                        continue
                    stats_path = d / "m5out" / "stats.txt"
                    stats = parse_stats(stats_path)
                    wl_map.setdefault(workload, {})[point] = stats

                wl_metrics = {}
                for workload, points in wl_map.items():
                    js = cluster[workload]
                    point_weights = js["points"]
                    acc_entries = []
                    cov_entries = []
                    bop_ratio_entries = []
                    for point, stats in points.items():
                        if str(point) not in point_weights:
                            continue
                        w = float(point_weights[str(point)])
                        if point == 0:
                            continue
                        if STAT_KEYS["accuracy"] in stats:
                            acc_entries.append((w, stats[STAT_KEYS["accuracy"]]))
                        if STAT_KEYS["coverage"] in stats:
                            cov_entries.append((w, stats[STAT_KEYS["coverage"]]))
                        issued = stats.get(STAT_KEYS["bop_issued"], 0.0)
                        useful = stats.get(STAT_KEYS["bop_useful"], 0.0)
                        ratio = useful / issued if issued > 0 else math.nan
                        if not math.isnan(ratio):
                            bop_ratio_entries.append((w, ratio))

                    wl_metrics[workload] = {
                        "insts": float(js["insts"]),
                        "accuracy": finite_or_none(weighted_avg(acc_entries)),
                        "coverage": finite_or_none(weighted_avg(cov_entries)),
                        "bop_accuracy": finite_or_none(weighted_avg(bop_ratio_entries)),
                    }

                if not wl_metrics:
                    continue

                inst_total = sum(v["insts"] for v in wl_metrics.values())
                def bench_metric(name):
                    entries = []
                    for workload, v in wl_metrics.items():
                        if v[name] is not None and not math.isnan(v[name]):
                            entries.append((v["insts"] / inst_total, v[name]))
                    return weighted_avg(entries)

                tag_out[bench] = {
                    "weighted_accuracy": bench_metric("accuracy"),
                    "weighted_coverage": bench_metric("coverage"),
                    "weighted_bop_accuracy": bench_metric("bop_accuracy"),
                    "score_coverage": score_cov.get(bench),
                }
            group_out[tag] = tag_out

        compare = {}
        for bench in INT_BENCHES:
            base = group_out[base_tag].get(bench, {})
            opt = group_out[opt_tag].get(bench, {})
            compare[bench] = {}
            for key in ["weighted_accuracy", "weighted_coverage", "weighted_bop_accuracy", "score_coverage"]:
                b = base.get(key)
                o = opt.get(key)
                compare[bench][key] = {
                    "base": b,
                    "opt": o,
                    "delta_pct": pct(o, b) if (b is not None and o is not None and not (math.isnan(b) or math.isnan(o))) else None,
                }
        result["groups"][group] = compare

    print(json.dumps(result, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
