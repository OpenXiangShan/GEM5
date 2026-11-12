#!/usr/bin/env python3
"""Validate and summarize DIP snapshots from one or more gem5 output directories."""

import argparse
from collections import Counter, defaultdict
import configparser
import csv
import math
from pathlib import Path


STATES = ("Base", "Stall", "FrontEnd", "Misspeculation")
STACK = (*STATES, "Pending")
CAUSES = (
    "S_ROB",
    "S_ISB",
    "S_LDQ",
    "S_STQ",
    "S_REN",
    "S_SER",
    "F_BUB",
    "F_CTM",
    "M_SWE",
    "M_FL",
    "M_UNKNOWN",
)
IQ_FLAGS = ("Inports", "Entries", "Replay", "Quota", "ReadyPeer")
FLUSH_REASONS = (
    "None",
    "BranchMispredict",
    "MemOrderViolation",
    "ValuePrediction",
    "Trap",
    "ThreadContext",
    "SquashAfter",
    "LongLatencyFlush",
)
PC_FIELDS = (
    "run",
    "pc",
    *(f"{state}_slots" for state in STATES),
    "total_slots",
    "overhead_slots",
    "share_pct",
    "estimated_cpi",
)
CONTEXT = (
    "state",
    "cause",
    "resolution",
    "iq",
    "iq_flags",
    "frontend_reason",
    "flush_reason",
)


def mask_names(mask, names):
    """Keep multi-hot observations together rather than summing their bits."""
    return "|".join(
        name for bit, name in enumerate(names) if mask & (1 << bit)
    )


def read_stats(path, cpu):
    """Read the latest stats block, which owns the overwritten DIP CSV."""
    values = {}
    complete = False
    with path.open() as source:
        for line in source:
            if line.startswith("---------- Begin"):
                values.clear()
                complete = False
            elif line.startswith("---------- End"):
                complete = True
            fields = line.split()
            if len(fields) >= 2 and fields[0].startswith(cpu + "."):
                values[fields[0][len(cpu) + 1 :]] = fields[1]
    if not complete:
        raise ValueError(f"{path}: latest stats block is incomplete")
    return values


def cycle_cost(slots, summary):
    """Convert sampled slot weight to a period-scaled CPI estimate."""
    return (
        slots
        / summary["width"]
        * max(1, summary["period"])
        / summary["instructions"]
    )


def analyze(path, cpu, label):
    """Check one CSV against its config/stats and aggregate finalized PCs."""
    csv_path = path / f"{cpu}.dip.csv" if path.is_dir() else path
    folder = csv_path.parent
    config = configparser.ConfigParser(interpolation=None)
    config.read(folder / "config.ini")
    if not config.getboolean(cpu, "enableDip"):
        raise ValueError(f"{folder}: enableDip is false")
    width = config.getint(cpu, "renameWidth")
    period = config.getint(cpu, "dipSamplePeriod")
    stats = read_stats(folder / "stats.txt", cpu)
    totals, states, causes = Counter(), Counter(), Counter()
    pcs = defaultdict(Counter)
    contexts = Counter()
    allowed = {"Base": 0, "Stall": 63, "FrontEnd": 192, "Misspeculation": 1792}
    with csv_path.open(newline="") as source:
        reader = csv.DictReader(source)
        required = {"thread", "pc", "slot_weight", "cycle_weight", *CONTEXT}
        missing = required - set(reader.fieldnames or ())
        if missing:
            raise ValueError(f"{csv_path}: missing columns {sorted(missing)}")
        for line, row in enumerate(reader, 2):
            state, resolution = row["state"], row["resolution"]
            slots, cause = int(row["slot_weight"]), int(row["cause"])
            if (
                row["thread"] != "0"
                or state not in STATES
                or slots <= 0
                or cause < 0
                or cause & ~allowed[state]
                or resolution not in ("resolved", "unresolved", "pending")
                or not math.isclose(
                    float(row["cycle_weight"]), slots / width, rel_tol=1e-12
                )
                or (
                    resolution != "pending"
                    and bool(row["pc"]) != (resolution == "resolved")
                )
            ):
                raise ValueError(f"{csv_path}:{line}: invalid DIP row")
            iq, flags, frontend, flush = (
                int(row[name]) for name in CONTEXT[3:]
            )
            if flags < 0 or flags >= 1 << len(IQ_FLAGS):
                raise ValueError(f"{csv_path}:{line}: invalid IQ flags")
            if not 0 <= flush < len(FLUSH_REASONS):
                raise ValueError(f"{csv_path}:{line}: invalid flush reason")
            contexts[
                state, cause, resolution, iq, flags, frontend, flush
            ] += slots
            totals[resolution] += slots
            if resolution == "pending":
                continue
            states[state] += slots
            for bit, name in enumerate(CAUSES):
                if cause & (1 << bit):
                    causes[name] += slots
            if resolution == "resolved":
                pc = hex(int(row["pc"], 16))
                pcs[pc][state] += slots

    expected = {
        "attributedSlots": totals["resolved"],
        "unresolvedSlots": totals["unresolved"],
        "pendingSlots": totals["pending"],
        "sampledSlots": sum(totals.values()),
        **{f"slotsByState::{name}": states[name] for name in STATES},
        **{f"slotsByCause::{name}": causes[name] for name in CAUSES},
    }
    for name, actual in expected.items():
        recorded = int(stats[f"dip.{name}"])
        if actual != recorded:
            raise ValueError(
                f"{folder}: CSV {name}={actual}, latest stats={recorded}; "
                "use config, stats and CSV from the same snapshot"
            )
    samples = int(stats["dip.samples"])
    total = sum(totals.values())
    if total == 0 or samples * width != total:
        raise ValueError(
            f"{folder}: no samples or sampledSlots != samples * width"
        )
    instructions = int(stats["committedInsts"])
    if instructions <= 0:
        raise ValueError(f"{folder}: committedInsts must be positive for CPI")
    summary = dict(
        run=label,
        source=str(csv_path.resolve()),
        width=width,
        period=period,
        samples=samples,
        sampled_slots=total,
        cycles=int(stats["numCycles"]),
        instructions=instructions,
        resolved_slots=totals["resolved"],
        unresolved_slots=totals["unresolved"],
        **{f"{name}_slots": states[name] for name in STATES},
        Pending_slots=totals["pending"],
        **{f"{name}_slots": causes[name] for name in CAUSES},
        decode_stranded_slots=int(stats["dip.decodeStrandedSlots"]),
        ready_branch_waits=int(stats["dip.readyBranchWaits"]),
    )
    summary["cpi"] = summary["cycles"] / instructions
    summary["estimated_cpi"] = cycle_cost(total, summary)
    pc_rows = []
    for pc, counts in sorted(
        pcs.items(),
        key=lambda item: (
            -sum(item[1][s] for s in STATES[1:]),
            int(item[0], 16),
        ),
    ):
        slots = sum(counts.values())
        pc_rows.append(
            dict(
                run=label,
                pc=pc,
                **{f"{name}_slots": counts[name] for name in STATES},
                total_slots=slots,
                overhead_slots=slots - counts["Base"],
                share_pct=100 * slots / total,
                estimated_cpi=cycle_cost(slots, summary),
            )
        )
    context_rows = []
    for key, slots in sorted(
        contexts.items(), key=lambda item: (-item[1], item[0])
    ):
        row = dict(run=label, **dict(zip(CONTEXT, key)))
        row.update(
            causes=mask_names(row["cause"], CAUSES),
            iq_gates=mask_names(row["iq_flags"], IQ_FLAGS),
            flush=FLUSH_REASONS[row["flush_reason"]],
            slot_weight=slots,
            sampled_cycles=slots / width,
            share_pct=100 * slots / total,
            estimated_cpi=cycle_cost(slots, summary),
        )
        context_rows.append(row)
    return summary, pc_rows, context_rows


def write_csv(path, rows, fieldnames=None):
    with path.open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=fieldnames or list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def write_report(path, summaries, pcs):
    lines = [
        "# DIP analysis",
        "",
        "CSV/config/latest-stats accounting checks passed for every run.",
        "",
        "State shares include finalized resolved and unresolved slots; Pending "
        "is a separate, provisional bucket. Cause bits overlap and exclude "
        "pending. Unresolved slots are already included in the final states.",
        "",
        "CPI estimates use slots / renameWidth * max(1, sample period) / "
        "committedInsts. Period > 1 retains sampling error. Actual CPI uses "
        "numCycles / committedInsts. PC tables include finalized resolved "
        "slots only, sorted by non-Base slots; tentative PCs are excluded.",
        "",
    ]
    for row in summaries:
        label, total = row["run"], row["sampled_slots"]
        lines.extend(
            [
                f"## {label}",
                "",
                f"Source: `{row['source']}`",
                "",
                f"Width: {row['width']}; sample period: {row['period']}; "
                f"samples: {row['samples']}; sampled slots: {total}.",
                "",
                f"Actual CPI: {row['cpi']:.6f}; "
                f"DIP estimated CPI: {row['estimated_cpi']:.6f}.",
                "",
                f"Unresolved: {100 * row['unresolved_slots'] / total:.4f}% "
                "of all slots (included in finalized states).",
                "",
                "| State | Slots | Share of all slots | Estimated CPI |",
                "| --- | ---: | ---: | ---: |",
            ]
        )
        for state in STACK:
            slots = row[f"{state}_slots"]
            lines.append(
                f"| {state} | {slots} | {100 * slots / total:.4f}% "
                f"| {cycle_cost(slots, row):.6f} |"
            )
        lines.extend(
            [
                "",
                "Top PCs by finalized non-Base slots:",
                "",
                "| PC | Non-Base slots | Share of all slots (all states) |",
                "| --- | ---: | ---: |",
            ]
        )
        for pc in [p for p in pcs if p["run"] == label][:10]:
            lines.append(
                f"| {pc['pc']} | {pc['overhead_slots']} "
                f"| {pc['share_pct']:.4f}% |"
            )
        lines.extend(
            [
                "",
                f"Auxiliary context: decodeStrandedSlots="
                f"{row['decode_stranded_slots']}, readyBranchWaits="
                f"{row['ready_branch_waits']}. These are outside the DIP budget.",
                "",
            ]
        )
    path.write_text("\n".join(lines))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "inputs",
        nargs="+",
        type=Path,
        help="m5out directories or DIP CSVs with adjacent config.ini/stats.txt",
    )
    parser.add_argument("-o", "--output", required=True, type=Path)
    parser.add_argument("--cpu", default="system.cpu")
    parser.add_argument(
        "--labels", nargs="+", help="one unique name per input"
    )
    args = parser.parse_args()
    labels = args.labels
    if labels is None:
        labels = []
        for path in args.inputs:
            folder = (path if path.is_dir() else path.parent).resolve()
            labels.append(
                (folder.parent if folder.name == "m5out" else folder).name
            )
    if len(labels) != len(args.inputs) or len(set(labels)) != len(labels):
        parser.error("--labels must supply one unique name per input")
    summaries, pcs, contexts = [], [], []
    for path, label in zip(args.inputs, labels):
        try:
            summary, pc_rows, context_rows = analyze(path, args.cpu, label)
        except (OSError, KeyError, ValueError, configparser.Error) as error:
            parser.error(f"{path}: {error}")
        summaries.append(summary)
        pcs.extend(pc_rows)
        contexts.extend(context_rows)
    args.output.mkdir(parents=True, exist_ok=True)
    write_csv(args.output / "summary.csv", summaries)
    write_csv(args.output / "pcs.csv", pcs, PC_FIELDS)
    write_csv(args.output / "contexts.csv", contexts)
    write_report(args.output / "report.md", summaries, pcs)
    print(f"Validated {len(summaries)} run(s); saved {args.output}")


if __name__ == "__main__":
    main()
