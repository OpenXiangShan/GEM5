#!/usr/bin/env python3
"""Generate DIP/TEA CSVs and Markdown, with optional plots for a completed run."""

import argparse
import configparser
import csv
from pathlib import Path
import subprocess
import sys

from analyze_dip import STACK, cycle_cost, read_stats
import plot_tea
from pc_symbols import objdump_tool, profile_elfs


def generate(folder, output, elfs=(), objdump=None, plots=False):
    config = configparser.ConfigParser(interpolation=None)
    config.read(folder / "config.ini")
    tools = Path(__file__).resolve().parent
    lines = [
        "# DIP and TEA profile report",
        "",
        f"Source: `{folder.resolve()}`; latest statistics block.",
        "",
        "DIP observes dispatch slots; TEA observes architectural retirement. "
        "Their stacks describe separate observation points and must not be added. "
        "Cause/event bits overlap and are observations, not removable cycle costs.",
        "",
    ]
    symbol_args = []
    if plots:
        elfs = list(elfs) or profile_elfs(config, output)
        if elfs and objdump_tool(objdump):
            lines.extend(["ELF inputs for exact PC instruction labels:", ""])
            for elf in elfs:
                lines.append(f"- `{elf}`")
                symbol_args.extend(["--elf", str(elf)])
            if objdump:
                symbol_args.extend(["--objdump", objdump])
            lines.append("")
        else:
            lines.extend(
                [
                    "Instruction labels unavailable: no matching profile ELF "
                    "or RISC-V objdump. Supply --elf and --objdump to annotate PCs.",
                    "",
                ]
            )
    figures = []
    for cpu in config.sections():
        dip = config.getboolean(cpu, "enableDip", fallback=False)
        tea = config.getboolean(cpu, "enableTea", fallback=False)
        if not (dip or tea):
            continue
        stats = read_stats(folder / "stats.txt", cpu)
        cycles = int(stats["numCycles"])
        instructions = int(stats["committedInsts"])
        cpi = f"{cycles / instructions:.6f}" if instructions else "n/a"
        lines.extend(
            [
                f"## {cpu}",
                "",
                f"Cycles: {cycles}; committed instructions: {instructions}; CPI: {cpi}.",
                "",
            ]
        )
        if dip and int(stats["dip.samples"]) and instructions:
            dest = output / cpu / "dip"
            subprocess.run(
                [
                    sys.executable,
                    str(tools / "analyze_dip.py"),
                    str(folder / f"{cpu}.dip.csv"),
                    "--cpu",
                    cpu,
                    "--labels",
                    cpu,
                    "-o",
                    str(dest),
                ],
                check=True,
            )
            with (dest / "summary.csv").open(newline="") as stream:
                summary = next(csv.DictReader(stream))
            for key in ("width", "period", "instructions", "sampled_slots"):
                summary[key] = int(summary[key])
            lines.extend(
                [
                    "### DIP",
                    "",
                    f"Samples: {stats['dip.samples']}; period: {summary['period']}. "
                    "Pending is provisional; unresolved slots are included in "
                    "finalized states. CPI estimates scale sampled slots by the "
                    "period and retain sampling error.",
                    "",
                    "| State | Share of all slots | Estimated CPI |",
                    "| --- | ---: | ---: |",
                ]
            )
            for state in STACK:
                slots = int(summary[f"{state}_slots"])
                lines.append(
                    f"| {state} | {100 * slots / summary['sampled_slots']:.4f}% "
                    f"| {cycle_cost(slots, summary):.6f} |"
                )
            relative = dest.relative_to(output)
            lines.extend(
                [
                    "",
                    f"[DIP details and top PCs]({relative}/report.md) · "
                    f"[PC table]({relative}/pcs.csv) · "
                    f"[Admission context]({relative}/contexts.csv)",
                    "",
                ]
            )
            if plots:
                for view in ("overview", "pcs"):
                    if view == "pcs" and not int(summary["resolved_slots"]):
                        continue
                    image = dest / f"{view}.png"
                    figures.append(
                        (
                            f"{cpu} DIP {view}",
                            image,
                            [
                                sys.executable,
                                str(tools / "plot_dip.py"),
                                str(dest),
                                "--view",
                                view,
                                "-o",
                                str(image),
                                *(symbol_args if view == "pcs" else []),
                            ],
                        )
                    )
        else:
            lines.extend(
                [
                    "### DIP",
                    "",
                    "Disabled."
                    if not dip
                    else "No measured DIP profile: no samples or committed instructions "
                    "in the final window (the run may have ended during warmup).",
                    "",
                ]
            )
        if tea and int(stats["tea.samples"]):
            shares = plot_tea.read_stats(folder / "stats.txt", cpu)[0][1]
            period = config.getint(cpu, "teaSamplePeriod")
            lines.extend(
                [
                    "### TEA",
                    "",
                    f"Samples: {stats['tea.samples']}; period: {period}. "
                    "State shares include all samples. Event weights overlap.",
                    "",
                    "| State | Share of all samples |",
                    "| --- | ---: |",
                ]
            )
            for state in plot_tea.STATES:
                lines.append(f"| {state} | {shares[state]:.4f}% |")
            lines.extend(
                [
                    "",
                    "| Event | Share of all samples |",
                    "| --- | ---: |",
                ]
            )
            for event in plot_tea.EVENTS:
                lines.append(f"| {event} | {shares[event]:.4f}% |")
            lines.extend(
                [
                    "",
                    f"Attribution: attributed={stats['tea.attributedSamples']}, "
                    f"cancelled={stats['tea.cancelledTargets']}, "
                    f"pending={stats['tea.pendingSamples']}, "
                    f"unresolved={stats['tea.unresolvedTargets']}. "
                    "These resolution counts are already part of the state stack.",
                    "",
                ]
            )
            if plots:
                image = output / cpu / "tea.png"
                figures.append(
                    (
                        f"{cpu} TEA",
                        image,
                        [
                            sys.executable,
                            str(tools / "plot_tea.py"),
                            str(folder / "stats.txt"),
                            "--cpu",
                            cpu,
                            "--labels",
                            cpu,
                            "-o",
                            str(image),
                        ],
                    )
                )
                pc_rows, _ = plot_tea.read_pcs(folder / f"{cpu}.tea.csv")
                if pc_rows:
                    image = output / cpu / "tea-pcs.png"
                    figures.append(
                        (
                            f"{cpu} TEA PCs",
                            image,
                            [
                                sys.executable,
                                str(tools / "plot_tea.py"),
                                str(folder / f"{cpu}.tea.csv"),
                                "--view",
                                "pcs",
                                "--cpu",
                                cpu,
                                "-o",
                                str(image),
                                *symbol_args,
                            ],
                        )
                    )
        else:
            lines.extend(
                [
                    "### TEA",
                    "",
                    "Disabled."
                    if not tea
                    else "No TEA samples in the final window.",
                    "",
                ]
            )
    if plots:
        lines.extend(["## Visualizations", ""])
    else:
        lines.extend(
            [
                "Generate the image report manually with "
                "`python3 util/profile_report.py <outdir> --plots`.",
                "",
            ]
        )
    errors = []
    for caption, image, command in figures:
        try:
            subprocess.run(command, check=True)
        except (OSError, subprocess.CalledProcessError) as error:
            lines.extend([f"{caption}: plotting failed: `{error}`.", ""])
            errors.append(str(error))
        else:
            lines.extend([f"![{caption}]({image.relative_to(output)})", ""])
    output.mkdir(parents=True, exist_ok=True)
    report = output / "report.md"
    report.write_text("\n".join(lines))
    print(f"Profile report: {report}", flush=True)
    if errors:
        raise ValueError(
            "plotting failed; numerical report saved; see plotting errors above"
        )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "input", type=Path, help="completed gem5 output directory"
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        help="report directory (default: INPUT/profile)",
    )
    parser.add_argument(
        "--plots",
        action="store_true",
        help="also generate the image report (requires Matplotlib)",
    )
    parser.add_argument(
        "--elf",
        type=Path,
        action="append",
        default=[],
        help="workload ELF for --plots; repeat for additional images; default: checkpoint profile discovery",
    )
    parser.add_argument(
        "--objdump", help="RISC-V objdump executable for instruction labels"
    )
    args = parser.parse_args()
    try:
        generate(
            args.input,
            args.output or args.input / "profile",
            args.elf,
            args.objdump,
            args.plots,
        )
    except (
        OSError,
        KeyError,
        ValueError,
        configparser.Error,
        subprocess.CalledProcessError,
    ) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
