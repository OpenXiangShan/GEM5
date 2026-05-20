#!/usr/bin/env python3

from __future__ import annotations

import csv
import importlib.util
import json
import sys
from collections import Counter
from pathlib import Path


RATIO_EPS_LIST = [0.0, 0.001, 0.002, 0.005, 0.01, 0.015, 0.02, 0.03, 0.05]


def load_helper_module(script_path: Path):
    spec = importlib.util.spec_from_file_location("libq_delay", script_path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def choose_largest_near_tie(cov: dict[int, int], phase_len: int, ratio_eps: float) -> tuple[int, int]:
    best_cov = max(cov.values())
    near = [
        off for off, value in cov.items()
        if (best_cov - value) / phase_len <= ratio_eps + 1e-12
    ]
    pick = max(near, key=lambda off: (abs(off), off))
    return pick, best_cov


def main() -> None:
    repo = Path(__file__).resolve().parents[3]
    helper = load_helper_module(Path(__file__).resolve().parent / "analyze_libq_student_delay.py")

    cfg = helper.load_config(
        repo / "test/libq_15361_nol2NL_studentBop/config.ini",
        "system.l2_wrappers.prefetcher.bop_large",
    )
    rows = helper.load_train_rows(
        repo / "test/libq_15361_nol2NL_studentBop/bop_trace.db",
        "vbopTrainTraceTable",
    )
    phases = helper.simulate_teacher_phases(rows, cfg)

    candidate_sets = {
        "runtime_issue_support": [45, 81, 91],
        "teacher_support_from_stats": [
            45, 48, 54, 60, 64, 72, 75, 80, 81, 90, 91,
            96, 108, 125, 135, 147, 150, 192, 200, 216, 225, 240,
        ],
        "all_positive_offsets": [off for off in cfg.offsets if off > 0],
    }

    phase_cov_by_set: dict[str, list[dict]] = {}
    for set_name, cands in candidate_sets.items():
        per_phase = []
        for phase in phases:
            cov = {}
            for off in cands:
                cov[off] = helper.simulate_phase_cov(
                    phase.rows, [helper.StudentEntry(offset=off)], cfg, student_delay_ticks=0
                )[off]
            per_phase.append(
                {
                    "phase_idx": phase.phase_idx,
                    "phase_len": len(phase.rows),
                    "cov": cov,
                }
            )
        phase_cov_by_set[set_name] = per_phase

    summary = {
        "db_path": str(repo / "test/libq_15361_nol2NL_studentBop/bop_trace.db"),
        "phase_count": len(phases),
        "ratio_eps_list": RATIO_EPS_LIST,
        "candidate_sets": {},
    }

    rows_for_csv = []
    for set_name, phase_covs in phase_cov_by_set.items():
        set_summary = {}
        for ratio_eps in RATIO_EPS_LIST:
            winner_counter = Counter()
            changed_counter = 0
            for phase_info in phase_covs:
                phase_len = phase_info["phase_len"]
                cov = phase_info["cov"]
                base_pick, _ = choose_largest_near_tie(cov, phase_len, 0.0)
                pick, best_cov = choose_largest_near_tie(cov, phase_len, ratio_eps)
                winner_counter[pick] += 1
                if pick != base_pick:
                    changed_counter += 1

                rows_for_csv.append(
                    [
                        set_name,
                        ratio_eps,
                        phase_info["phase_idx"],
                        phase_len,
                        base_pick,
                        pick,
                        best_cov,
                        cov[pick],
                        (best_cov - cov[pick]) / phase_len,
                    ]
                )

            total = sum(winner_counter.values())
            mean_abs = sum(abs(off) * cnt for off, cnt in winner_counter.items()) / total
            set_summary[str(ratio_eps)] = {
                "winner_counts": dict(sorted(winner_counter.items())),
                "avg_abs_winner": mean_abs,
                "changed_phase_count_vs_eps0": changed_counter,
            }
        summary["candidate_sets"][set_name] = set_summary

    outdir = Path(__file__).resolve().parent
    summary_path = outdir / "near_tie_prefer_large_summary.json"
    phase_path = outdir / "near_tie_prefer_large_phase_winners.csv"
    summary_path.write_text(json.dumps(summary, indent=2, sort_keys=True))

    with phase_path.open("w", newline="") as fh:
        writer = csv.writer(fh)
        writer.writerow(
            [
                "candidate_set",
                "ratio_eps",
                "phase_idx",
                "phase_len",
                "winner_eps0",
                "winner_eps",
                "best_cov",
                "picked_cov",
                "relative_gap_to_best",
            ]
        )
        writer.writerows(rows_for_csv)

    print(json.dumps(summary, indent=2, sort_keys=True))
    print("wrote", summary_path)
    print("wrote", phase_path)


if __name__ == "__main__":
    main()
