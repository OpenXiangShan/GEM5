#!/usr/bin/env python3
"""Plot TEA state shares and event weights from gem5 stats or summary CSVs."""

import argparse
import csv
import math
from pathlib import Path


STATES = ("Computing", "Stalled", "Flushed", "Drained")
EVENTS = (
    "DR_L1",
    "DR_TLB",
    "DR_SQ",
    "FL_MB",
    "FL_EX",
    "FL_MO",
    "ST_L1",
    "ST_TLB",
    "ST_LLC",
)
COLORS = ("#4c78a8", "#e9a03b", "#d45d61", "#72b7b2")


def read_stats(path, cpu):
    """Normalize the selected CPU's TEA counters in the last stats block."""
    prefix = f"{cpu}.tea."
    stats = {}
    with path.open() as source:
        for line in source:
            if line.startswith("---------- Begin"):
                stats.clear()
            fields = line.split()
            if len(fields) >= 2 and fields[0].startswith(prefix):
                stats[fields[0][len(prefix) :]] = float(fields[1])
    samples = stats["samples"]
    if not math.isfinite(samples) or samples <= 0:
        raise ValueError(f"{prefix}samples must be positive and finite")
    shares = {}
    for names, group in (
        (STATES, "samplesByState"),
        (EVENTS, "samplesByEvent"),
    ):
        for name in names:
            shares[name] = 100 * stats[f"{group}::{name}"] / samples
    return [(path.parent.name or path.stem, shares)]


def read_summary(path):
    """Read percentages that have already been aggregated by the caller."""
    with path.open(newline="") as source:
        return [
            (
                row["benchmark"],
                {name: float(row[f"{name}_pct"]) for name in STATES + EVENTS},
            )
            for row in csv.DictReader(source)
        ]


def plot(rows, output, title):
    import matplotlib

    matplotlib.use("Agg")
    from matplotlib import pyplot as plt

    fig, axes = plt.subplots(
        1,
        2,
        figsize=(14, max(4, len(rows) * 0.34)),
        sharey=True,
        gridspec_kw={"width_ratios": [1.2, 1]},
    )
    positions = range(len(rows))
    offset = [0.0] * len(rows)
    for state, color in zip(STATES, COLORS):
        values = [shares[state] for _, shares in rows]
        axes[0].barh(positions, values, left=offset, label=state, color=color)
        offset = [left + value for left, value in zip(offset, values)]
    axes[0].set_yticks(positions, [label for label, _ in rows])
    axes[0].set(
        xlim=(0, 100),
        xlabel="Share of all samples (%)",
        title="TEA states (mutually exclusive)",
    )
    axes[0].legend(
        loc="lower center",
        bbox_to_anchor=(0.5, 1.02),
        ncol=2,
        frameon=False,
        borderaxespad=2.0,
    )
    matrix = [[shares[event] for event in EVENTS] for _, shares in rows]
    heatmap = axes[1].imshow(
        matrix,
        aspect="auto",
        origin="lower",
        cmap="YlOrRd",
        vmin=0,
        vmax=100,
    )
    axes[1].set_xticks(range(len(EVENTS)), EVENTS, rotation=55, ha="right")
    axes[1].set_title("Event weights (overlaps allowed)")
    fig.colorbar(
        heatmap,
        ax=axes[1],
        label="% of all sample weight",
        fraction=0.04,
        pad=0.03,
    )
    for ax in axes:
        ax.spines[["top", "right"]].set_visible(False)
    fig.suptitle(title)
    fig.tight_layout()
    output.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output, dpi=180, bbox_inches="tight")
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "inputs",
        type=Path,
        nargs="+",
        help="stats.txt files (last block) or summary CSVs (see TEA guide)",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        required=True,
        help="output figure path, e.g. breakdown.png, .svg, or .pdf",
    )
    parser.add_argument(
        "--cpu",
        default="system.cpu",
        help="CPU object name in stats.txt (default: %(default)s)",
    )
    parser.add_argument(
        "--labels",
        nargs="+",
        help="display labels, one per plotted row",
    )
    parser.add_argument("--title", default="TEA sample breakdown")
    args = parser.parse_args()

    rows = []
    for path in args.inputs:
        try:
            data = (
                read_summary(path)
                if path.suffix == ".csv"
                else read_stats(path, args.cpu)
            )
            if not data:
                raise ValueError("no TEA rows found")
            for label, shares in data:
                if any(
                    not math.isfinite(value)
                    or not -1e-7 <= value <= 100 + 1e-7
                    for value in shares.values()
                ):
                    raise ValueError(f"{label}: shares must be within 0..100")
                if not math.isclose(
                    math.fsum(shares[state] for state in STATES),
                    100,
                    abs_tol=1e-5,
                ):
                    raise ValueError(f"{label}: state shares must sum to 100%")
            rows.extend(data)
        except (OSError, KeyError, ValueError) as error:
            parser.error(f"{path}: {error}")
    if args.labels is not None:
        if len(args.labels) != len(rows):
            parser.error("--labels must provide one label per plotted row")
        rows = [(label, row[1]) for label, row in zip(args.labels, rows)]
    plot(rows, args.output, args.title)
    print(f"Saved {args.output}")


if __name__ == "__main__":
    main()
