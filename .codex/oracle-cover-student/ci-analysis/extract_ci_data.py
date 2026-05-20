#!/usr/bin/env python3

import json
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

INT_BENCHES = [
    "perlbench", "bzip2", "gcc", "mcf", "gobmk", "hmmer",
    "sjeng", "libquantum", "h264ref", "omnetpp", "astar", "xalancbmk",
]

GLOBAL_COUNTERS = [
    "system.cpu.ipc",
    "system.cpu.dcache.demandMisses::total",
    "system.cpu.dcache.demandMissRate::total",
    "system.cpu.dcache.demandMshrMisses::total",
    "system.l2_wrappers.prefetcher.demandMshrMisses",
    "system.l2_wrappers.prefetcher.pfIssued",
    "system.l2_wrappers.prefetcher.pfUseful",
    "system.l2_wrappers.prefetcher.pfIssued_srcs::4",
    "system.l2_wrappers.prefetcher.pfUseful_srcs::4",
    "system.l2_wrappers.prefetcher.accuracy",
    "system.l2_wrappers.prefetcher.coverage",
]

STUDENT_COUNTERS = [
    "system.l2_wrappers.prefetcher.bop_large.teacherInjectedCount",
    "system.l2_wrappers.prefetcher.bop_large.studentPhaseCount",
    "system.l2_wrappers.prefetcher.bop_large.studentIssueCount",
    "system.l2_wrappers.prefetcher.bop_large.studentFallbackCount",
    "system.l2_wrappers.prefetcher.bop_large.studentCovRatioPctDist::mean",
    "system.l2_wrappers.prefetcher.bop_small.teacherInjectedCount",
    "system.l2_wrappers.prefetcher.bop_small.studentPhaseCount",
    "system.l2_wrappers.prefetcher.bop_small.studentIssueCount",
    "system.l2_wrappers.prefetcher.bop_small.studentFallbackCount",
    "system.l2_wrappers.prefetcher.bop_small.studentCovRatioPctDist::mean",
]

SCORE_RE = re.compile(r"^([A-Za-z0-9]+)\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)$")
STAT_RE = re.compile(r"^(\S+)\s+([-+0-9.eEinanfINFNaN]+)")


def parse_score(path: Path):
    scores = {}
    int_score = None
    lines = path.read_text().splitlines()
    in_int = False
    for line in lines:
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
            bench = m.group(1)
            if bench in INT_BENCHES:
                scores[bench] = {
                    "time": float(m.group(2)),
                    "ref_time": float(m.group(3)),
                    "score": float(m.group(4)),
                    "coverage": float(m.group(5)),
                }
    return scores, int_score


def parse_stats(path: Path, wanted):
    out = {}
    with path.open() as f:
        for line in f:
            m = STAT_RE.match(line.strip())
            if not m:
                continue
            key, val = m.group(1), m.group(2)
            if key in wanted:
                try:
                    out[key] = float(val)
                except ValueError:
                    if val.lower() == "nan":
                        out[key] = float("nan")
    return out


def find_stats(run_dir: Path, bench: str) -> Path:
    matches = sorted((run_dir / "spec_all").glob(f"{bench}_*/m5out/stats.txt"))
    if not matches:
        raise FileNotFoundError(f"stats not found for {bench} in {run_dir}")
    return matches[0]


def pct(new, old):
    if old == 0:
        return None
    return (new - old) / old * 100.0


def main():
    run_scores = {}
    run_int_score = {}
    for tag, run_dir in RUNS.items():
        scores, int_score = parse_score(run_dir / "score.txt")
        run_scores[tag] = scores
        run_int_score[tag] = int_score

    result = {"runs": {}, "groups": {}}
    for tag, run_dir in RUNS.items():
        result["runs"][tag] = {
            "path": str(run_dir),
            "int_score": run_int_score[tag],
            "scores": run_scores[tag],
        }

    rep_benches = sorted(set(INT_BENCHES + ["mcf", "omnetpp", "libquantum", "gcc", "xalancbmk"]))
    wanted = set(GLOBAL_COUNTERS + STUDENT_COUNTERS)

    for group, (base_tag, opt_tag) in GROUPS.items():
        group_out = {
            "base": base_tag,
            "opt": opt_tag,
            "int_score_base": run_int_score[base_tag],
            "int_score_opt": run_int_score[opt_tag],
            "int_score_delta_pct": pct(run_int_score[opt_tag], run_int_score[base_tag]),
            "benchmarks": {},
        }
        for bench in INT_BENCHES:
            b = run_scores[base_tag][bench]["score"]
            o = run_scores[opt_tag][bench]["score"]
            group_out["benchmarks"][bench] = {
                "base_score": b,
                "opt_score": o,
                "delta_pct": pct(o, b),
                "base_time": run_scores[base_tag][bench]["time"],
                "opt_time": run_scores[opt_tag][bench]["time"],
            }

        for bench in rep_benches:
            base_stats = parse_stats(find_stats(RUNS[base_tag], bench), wanted)
            opt_stats = parse_stats(find_stats(RUNS[opt_tag], bench), wanted)
            group_out["benchmarks"][bench]["stats"] = {}
            for key in sorted(wanted):
                if key in base_stats or key in opt_stats:
                    b = base_stats.get(key)
                    o = opt_stats.get(key)
                    group_out["benchmarks"][bench]["stats"][key] = {
                        "base": b,
                        "opt": o,
                        "delta_pct": pct(o, b) if (b is not None and o is not None and not (b != b or o != o)) else None,
                    }

        gains = sorted(
            ((bench, info["delta_pct"]) for bench, info in group_out["benchmarks"].items()),
            key=lambda x: x[1] if x[1] is not None else -1e9,
            reverse=True,
        )
        group_out["top_gains"] = gains[:5]
        group_out["top_regressions"] = list(reversed(gains[-5:]))
        result["groups"][group] = group_out

    print(json.dumps(result, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
