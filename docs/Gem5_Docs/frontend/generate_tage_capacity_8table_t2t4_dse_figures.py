#!/usr/bin/env python3
"""Render the capacity-versus-score projection for solver run 32253821540."""

import argparse
import csv
import json
from dataclasses import dataclass
from pathlib import Path


BASELINE_CAPACITY_BITS = 589_824
SCORE_COLUMN = "max:score_txt:Estimated Int score per GHz"
BRANCH_MISPREDICT_COLUMN = "min:stats:system.cpu.iew.branchMispredicts"
EXPECTED_VALID_TRIALS = 830
EXPECTED_AREA_SCORE_FRONTIER = 18

BLUE = "#2166ac"
GRAY = "#8f969d"
RED = "#c53030"


@dataclass(frozen=True)
class Trial:
    trial_id: str
    is_baseline: bool
    score: float
    branch_mispredicts: float
    capacity_bits: int

    @property
    def capacity_kib(self) -> float:
        return self.capacity_bits / 8192.0


def decode_capacity_bits(encoded: str) -> int:
    values = json.loads(encoded)
    if len(values) != 24:
        raise ValueError(f"expected 24 fixed-8-table values, got {len(values)}")
    table_sizes = values[:8]
    tag_bits = values[8:16]
    num_ways = values[16:]
    return sum(
        sets * ways * (tag_bits + 5)
        for sets, tag_bits, ways in zip(table_sizes, tag_bits, num_ways)
    )


def read_trials(artifact_dir: Path) -> list[Trial]:
    trials = []
    with (artifact_dir / "history.csv").open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            if row["status"] != "valid":
                continue
            is_baseline = row["is_baseline"] == "True"
            trials.append(
                Trial(
                    trial_id=row["trial_id"],
                    is_baseline=is_baseline,
                    score=float(row[SCORE_COLUMN]),
                    branch_mispredicts=float(row[BRANCH_MISPREDICT_COLUMN]),
                    capacity_bits=(
                        BASELINE_CAPACITY_BITS
                        if is_baseline
                        else decode_capacity_bits(row["tageConfig"])
                    ),
                )
            )

    if len(trials) != EXPECTED_VALID_TRIALS:
        raise ValueError(f"expected {EXPECTED_VALID_TRIALS} valid trials")
    if sum(trial.is_baseline for trial in trials) != 1:
        raise ValueError("expected exactly one config-default baseline")
    return trials


def area_score_frontier(trials: list[Trial]) -> list[Trial]:
    frontier = []
    for candidate in trials:
        if candidate.is_baseline:
            continue
        dominated = any(
            other is not candidate
            and other.capacity_bits <= candidate.capacity_bits
            and other.score >= candidate.score
            and (
                other.capacity_bits < candidate.capacity_bits
                or other.score > candidate.score
            )
            for other in trials
        )
        if not dominated:
            frontier.append(candidate)
    return sorted(frontier, key=lambda trial: trial.capacity_bits)


def plot(trials: list[Trial], output_path: Path) -> None:
    # Keep data auditing usable on hosts that do not have matplotlib installed.
    import matplotlib.pyplot as plt

    baseline = next(trial for trial in trials if trial.is_baseline)
    candidates = [trial for trial in trials if not trial.is_baseline]
    frontier = area_score_frontier(trials)
    if len(frontier) != EXPECTED_AREA_SCORE_FRONTIER:
        raise ValueError(
            "capacity-score frontier differs from the audited 18-point result"
        )
    best = max(frontier, key=lambda trial: trial.score)
    if best.trial_id != "trial_0748":
        raise ValueError("the audited best-score point is trial_0748")

    fig, ax = plt.subplots(figsize=(14.8, 8.7), layout="constrained")
    fig.patch.set_facecolor("white")
    ax.set_facecolor("#fcfcfc")
    ax.grid(color="#d7dce1", linewidth=0.9, alpha=0.95)
    for spine in ax.spines.values():
        spine.set_color("#4a4f55")
        spine.set_linewidth(1.1)

    ax.scatter(
        [trial.capacity_kib for trial in candidates],
        [trial.score for trial in candidates],
        s=42,
        color=GRAY,
        alpha=0.62,
        edgecolors="white",
        linewidths=0.35,
        label="Trials (n=829)",
        zorder=2,
    )
    ax.scatter(
        baseline.capacity_kib,
        baseline.score,
        s=124,
        color=BLUE,
        edgecolors="white",
        linewidths=1.0,
        label="Baseline (trial_0001)",
        zorder=5,
    )
    ax.plot(
        [trial.capacity_kib for trial in frontier],
        [trial.score for trial in frontier],
        color=RED,
        linewidth=1.8,
        alpha=0.9,
        zorder=6,
    )
    ax.scatter(
        [trial.capacity_kib for trial in frontier],
        [trial.score for trial in frontier],
        s=72,
        color=RED,
        edgecolors="white",
        linewidths=0.75,
        label=f"Area-score Pareto frontier (n={len(frontier)})",
        zorder=7,
    )
    ax.scatter(
        best.capacity_kib,
        best.score,
        s=390,
        marker="*",
        color=RED,
        edgecolors="#7f1d1d",
        linewidths=0.9,
        label="Best score (trial_0748)",
        zorder=8,
    )
    ax.annotate(
        "trial_0748\n27.336190 (+0.713%)",
        xy=(best.capacity_kib, best.score),
        xytext=(-16, -46),
        textcoords="offset points",
        ha="right",
        color="#7f1d1d",
        fontsize=10,
        weight="bold",
        arrowprops={"arrowstyle": "-", "color": RED, "lw": 1.0},
        zorder=9,
    )
    ax.annotate(
        "baseline\n27.142763",
        xy=(baseline.capacity_kib, baseline.score),
        xytext=(-62, -28),
        textcoords="offset points",
        ha="right",
        color=BLUE,
        fontsize=9,
        arrowprops={"arrowstyle": "-", "color": BLUE, "lw": 0.9},
        zorder=9,
    )

    ax.set_title("KMHv3 TAGE Capacity DSE: Area Proxy vs Performance", fontsize=19, pad=14)
    ax.set_xlabel("TAGE logical capacity / area proxy (KiB, lower is better)", fontsize=12.5)
    ax.set_ylabel("Estimated Int score per GHz (higher is better)", fontsize=12.5)
    ax.set_xlim(56.5, 94.5)
    ax.set_ylim(26.32, 27.39)
    ax.legend(loc="lower right", frameon=True, framealpha=0.97, edgecolor="#cbd1d8")
    ax.tick_params(labelsize=11)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output_path, dpi=220, facecolor="white")
    plt.close(fig)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifact_dir", type=Path)
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("docs/Gem5_Docs/images/tage-capacity-8table-t2t4-dse-2d-20260821.png"),
    )
    parser.add_argument(
        "--verify-only",
        action="store_true",
        help="audit the artifact without importing matplotlib or rendering a PNG",
    )
    args = parser.parse_args()
    trials = read_trials(args.artifact_dir)
    frontier = area_score_frontier(trials)
    if len(frontier) != EXPECTED_AREA_SCORE_FRONTIER:
        raise ValueError(
            "capacity-score frontier differs from the audited 18-point result"
        )
    best = max(frontier, key=lambda trial: trial.score)
    if best.trial_id != "trial_0748":
        raise ValueError("the audited best-score point is trial_0748")
    if args.verify_only:
        print(
            f"audited {len(trials)} valid trials; "
            f"area-score frontier: {len(frontier)}; best: {best.trial_id}"
        )
        return
    plot(trials, args.output)
    print(f"output={args.output}")


if __name__ == "__main__":
    main()
