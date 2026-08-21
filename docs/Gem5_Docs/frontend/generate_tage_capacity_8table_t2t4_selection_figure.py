#!/usr/bin/env python3
"""Render the solver DSE plot with GCC15 SPEC06 1.0c regression selections."""

import argparse
from dataclasses import dataclass
from pathlib import Path

from generate_tage_capacity_8table_t2t4_dse_figures import (
    BLUE,
    EXPECTED_AREA_SCORE_FRONTIER,
    GRAY,
    RED,
    Trial,
    area_score_frontier,
    read_trials,
)


TEAL = "#0f766e"
AMBER = "#b45309"


@dataclass(frozen=True)
class Selection:
    label: str
    trial_id: str
    category: str
    offset: tuple[int, int]
    horizontal_alignment: str


# P1--P3 are the formal score/branch-mispredict solver Pareto set.  C1--C4
# are capacity/validation choices and do not define a second solver frontier.
SELECTIONS = (
    Selection("P1", "trial_0748", "pareto", (-18, -44), "right"),
    Selection("P2", "trial_0271", "pareto", (11, -40), "left"),
    Selection("P3", "trial_0505", "pareto", (11, -24), "left"),
    Selection("C1", "trial_0584", "capacity", (-11, 16), "right"),
    Selection("C2", "trial_0808", "capacity", (-11, -32), "right"),
    Selection("C3", "trial_0335", "capacity", (12, -22), "left"),
    Selection("C4", "trial_0166", "capacity", (-12, 16), "right"),
)


def selected_trials(trials: list[Trial]) -> list[tuple[Selection, Trial]]:
    by_id = {trial.trial_id: trial for trial in trials}
    missing = [selection.trial_id for selection in SELECTIONS if selection.trial_id not in by_id]
    if missing:
        raise ValueError(f"selected trials missing from artifact: {missing}")
    if len({selection.label for selection in SELECTIONS}) != len(SELECTIONS):
        raise ValueError("selection labels must be unique")
    if len({selection.trial_id for selection in SELECTIONS}) != len(SELECTIONS):
        raise ValueError("selection trial IDs must be unique")
    return [(selection, by_id[selection.trial_id]) for selection in SELECTIONS]


def plot(trials: list[Trial], output_path: Path) -> None:
    import matplotlib.pyplot as plt

    baseline = next(trial for trial in trials if trial.is_baseline)
    candidates = [trial for trial in trials if not trial.is_baseline]
    frontier = area_score_frontier(trials)
    if len(frontier) != EXPECTED_AREA_SCORE_FRONTIER:
        raise ValueError("capacity-score frontier differs from audited artifact")
    selections = selected_trials(trials)

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
        label="Default baseline (trial_0001)",
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
        label=f"Area-score projection (n={len(frontier)})",
        zorder=7,
    )

    pareto = [(selection, trial) for selection, trial in selections if selection.category == "pareto"]
    capacity = [(selection, trial) for selection, trial in selections if selection.category == "capacity"]
    ax.scatter(
        [trial.capacity_kib for _, trial in pareto],
        [trial.score for _, trial in pareto],
        s=148,
        marker="s",
        color=TEAL,
        edgecolors="white",
        linewidths=1.15,
        label="P: solver score/branch Pareto",
        zorder=9,
    )
    ax.scatter(
        [trial.capacity_kib for _, trial in capacity],
        [trial.score for _, trial in capacity],
        s=150,
        marker="D",
        color=AMBER,
        edgecolors="white",
        linewidths=1.15,
        label="C: capacity/validation choices",
        zorder=9,
    )
    for selection, trial in selections:
        color = TEAL if selection.category == "pareto" else AMBER
        ax.annotate(
            f"{selection.label}\n{selection.trial_id.removeprefix('trial_')}",
            xy=(trial.capacity_kib, trial.score),
            xytext=selection.offset,
            textcoords="offset points",
            ha=selection.horizontal_alignment,
            va="center",
            color=color,
            fontsize=9.4,
            weight="bold",
            arrowprops={"arrowstyle": "-", "color": color, "lw": 0.9},
            zorder=10,
        )

    ax.annotate(
        "default\n72.000 KiB",
        xy=(baseline.capacity_kib, baseline.score),
        xytext=(-62, -28),
        textcoords="offset points",
        ha="right",
        color=BLUE,
        fontsize=9,
        arrowprops={"arrowstyle": "-", "color": BLUE, "lw": 0.9},
        zorder=10,
    )
    ax.set_title(
        "KMHv3 TAGE Capacity DSE: Selected GCC15 SPEC06 1.0c Regression Points",
        fontsize=18,
        pad=14,
    )
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
        default=Path(
            "docs/Gem5_Docs/images/"
            "tage-capacity-8table-t2t4-selected-spec06-1c-20260821.png"
        ),
    )
    parser.add_argument(
        "--verify-only",
        action="store_true",
        help="audit the selected trial IDs without importing matplotlib or rendering a PNG",
    )
    args = parser.parse_args()

    trials = read_trials(args.artifact_dir)
    frontier = area_score_frontier(trials)
    if len(frontier) != EXPECTED_AREA_SCORE_FRONTIER:
        raise ValueError("capacity-score frontier differs from audited artifact")
    selections = selected_trials(trials)
    if args.verify_only:
        labels = ", ".join(
            f"{selection.label}={trial.trial_id}"
            for selection, trial in selections
        )
        print(
            f"audited {len(trials)} valid trials; "
            f"area-score frontier: {len(frontier)}; selections: {labels}"
        )
        return
    plot(trials, args.output)
    print(f"output={args.output}")


if __name__ == "__main__":
    main()
