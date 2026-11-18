#!/usr/bin/env python3
"""Plot TEA state shares and event weights from gem5 stats or summary CSVs."""

import argparse
from collections import Counter
import csv
import math
from pathlib import Path
import subprocess

from pc_symbols import pc_labels


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


def sample_rows(reader):
    for row in reader:
        weight = float(row["sample_weight"])
        if not math.isfinite(weight) or weight < 0:
            raise ValueError("TEA sample weights must be finite and nonnegative")
        if weight:
            pc = hex(int(row["pc"], 16)) if row["pc"] else None
            yield pc, int(row["psv"]), row.get("state"), weight


def require_state(reader):
    if "state" not in (reader.fieldnames or ()):
        raise ValueError("raw TEA CSV has no sampled state column")


def read_pcs(path, state=None):
    """Rank finalized PCs relative to all weight in the chosen population."""
    pcs = Counter()
    total = 0.0
    with path.open(newline="") as source:
        reader = csv.DictReader(source)
        if state:
            require_state(reader)
        for pc, psv, sampled_state, weight in sample_rows(reader):
            if state and sampled_state != state:
                continue
            total += weight
            if psv >= 0 and pc:
                pcs[pc] += weight
    if total <= 0 and state is None:
        raise ValueError("no TEA samples")
    return (
        sorted(pcs.items(), key=lambda item: (-item[1], int(item[0], 16))),
        total,
    )


RESOLUTIONS = {-1: "Cancelled", -2: "Pending", -3: "Unresolved"}
COLUMNS = EVENTS + ("No event",) + tuple(RESOLUTIONS.values())


def pc_events(path, selected_pc):
    totals = Counter()
    counts = Counter()
    with path.open(newline="") as source:
        reader = csv.DictReader(source)
        require_state(reader)
        for pc, psv, state, weight in sample_rows(reader):
            if pc != selected_pc:
                continue
            if state not in STATES:
                raise ValueError(f"unknown TEA state: {state}")
            totals[state] += weight
            if psv < 0:
                if psv not in RESOLUTIONS:
                    raise ValueError(f"unknown TEA resolution: {psv}")
                counts[state, RESOLUTIONS[psv]] += weight
            elif psv == 0:
                counts[state, "No event"] += weight
            else:
                if psv >> len(EVENTS):
                    raise ValueError(f"unknown TEA event bits in PSV: {psv}")
                for bit, event in enumerate(EVENTS):
                    if psv & (1 << bit):
                        counts[state, event] += weight
    if not totals:
        raise ValueError(f"no TEA samples for PC {selected_pc}")
    return totals, counts


def plot_pc_events(path, output, cpu, pc):
    import matplotlib

    matplotlib.use("Agg")
    from matplotlib import pyplot as plt

    totals, counts = pc_events(path, pc)
    matrix = [
        [
            100 * counts[state, col] / totals[state] if totals[state] else math.nan
            for col in COLUMNS
        ]
        for state in STATES
    ]
    fig, ax = plt.subplots(figsize=(13, 4))
    heat = ax.imshow(matrix, aspect="auto", cmap="Blues", vmin=0, vmax=100)
    for i, row in enumerate(matrix):
        for j, value in enumerate(row):
            if value > 0:
                ax.text(j, i, f"{value:.1f}", ha="center", va="center",
                        color="white" if value > 55 else "black")
    ax.set_facecolor("#dedede")
    ax.set_xticks(range(len(COLUMNS)), COLUMNS, rotation=45, ha="right")
    ax.set_yticks(range(len(STATES)), [f"{s} (w={totals[s]:g})" for s in STATES])
    ax.set_title(f"{cpu} · {pc} · PSV by sampled state")
    ax.set_xlabel("Percent of row weight · event bits overlap · gray = no samples")
    fig.colorbar(heat, ax=ax, label="%")
    fig.tight_layout()
    output.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output, dpi=180, bbox_inches="tight")
    plt.close(fig)


def plot_pcs(path, output, cpu, top, elfs, objdump, state=None):
    import matplotlib

    matplotlib.use("Agg")
    from matplotlib import pyplot as plt

    rows, total = read_pcs(path, state)
    rows = rows[:top]
    if not rows:
        raise ValueError("no finalized attributed TEA PC samples")
    fig, ax = plt.subplots(
        figsize=(
            14 if elfs else 11,
            max(4.5, len(rows) * (0.65 if elfs else 0.38) + 2),
        )
    )
    ax.barh(
        range(len(rows)),
        [100 * weight / total for _, weight in rows],
        color=COLORS[0],
    )
    ax.set_yticks(
        range(len(rows)),
        pc_labels([pc for pc, _ in rows], elfs, objdump),
        fontsize=9,
    )
    ax.invert_yaxis()
    population = state or "all states"
    ax.set_title(f"{cpu}: TEA finalized PC sample weights ({population})")
    ax.set_xlabel(
        f"Share of TEA sample weight ({population}, including all resolutions) [%]"
    )
    ax.spines[["top", "right"]].set_visible(False)
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
        help="stats.txt, summary CSVs for overview, or a raw TEA CSV for PC views",
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
    parser.add_argument(
        "--view", choices=("overview", "pcs", "state-pcs", "state-psv"), default="overview"
    )
    parser.add_argument("--state", choices=STATES, help="state-pcs filter (default: Stalled)")
    parser.add_argument("--pc", type=lambda value: hex(int(value, 16)),
                        help="hexadecimal PC for state-psv")
    parser.add_argument("--top", type=int, default=15)
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
    parser.add_argument("--title", default="TEA sample breakdown")
    args = parser.parse_args()
    if args.top <= 0:
        parser.error("--top must be positive")
    if args.state and args.view != "state-pcs":
        parser.error("--state requires --view state-pcs")
    if (args.pc is not None) != (args.view == "state-psv"):
        parser.error("--pc and --view state-psv must be used together")
    if args.view != "overview":
        if len(args.inputs) != 1:
            parser.error(f"--view {args.view} requires one stats.txt or raw TEA CSV")
        path = args.inputs[0]
        if path.suffix != ".csv":
            path = path.parent / f"{args.cpu}.tea.csv"
        try:
            if args.view == "state-psv":
                plot_pc_events(path, args.output, args.cpu, args.pc)
            else:
                state = (args.state or "Stalled") if args.view == "state-pcs" else None
                plot_pcs(path, args.output, args.cpu, args.top, args.elf,
                         args.objdump, state)
        except (
            OSError,
            KeyError,
            ValueError,
            subprocess.CalledProcessError,
        ) as error:
            parser.error(str(error))
        print(f"Saved {args.output}")
        return

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
