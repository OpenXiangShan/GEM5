#!/usr/bin/env python3
"""Render 0.3c solver DSE points selected for GCC15 SPEC06 1.0c regression."""

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
EXPECTED_SCORE_BRANCH_PARETO = frozenset(
    {"trial_0271", "trial_0505", "trial_0748"}
)
NEAR_BASELINE_MIN_KIB = 70.0
NEAR_BASELINE_MAX_KIB = 75.0
NEAR_BASELINE_SCORE_RANKS = {"C3": 0, "C4": 1}


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
    missing = [
        selection.trial_id
        for selection in SELECTIONS
        if selection.trial_id not in by_id
    ]
    if missing:
        raise ValueError(f"selected trials missing from artifact: {missing}")
    if len({selection.label for selection in SELECTIONS}) != len(SELECTIONS):
        raise ValueError("selection labels must be unique")
    if len({selection.trial_id for selection in SELECTIONS}) != len(SELECTIONS):
        raise ValueError("selection trial IDs must be unique")
    return [(selection, by_id[selection.trial_id]) for selection in SELECTIONS]


def score_branch_pareto(trials: list[Trial]) -> set[str]:
    return {
        candidate.trial_id
        for candidate in trials
        if not any(
            other is not candidate
            and other.score >= candidate.score
            and other.branch_mispredicts <= candidate.branch_mispredicts
            and (
                other.score > candidate.score
                or other.branch_mispredicts < candidate.branch_mispredicts
            )
            for other in trials
        )
    }


def validate_selection(trials: list[Trial]) -> list[tuple[Selection, Trial]]:
    selections = selected_trials(trials)
    selected_by_label = {selection.label: trial for selection, trial in selections}
    formal_pareto = score_branch_pareto(trials)
    if formal_pareto != EXPECTED_SCORE_BRANCH_PARETO:
        raise ValueError(
            "score/branch Pareto differs from audited artifact: "
            f"{sorted(formal_pareto)}"
        )
    selected_pareto = {
        trial.trial_id
        for selection, trial in selections
        if selection.category == "pareto"
    }
    if selected_pareto != formal_pareto:
        raise ValueError("P labels do not match the formal score/branch Pareto set")

    area_score = area_score_frontier(trials)
    if len(area_score) != EXPECTED_AREA_SCORE_FRONTIER:
        raise ValueError("capacity-score frontier differs from audited artifact")
    area_score_ids = {trial.trial_id for trial in area_score}
    capacity_selection_ids = {
        trial.trial_id
        for selection, trial in selections
        if selection.category == "capacity"
    }
    if not capacity_selection_ids <= area_score_ids:
        raise ValueError("C labels must be on the capacity-score projection")

    near_baseline = sorted(
        (
            trial
            for trial in trials
            if not trial.is_baseline
            and NEAR_BASELINE_MIN_KIB <= trial.capacity_kib <= NEAR_BASELINE_MAX_KIB
        ),
        key=lambda trial: (-trial.score, trial.trial_id),
    )
    for label, expected_rank in NEAR_BASELINE_SCORE_RANKS.items():
        if (
            len(near_baseline) <= expected_rank
            or near_baseline[expected_rank].trial_id
            != selected_by_label[label].trial_id
        ):
            raise ValueError(
                f"{label} is not score rank #{expected_rank + 1} in the "
                f"{NEAR_BASELINE_MIN_KIB:g}--{NEAR_BASELINE_MAX_KIB:g} KiB band"
            )
    return selections


def plot(
    trials: list[Trial],
    selections: list[tuple[Selection, Trial]],
    output_path: Path,
) -> None:
    import matplotlib.pyplot as plt

    baseline = next(trial for trial in trials if trial.is_baseline)
    candidates = [trial for trial in trials if not trial.is_baseline]
    frontier = area_score_frontier(trials)

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
        "KMHv3 TAGE 0.3c DSE: Points Selected for GCC15 SPEC06 1.0c Regression",
        fontsize=18,
        pad=14,
    )
    ax.set_xlabel("TAGE logical capacity / area proxy (KiB, lower is better)", fontsize=12.5)
    ax.set_ylabel(
        "0.3c solver Estimated Int score per GHz (higher is better)",
        fontsize=12.5,
    )
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
    selections = validate_selection(trials)
    if args.verify_only:
        labels = ", ".join(
            f"{selection.label}={trial.trial_id}"
            for selection, trial in selections
        )
        print(
            f"audited {len(trials)} valid trials; "
            f"area-score frontier: {EXPECTED_AREA_SCORE_FRONTIER}; selections: {labels}"
        )
        return
    plot(trials, selections, args.output)
    print(f"output={args.output}")


if __name__ == "__main__":
    main()
