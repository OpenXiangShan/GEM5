#!/usr/bin/env python3
"""Recreate the TAGE capacity DSE figures from a solver-run artifact."""

import argparse
import csv
import json
from dataclasses import dataclass
from pathlib import Path

import matplotlib.pyplot as plt


BASELINE_CAPACITY_BITS = 589_824
MIN_CAPACITY_BITS = 294_912
MAX_CAPACITY_BITS = 707_788
SCORE_COLUMN = "max:score_txt:Estimated Int score per GHz"
MISPREDICT_COLUMN = "min:stats:system.cpu.iew.branchMispredicts"

BLUE = "#2166ac"
GRAY = "#8f969d"
RED = "#c53030"


@dataclass(frozen=True)
class Trial:
    trial_id: str
    generation: int
    is_baseline: bool
    score: float
    mispredicts: float
    capacity_bits: int

    @property
    def capacity_kib(self) -> float:
        return self.capacity_bits / 8192.0


def decode_capacity_bits(encoded: str) -> int:
    values = json.loads(encoded)
    num_tables = values[0]
    table_sizes = values[1 : 1 + num_tables]
    tag_bits = values[1 + num_tables : 1 + 2 * num_tables]
    num_ways = values[1 + 2 * num_tables : 1 + 3 * num_tables]

    if len(values) != 1 + 3 * num_tables:
        raise ValueError(f"malformed tageConfig with {len(values)} values")

    return sum(
        sets * ways * (tag + 5)
        for sets, tag, ways in zip(table_sizes, tag_bits, num_ways)
    )


def read_trials(artifact_dir: Path) -> list[Trial]:
    history_path = artifact_dir / "history.csv"
    with history_path.open(newline="") as history_file:
        rows = list(csv.DictReader(history_file))

    trials = []
    for row in rows:
        if row["status"] != "valid":
            continue

        is_baseline = row["is_baseline"] == "True"
        capacity_bits = (
            BASELINE_CAPACITY_BITS
            if is_baseline
            else decode_capacity_bits(row["tageConfig"])
        )
        if not is_baseline and not (
            MIN_CAPACITY_BITS <= capacity_bits <= MAX_CAPACITY_BITS
        ):
            raise ValueError(
                f"{row['trial_id']} exceeds the capacity constraint: "
                f"{capacity_bits} bit"
            )
        trials.append(
            Trial(
                trial_id=row["trial_id"],
                generation=int(row["generation"]),
                is_baseline=is_baseline,
                score=float(row[SCORE_COLUMN]),
                mispredicts=float(row[MISPREDICT_COLUMN]),
                capacity_bits=capacity_bits,
            )
        )

    if len(trials) != len(rows):
        raise ValueError("the report expects every recorded trial to be valid")
    if sum(trial.is_baseline for trial in trials) != 1:
        raise ValueError("the report expects exactly one baseline")
    return trials


def is_dominated(candidate: Trial, population: list[Trial]) -> bool:
    for other in population:
        if other == candidate:
            continue
        if (
            other.score >= candidate.score
            and other.mispredicts <= candidate.mispredicts
            and (
                other.score > candidate.score
                or other.mispredicts < candidate.mispredicts
            )
        ):
            return True
    return False


def compute_pareto_frontier(trials: list[Trial]) -> list[Trial]:
    return [trial for trial in trials if not is_dominated(trial, trials)]


def configure_axes(ax) -> None:
    ax.set_facecolor("#fcfcfc")
    ax.grid(True, color="#d7dce1", linewidth=0.8, alpha=0.9)
    for spine in ax.spines.values():
        spine.set_color("#4a4f55")


def plot_2d(trials: list[Trial], pareto: list[Trial], output_path: Path) -> None:
    baseline = next(trial for trial in trials if trial.is_baseline)
    candidates = [trial for trial in trials if not trial.is_baseline]
    best = max(pareto, key=lambda trial: (trial.score, -trial.mispredicts))
    pareto_by_capacity = sorted(pareto, key=lambda trial: trial.capacity_kib)

    fig, ax = plt.subplots(figsize=(13.5, 8.1), layout="constrained")
    configure_axes(ax)
    ax.scatter(
        [trial.capacity_kib for trial in candidates],
        [trial.score for trial in candidates],
        s=36,
        color=GRAY,
        alpha=0.63,
        edgecolors="white",
        linewidths=0.35,
        label="Trials (n=671)",
    )
    ax.scatter(
        baseline.capacity_kib,
        baseline.score,
        s=96,
        color=BLUE,
        edgecolors="white",
        linewidths=0.9,
        zorder=4,
        label="Baseline (trial_0001)",
    )
    ax.plot(
        [trial.capacity_kib for trial in pareto_by_capacity],
        [trial.score for trial in pareto_by_capacity],
        color=RED,
        linewidth=1.6,
        alpha=0.82,
        zorder=5,
    )
    ax.scatter(
        [trial.capacity_kib for trial in pareto],
        [trial.score for trial in pareto],
        s=74,
        color=RED,
        edgecolors="white",
        linewidths=0.8,
        zorder=6,
        label="2-objective Pareto frontier (n=3)",
    )
    ax.scatter(
        best.capacity_kib,
        best.score,
        s=260,
        marker="*",
        color=RED,
        edgecolors="#7f1d1d",
        linewidths=0.8,
        zorder=7,
        label="Best primary-score point (trial_0205)",
    )

    for trial in pareto:
        offset = (7, 9) if trial.trial_id != "trial_0205" else (7, -18)
        ax.annotate(
            trial.trial_id,
            (trial.capacity_kib, trial.score),
            xytext=offset,
            textcoords="offset points",
            color="#7f1d1d",
            fontsize=9,
            weight="bold",
        )

    ax.set_title("KMHv3 BTB-TAGE Capacity DSE", fontsize=18, pad=12)
    ax.set_xlabel("TAGE logical capacity / area proxy (KiB)", fontsize=12)
    ax.set_ylabel("Estimated Int score per GHz (higher is better)", fontsize=12)
    ax.legend(loc="lower right", frameon=True, framealpha=0.96)
    fig.savefig(output_path, dpi=220, facecolor="white")
    plt.close(fig)


def plot_3d(trials: list[Trial], pareto: list[Trial], output_path: Path) -> None:
    baseline = next(trial for trial in trials if trial.is_baseline)
    candidates = [trial for trial in trials if not trial.is_baseline]
    best = max(pareto, key=lambda trial: (trial.score, -trial.mispredicts))
    pareto_by_capacity = sorted(pareto, key=lambda trial: trial.capacity_kib)

    fig = plt.figure(figsize=(14.0, 10.0), layout="constrained")
    ax = fig.add_subplot(111, projection="3d")
    ax.set_facecolor("#fcfcfc")
    ax.xaxis.pane.set_facecolor("#f7f9fb")
    ax.yaxis.pane.set_facecolor("#f7f9fb")
    ax.zaxis.pane.set_facecolor("#f7f9fb")
    ax.grid(True, color="#d7dce1", linewidth=0.7)

    ax.scatter(
        [trial.capacity_kib for trial in candidates],
        [trial.score for trial in candidates],
        [trial.mispredicts for trial in candidates],
        s=23,
        color=GRAY,
        alpha=0.55,
        depthshade=False,
        label="Trials (n=671)",
    )
    ax.scatter(
        baseline.capacity_kib,
        baseline.score,
        baseline.mispredicts,
        s=88,
        color=BLUE,
        edgecolors="white",
        linewidths=0.8,
        depthshade=False,
        label="Baseline (trial_0001)",
    )
    ax.plot(
        [trial.capacity_kib for trial in pareto_by_capacity],
        [trial.score for trial in pareto_by_capacity],
        [trial.mispredicts for trial in pareto_by_capacity],
        color=RED,
        linewidth=1.8,
        alpha=0.85,
    )
    ax.scatter(
        [trial.capacity_kib for trial in pareto],
        [trial.score for trial in pareto],
        [trial.mispredicts for trial in pareto],
        s=66,
        color=RED,
        edgecolors="white",
        linewidths=0.7,
        depthshade=False,
        label="2-objective Pareto frontier (n=3)",
    )
    ax.scatter(
        best.capacity_kib,
        best.score,
        best.mispredicts,
        s=280,
        marker="*",
        color=RED,
        edgecolors="#7f1d1d",
        linewidths=0.8,
        depthshade=False,
        label="Best primary-score point (trial_0205)",
    )

    ax.set_title("KMHv3 BTB-TAGE Capacity DSE", fontsize=18, pad=18)
    ax.set_xlabel("TAGE logical capacity / area proxy (KiB)", labelpad=11)
    ax.set_ylabel("Estimated Int score per GHz", labelpad=14)
    ax.set_zlabel("Weighted branch mispredicts", labelpad=11)
    ax.view_init(elev=25, azim=-61)
    ax.legend(loc="upper left", bbox_to_anchor=(0.02, 0.98), framealpha=0.96)
    fig.savefig(output_path, dpi=220, facecolor="white")
    plt.close(fig)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifact_dir", type=Path)
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path("docs/Gem5_Docs/images"),
        help="directory for the generated PNG files",
    )
    args = parser.parse_args()

    trials = read_trials(args.artifact_dir)
    pareto = compute_pareto_frontier(trials)
    expected_pareto = {"trial_0205", "trial_0351", "trial_0671"}
    if {trial.trial_id for trial in pareto} != expected_pareto:
        raise ValueError("artifact Pareto frontier differs from the audited result")

    args.output_dir.mkdir(parents=True, exist_ok=True)
    plot_3d(
        trials,
        pareto,
        args.output_dir / "tage-capacity-nsga2-dse-3d-20260815.png",
    )
    plot_2d(
        trials,
        pareto,
        args.output_dir / "tage-capacity-nsga2-dse-2d-20260815.png",
    )
    print(f"audited {len(trials)} valid trials; Pareto size: {len(pareto)}")


if __name__ == "__main__":
    main()
