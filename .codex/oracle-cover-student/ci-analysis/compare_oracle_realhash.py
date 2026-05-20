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

TRIPLETS = {
    "all": ("all_base", "all_opt", "all_hash"),
    "vbop": ("vbop_base", "vbop_opt", "vbop_hash"),
    "pbop": ("pbop_base", "pbop_opt", "pbop_hash"),
}

BENCHES = ["mcf", "omnetpp", "libquantum", "gcc", "xalancbmk"]
CLUSTER_JSON = Path(
    "/nfs/share/zyy/spec06_rv64gcb_O3_20m_gcc12.2.0-intFpcOff-jeMalloc/"
    "zstd-checkpoint-0-0-0/cluster-0-0.json"
)
REP_POINTS = {
    "all": {
        "mcf": "mcf_11392",
        "omnetpp": "omnetpp_16482",
        "libquantum": "libquantum_15361",
    },
    "vbop": {
        "mcf": "mcf_11392",
        "omnetpp": "omnetpp_20261",
        "libquantum": "libquantum_15361",
    },
    "pbop": {
        "mcf": "mcf_11392",
        "omnetpp": "omnetpp_19013",
        "libquantum": "libquantum_15361",
    },
}

SCORE_RE = re.compile(r"^([A-Za-z0-9._-]+)\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)$")
STAT_RE = re.compile(r"^(\S+)\s+([-+0-9.eEinanfINFNaN]+)")

REP_KEYS = [
    "system.cpu.ipc",
    "system.cpu.dcache.demandMisses::total",
    "system.cpu.dcache.demandAvgMissLatency::total",
    "system.cpu.dcache.demandAvgMshrMissLatency::total",
    "system.l2_wrappers.prefetcher.demandMshrMisses",
    "system.l2_wrappers.prefetcher.pfIssued_srcs::4",
    "system.l2_wrappers.prefetcher.pfUseful_srcs::4",
    "system.l2_wrappers.prefetcher.accuracy",
    "system.l2_wrappers.prefetcher.coverage",
    "system.l2_wrappers.prefetcher.bop_large.studentIssueCount",
    "system.l2_wrappers.prefetcher.bop_large.studentFallbackCount",
    "system.l2_wrappers.prefetcher.bop_large.studentCovRatioPctDist::mean",
    "system.l2_wrappers.prefetcher.bop_small.studentIssueCount",
    "system.l2_wrappers.prefetcher.bop_small.studentFallbackCount",
    "system.l2_wrappers.prefetcher.bop_small.studentCovRatioPctDist::mean",
]

WEIGHTED_KEYS = {
    "accuracy": "system.l2_wrappers.prefetcher.accuracy",
    "coverage": "system.l2_wrappers.prefetcher.coverage",
}


def pct(new, old):
    if old == 0:
        return None
    return (new - old) / old * 100.0


def retention(base, oracle, realized):
    oracle_gain = oracle - base
    realized_gain = realized - base
    if oracle_gain == 0:
        return None
    return realized_gain / oracle_gain


def parse_scores(run_dir: Path):
    out = {}
    int_score = None
    in_int = False
    for line in (run_dir / "score.txt").read_text().splitlines():
        if line.strip() == "================ Int =================":
            in_int = True
            continue
        if line.startswith("Estimated Int score per GHz:"):
            int_score = float(line.split(":")[-1].strip())
            in_int = False
        if not in_int:
            continue
        m = SCORE_RE.match(line.strip())
        if m:
            out[m.group(1)] = {
                "time": float(m.group(2)),
                "score": float(m.group(4)),
            }
    return out, int_score


def parse_stats(path: Path):
    vals = {}
    with open(path) as f:
        for line in f:
            m = STAT_RE.match(line.strip())
            if m and m.group(1) in REP_KEYS:
                vals[m.group(1)] = float(m.group(2))
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


def parse_weighted_metrics(run_dir: Path, cluster: dict, bench: str):
    bench_dirs = sorted((run_dir / "spec_all").glob(f"{bench}_*"))
    wl_map = {}
    for d in bench_dirs:
        workload, point = workload_point_from_dirname(d.name)
        if workload not in cluster:
            continue
        stats = {}
        with open(d / "m5out" / "stats.txt") as f:
            for line in f:
                m = STAT_RE.match(line.strip())
                if not m:
                    continue
                key, val = m.group(1), m.group(2)
                if key in WEIGHTED_KEYS.values():
                    stats[key] = float(val)
        wl_map.setdefault(workload, {})[point] = stats

    wl_metrics = {}
    for workload, points in wl_map.items():
        js = cluster[workload]
        point_weights = js["points"]
        acc_entries = []
        cov_entries = []
        for point, stats in points.items():
            if str(point) not in point_weights or point == 0:
                continue
            w = float(point_weights[str(point)])
            if WEIGHTED_KEYS["accuracy"] in stats and math.isfinite(stats[WEIGHTED_KEYS["accuracy"]]):
                acc_entries.append((w, stats[WEIGHTED_KEYS["accuracy"]]))
            if WEIGHTED_KEYS["coverage"] in stats and math.isfinite(stats[WEIGHTED_KEYS["coverage"]]):
                cov_entries.append((w, stats[WEIGHTED_KEYS["coverage"]]))
        wl_metrics[workload] = {
            "insts": float(js["insts"]),
            "accuracy": finite_or_none(weighted_avg(acc_entries)),
            "coverage": finite_or_none(weighted_avg(cov_entries)),
        }

    if not wl_metrics:
        return {"accuracy": None, "coverage": None}

    inst_total = sum(v["insts"] for v in wl_metrics.values())
    def bench_metric(name):
        entries = []
        for workload, v in wl_metrics.items():
            if v[name] is not None:
                entries.append((v["insts"] / inst_total, v[name]))
        return finite_or_none(weighted_avg(entries))

    return {
        "accuracy": bench_metric("accuracy"),
        "coverage": bench_metric("coverage"),
    }


def main():
    with CLUSTER_JSON.open() as f:
        cluster = json.load(f)
    out = {"triplets": {}}
    for name, (base_tag, oracle_tag, hash_tag) in TRIPLETS.items():
        base_scores, base_int = parse_scores(RUNS[base_tag])
        oracle_scores, oracle_int = parse_scores(RUNS[oracle_tag])
        hash_scores, hash_int = parse_scores(RUNS[hash_tag])

        t = {
            "overall": {
                "base": base_int,
                "oracle": oracle_int,
                "hash": hash_int,
                "oracle_gain_pct": pct(oracle_int, base_int),
                "hash_gain_pct": pct(hash_int, base_int),
                "hash_vs_oracle_pct": pct(hash_int, oracle_int),
                "retention_ratio": retention(base_int, oracle_int, hash_int),
            },
            "benchmarks": {},
            "representative_points": {},
        }

        for bench in BENCHES:
            bs = base_scores[bench]["score"]
            os = oracle_scores[bench]["score"]
            hs = hash_scores[bench]["score"]
            base_w = parse_weighted_metrics(RUNS[base_tag], cluster, bench)
            oracle_w = parse_weighted_metrics(RUNS[oracle_tag], cluster, bench)
            hash_w = parse_weighted_metrics(RUNS[hash_tag], cluster, bench)
            t["benchmarks"][bench] = {
                "score": {
                    "base": bs,
                    "oracle": os,
                    "hash": hs,
                    "oracle_gain_pct": pct(os, bs),
                    "hash_gain_pct": pct(hs, bs),
                    "hash_vs_oracle_pct": pct(hs, os),
                    "retention_ratio": retention(bs, os, hs),
                },
                "weighted_coverage": {
                    "base": base_w["coverage"],
                    "oracle": oracle_w["coverage"],
                    "hash": hash_w["coverage"],
                    "oracle_gain_pct": pct(oracle_w["coverage"], base_w["coverage"]),
                    "hash_gain_pct": pct(hash_w["coverage"], base_w["coverage"]),
                    "hash_vs_oracle_pct": pct(hash_w["coverage"], oracle_w["coverage"]),
                    "retention_ratio": retention(base_w["coverage"], oracle_w["coverage"], hash_w["coverage"])
                    if None not in (base_w["coverage"], oracle_w["coverage"], hash_w["coverage"]) else None,
                },
                "weighted_accuracy": {
                    "base": base_w["accuracy"],
                    "oracle": oracle_w["accuracy"],
                    "hash": hash_w["accuracy"],
                    "oracle_gain_pct": pct(oracle_w["accuracy"], base_w["accuracy"])
                    if None not in (base_w["accuracy"], oracle_w["accuracy"]) else None,
                    "hash_gain_pct": pct(hash_w["accuracy"], base_w["accuracy"])
                    if None not in (base_w["accuracy"], hash_w["accuracy"]) else None,
                    "hash_vs_oracle_pct": pct(hash_w["accuracy"], oracle_w["accuracy"])
                    if None not in (oracle_w["accuracy"], hash_w["accuracy"]) else None,
                    "retention_ratio": retention(base_w["accuracy"], oracle_w["accuracy"], hash_w["accuracy"])
                    if None not in (base_w["accuracy"], oracle_w["accuracy"], hash_w["accuracy"]) else None,
                },
            }

        # read weighted coverage/accuracy for hash runs by tag-level recomputation file
        # weighted_pf_metrics.json only stores base->opt groups; for hash we recompute ad hoc below.
        # parse representative point stats
        for bench, point_dir in REP_POINTS[name].items():
            rep = {}
            for tag in [base_tag, oracle_tag, hash_tag]:
                stats = parse_stats(RUNS[tag] / "spec_all" / point_dir / "m5out" / "stats.txt")
                rep[tag] = stats
            t["representative_points"][bench] = rep
        out["triplets"][name] = t

    print(json.dumps(out, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
