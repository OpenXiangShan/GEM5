"""Run a local checkpoint or trace list, without the CI archive and scorer."""
import argparse
import os
from pathlib import Path
import sys

from .benchmarks import select_workloads
from .runner import (
    Job,
    RunnerConfig,
    cancellation_signals,
    run_jobs,
    simulation_env,
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("config", type=Path)
    parser.add_argument("listing", type=Path)
    parser.add_argument("root", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument(
        "--binary", type=Path, default=Path("build/RISCV/gem5.opt")
    )
    parser.add_argument("--trace-format", default="")
    parser.add_argument(
        "--jobs", type=int, default=int(os.getenv("xsgem5_para_jobs", 63))
    )
    parser.add_argument("--filter", default="")
    args, extra = parser.parse_known_args()
    cmd = [str(args.binary.resolve(strict=True))]
    if args.trace_format:
        cmd.extend(["--outdir=.", "--stats-file=stats.txt"])
    cmd.append(str(args.config.resolve(strict=True)))
    env = simulation_env()
    jobs = []
    for w in select_workloads(
        args.listing.read_text().splitlines(),
        args.root,
        args.filter,
        args.trace_format,
    ):
        directory = args.output.resolve() / w.name
        if (directory / "completed").is_file():
            print(f"Skipping completed {w.name}")
            continue
        jobs.append(
            Job(
                "",
                w.name,
                w.image,
                directory,
                cmd + w.arguments(args.trace_format) + extra,
                env,
            )
        )
    with cancellation_signals() as event:
        results = run_jobs(
            jobs, RunnerConfig(), args.jobs, cancelled=event.is_set
        )
        if event.is_set():
            return 130
    return int(any(r.return_code for r in results))


if __name__ == "__main__":
    sys.exit(main())
