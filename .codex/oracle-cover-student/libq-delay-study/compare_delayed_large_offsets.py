#!/usr/bin/env python3

from __future__ import annotations

import csv
import importlib.util
import json
import sys
from collections import Counter, deque
from pathlib import Path


FOCUS_OFFSETS = [144, 160, 162, 240]


def load_helper_module(script_path: Path):
    spec = importlib.util.spec_from_file_location("libq_delay", script_path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def exact_delayed_cov(rows, offset: int, block_size: int, delay_ticks: int, page_bytes: int, cross_page: bool) -> int:
    ready = set()
    pending = deque()
    cov = 0

    for _, tick, addr in rows:
        while pending and pending[0][0] <= tick:
            _, pred_addr = pending.popleft()
            ready.add(pred_addr)

        if addr in ready:
            cov += 1

        pred_addr = addr + offset * block_size
        if pred_addr < 0:
            continue
        if (not cross_page) and ((addr // page_bytes) != (pred_addr // page_bytes)):
            continue
        pending.append((tick + delay_ticks, pred_addr))

    return cov


def summarize_focus(records, focus_offsets):
    summary = {}
    for off in focus_offsets:
        off_rows = [row for row in records if row["focus_off"] == off]
        summary[str(off)] = {
            "avg_cov_ratio": sum(row["focus_cov_ratio"] for row in off_rows) / len(off_rows),
            "avg_abs_gap_to_best": sum(row["abs_gap"] for row in off_rows) / len(off_rows),
            "avg_ratio_gap_to_best": sum(row["ratio_gap"] for row in off_rows) / len(off_rows),
            "min_abs_gap_to_best": min(row["abs_gap"] for row in off_rows),
            "max_abs_gap_to_best": max(row["abs_gap"] for row in off_rows),
            "phases_within_0p5pct": sum(1 for row in off_rows if row["ratio_gap"] <= 0.005),
            "phases_within_1pct": sum(1 for row in off_rows if row["ratio_gap"] <= 0.01),
            "phases_within_2pct": sum(1 for row in off_rows if row["ratio_gap"] <= 0.02),
        }
    return summary


def main() -> None:
    repo = Path(__file__).resolve().parents[3]
    outdir = Path(__file__).resolve().parent
    helper = load_helper_module(outdir / "analyze_libq_student_delay.py")

    cfg = helper.load_config(
        repo / "test/libq_15361_nol2NL_studentBop/config.ini",
        "system.l2_wrappers.prefetcher.bop_large",
    )
    rows = helper.load_train_rows(
        repo / "test/libq_15361_nol2NL_studentBop/bop_trace.db",
        "vbopTrainTraceTable",
    )
    phases = helper.simulate_teacher_phases(rows, cfg)
    all_positive = [off for off in cfg.offsets if off > 0]

    exact_records = []
    dm_records = []
    exact_best_counts = Counter()
    dm_best_counts = Counter()

    for phase in phases:
        exact_cov = {
            off: exact_delayed_cov(
                phase.rows,
                off,
                cfg.block_size,
                cfg.teacher_delay_ticks,
                cfg.page_bytes,
                cfg.cross_page,
            )
            for off in all_positive
        }
        dm_cov = {
            off: helper.simulate_phase_cov(
                phase.rows,
                [helper.StudentEntry(offset=off)],
                cfg,
                cfg.teacher_delay_ticks,
            )[off]
            for off in all_positive
        }

        exact_best = max(all_positive, key=lambda off: (exact_cov[off], -abs(off), -off))
        dm_best = max(all_positive, key=lambda off: (dm_cov[off], -abs(off), -off))
        exact_best_counts[exact_best] += 1
        dm_best_counts[dm_best] += 1

        phase_len = len(phase.rows)
        for off in FOCUS_OFFSETS:
            e_cov = exact_cov[off]
            d_cov = dm_cov[off]
            exact_records.append(
                {
                    "phase_idx": phase.phase_idx,
                    "phase_len": phase_len,
                    "best_off": exact_best,
                    "best_cov": exact_cov[exact_best],
                    "best_cov_ratio": exact_cov[exact_best] / phase_len,
                    "focus_off": off,
                    "focus_cov": e_cov,
                    "focus_cov_ratio": e_cov / phase_len,
                    "abs_gap": exact_cov[exact_best] - e_cov,
                    "ratio_gap": (exact_cov[exact_best] - e_cov) / phase_len,
                }
            )
            dm_records.append(
                {
                    "phase_idx": phase.phase_idx,
                    "phase_len": phase_len,
                    "best_off": dm_best,
                    "best_cov": dm_cov[dm_best],
                    "best_cov_ratio": dm_cov[dm_best] / phase_len,
                    "focus_off": off,
                    "focus_cov": d_cov,
                    "focus_cov_ratio": d_cov / phase_len,
                    "abs_gap": dm_cov[dm_best] - d_cov,
                    "ratio_gap": (dm_cov[dm_best] - d_cov) / phase_len,
                }
            )

    summary = {
        "phase_count": len(phases),
        "delay_cycles": int(cfg.teacher_delay_ticks / cfg.ticks_per_cycle),
        "delay_ticks": cfg.teacher_delay_ticks,
        "focus_offsets": FOCUS_OFFSETS,
        "exact_delayed": {
            "best_offset_counts": dict(sorted(exact_best_counts.items())),
            "per_offset": summarize_focus(exact_records, FOCUS_OFFSETS),
        },
        "dm_delayed": {
            "best_offset_counts": dict(sorted(dm_best_counts.items())),
            "per_offset": summarize_focus(dm_records, FOCUS_OFFSETS),
        },
    }

    exact_csv = outdir / "large_offset_exact_delayed_vs_best.csv"
    dm_csv = outdir / "large_offset_dm_delayed_vs_best.csv"
    summary_json = outdir / "large_offset_delayed_vs_best_summary.json"

    for path, records in [(exact_csv, exact_records), (dm_csv, dm_records)]:
        with path.open("w", newline="") as fh:
            writer = csv.writer(fh)
            writer.writerow(
                [
                    "phase_idx",
                    "phase_len",
                    "best_off",
                    "best_cov",
                    "best_cov_ratio",
                    "focus_off",
                    "focus_cov",
                    "focus_cov_ratio",
                    "abs_gap",
                    "ratio_gap",
                ]
            )
            for row in records:
                writer.writerow(
                    [
                        row["phase_idx"],
                        row["phase_len"],
                        row["best_off"],
                        row["best_cov"],
                        f"{row['best_cov_ratio']:.6f}",
                        row["focus_off"],
                        row["focus_cov"],
                        f"{row['focus_cov_ratio']:.6f}",
                        row["abs_gap"],
                        f"{row['ratio_gap']:.6f}",
                    ]
                )

    summary_json.write_text(json.dumps(summary, indent=2, sort_keys=True))
    print(json.dumps(summary, indent=2, sort_keys=True))
    print("wrote", exact_csv)
    print("wrote", dm_csv)
    print("wrote", summary_json)


if __name__ == "__main__":
    main()
