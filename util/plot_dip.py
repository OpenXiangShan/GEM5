#!/usr/bin/env python3
"""Plot DIP state/cause comparisons or finalized PC hotspots from analyze_dip.py."""

import argparse
import csv
import subprocess
from pathlib import Path

from analyze_dip import CAUSES, STACK, STATES, cycle_cost
from pc_symbols import pc_labels


COLORS = ("#4c78a8", "#e9a03b", "#72b7b2", "#d45d61", "#b5b5b5")


def read_summary(folder):
    with (folder / "summary.csv").open(newline="") as source:
        rows = list(csv.DictReader(source))
    for row in rows:
        for key in ("width", "period", "instructions", "sampled_slots"):
            row[key] = int(row[key])
        for name in (*STACK, *CAUSES):
            row[f"{name}_slots"] = int(row[f"{name}_slots"])
    return rows


def value(slots, row, unit):
    if unit == "cpi":
        return cycle_cost(slots, row)
    return 100 * slots / row["sampled_slots"]


def overview(plt, rows, unit):
    fig, axes = plt.subplots(
        1,
        2,
        figsize=(15, max(4.5, len(rows) * 0.5 + 2)),
        gridspec_kw={"width_ratios": [1.1, 1]},
    )
    positions = range(len(rows))
    offsets = [0.0] * len(rows)
    for state, color in zip(STACK, COLORS):
        values = [value(row[f"{state}_slots"], row, unit) for row in rows]
        axes[0].barh(
            positions,
            values,
            left=offsets,
            label=state,
            color=color,
            hatch="//" if state == "Pending" else None,
        )
        offsets = [a + b for a, b in zip(offsets, values)]
    axes[0].set_yticks(positions, [row["run"] for row in rows])
    axes[0].set_title("Finalized states + provisional Pending")
    axes[0].set_xlabel(
        "Share of all sampled slots (%)"
        if unit == "percent"
        else "Estimated cycles per committed instruction"
    )
    if unit == "percent":
        axes[0].set_xlim(0, 100)
    axes[0].legend(
        loc="lower center",
        bbox_to_anchor=(0.5, 1.1),
        ncol=3,
        frameon=False,
        fontsize=9,
    )
    matrix = [
        [value(row[f"{cause}_slots"], row, unit) for cause in CAUSES]
        for row in rows
    ]
    maximum = 100 if unit == "percent" else max(map(max, matrix)) or 1
    image = axes[1].imshow(
        matrix,
        aspect="auto",
        origin="lower",
        cmap="YlOrRd",
        vmin=0,
        vmax=maximum,
    )
    axes[1].set_xticks(range(len(CAUSES)), CAUSES, rotation=55, ha="right")
    axes[1].set_yticks(positions, [row["run"] for row in rows])
    axes[1].set_title("Finalized cause bits (overlaps allowed)")
    for y, values in enumerate(matrix):
        for x, amount in enumerate(values):
            axes[1].text(
                x,
                y,
                f"{amount:.1f}" if unit == "percent" else f"{amount:.3f}",
                ha="center",
                va="center",
                fontsize=8,
                color="white" if amount > maximum * 0.6 else "black",
            )
    fig.colorbar(
        image,
        ax=axes[1],
        fraction=0.04,
        pad=0.03,
        label="% of all slots" if unit == "percent" else "Estimated CPI",
    )
    return fig, axes


def hotspots(plt, folder, summary, unit, top, elfs=(), objdump=None):
    with (folder / "pcs.csv").open(newline="") as source:
        rows = [
            row
            for row in csv.DictReader(source)
            if row["run"] == summary["run"]
        ]
    if not rows:
        raise ValueError("selected run has no finalized resolved PC slots")
    rows.sort(
        key=lambda row: (-int(row["overhead_slots"]), int(row["pc"], 16))
    )
    rows = rows[:top]
    fig, ax = plt.subplots(
        figsize=(
            14 if elfs else 11,
            max(4.5, len(rows) * (0.65 if elfs else 0.38) + 2),
        )
    )
    offsets = [0.0] * len(rows)
    for state, color in zip(STATES, COLORS):
        values = [
            value(int(row[f"{state}_slots"]), summary, unit) for row in rows
        ]
        ax.barh(
            range(len(rows)), values, left=offsets, label=state, color=color
        )
        offsets = [a + b for a, b in zip(offsets, values)]
    ax.set_yticks(
        range(len(rows)),
        pc_labels([row["pc"] for row in rows], elfs, objdump),
        fontsize=9,
    )
    ax.invert_yaxis()
    ax.set_title(f"{summary['run']}: top {len(rows)} PCs by non-Base slots")
    ax.set_xlabel(
        "Share of all run slots (%); finalized resolved PCs only"
        if unit == "percent"
        else "Estimated cycles per committed instruction"
    )
    ax.legend(loc="lower right", frameon=False)
    return fig, [ax]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "input", type=Path, help="analyze_dip.py output directory"
    )
    parser.add_argument(
        "-o",
        "--output",
        required=True,
        type=Path,
        help="figure filename, e.g. dip.png, dip.svg or dip.pdf",
    )
    parser.add_argument(
        "--view", choices=("overview", "pcs"), default="overview"
    )
    parser.add_argument(
        "--unit", choices=("percent", "cpi"), default="percent"
    )
    parser.add_argument(
        "--run", help="select a run label (required for multi-run PCs)"
    )
    parser.add_argument(
        "--top", type=int, default=15, help="number of PC hotspots"
    )
    parser.add_argument(
        "--elf",
        type=Path,
        action="append",
        default=[],
        help="matching workload ELF for PC labels; repeat for kernel ELF",
    )
    parser.add_argument(
        "--objdump", help="RISC-V objdump executable for --elf"
    )
    parser.add_argument("--title", default="DIP dispatch-slot attribution")
    args = parser.parse_args()
    if args.top <= 0:
        parser.error("--top must be positive")
    try:
        import matplotlib

        matplotlib.use("Agg")
        from matplotlib import pyplot as plt
    except ImportError:
        parser.error(
            "plotting requires Matplotlib; install it with pip install matplotlib"
        )
    try:
        rows = read_summary(args.input)
        if args.run is not None:
            rows = [row for row in rows if row["run"] == args.run]
        if not rows:
            raise ValueError("no matching runs in summary.csv")
        if args.view == "pcs":
            if len(rows) != 1:
                raise ValueError("use --run to select one run for the PC plot")
            fig, axes = hotspots(
                plt,
                args.input,
                rows[0],
                args.unit,
                args.top,
                args.elf,
                args.objdump,
            )
        else:
            fig, axes = overview(plt, rows, args.unit)
        for ax in axes:
            ax.spines[["top", "right"]].set_visible(False)
        fig.suptitle(args.title)
        fig.text(
            0.5,
            0.015,
            "Pending PCs are provisional and excluded from hotspots. "
            "Unresolved slots are included in finalized states.\n"
            "CPI uses period-scaled samples; period > 1 retains sampling error.",
            ha="center",
            fontsize=9,
        )
        fig.tight_layout(rect=(0, 0.08, 1, 0.94))
        args.output.parent.mkdir(parents=True, exist_ok=True)
        fig.savefig(args.output, dpi=180, bbox_inches="tight")
        plt.close(fig)
    except (
        OSError,
        KeyError,
        ValueError,
        subprocess.CalledProcessError,
    ) as error:
        parser.error(str(error))
    print(f"Saved {args.output}")


if __name__ == "__main__":
    main()
