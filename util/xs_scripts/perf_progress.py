#!/usr/bin/env python3
"""Report perf-trigger-style progress for the existing GEM5 launchers."""

import json
import logging
import os
import socket
import subprocess
import sys
import time
from itertools import islice
from pathlib import Path
from typing import TextIO

RESULTS_DIR = Path("spec_all")


def checkpoint_labels(
    listing: Path, filters: str, trace_format: str
) -> dict[str, str]:
    tokens = [
        token.strip().lower() for token in filters.split(",") if token.strip()
    ]
    names = [
        line.split()[0]
        for line in listing.read_text().splitlines()
        if line.strip()
        and (not tokens or any(token in line.lower() for token in tokens))
    ]
    cluster = Path("cluster.json")
    weights = {}
    if not trace_format and cluster.is_file():
        weights = {
            f"{workload}_{point}": float(weight)
            for workload, entry in json.loads(cluster.read_text()).items()
            if isinstance(entry, dict)
            for point, weight in entry.get("points", {}).items()
        }
    return {
        name: f"{name}_{weights[name]:.6f}" if name in weights else name
        for name in names
    }


def checkpoint_host(log_path: Path) -> str:
    # Remote launchers record the real host in the slice log header.
    try:
        with log_path.open(errors="replace") as log:
            for line in islice(log, 3):
                if line.startswith("host: "):
                    return line.removeprefix("host: ").strip().split(".", 1)[0]
    except FileNotFoundError:
        pass
    return ""


def report_progress(
    proc: subprocess.Popen,
    launcher: TextIO,
    labels: dict[str, str],
    already_done: set[str],
    local: bool,
) -> int:
    local_host = socket.gethostname().split(".", 1)[0] if local else ""
    assigned: dict[str, tuple[str, float | None]] = {}
    finished: set[str] = set()
    failed_count = last_remaining = 0
    total = len(labels)

    def progress(message: str, level: int = logging.INFO) -> None:
        counts = f"{len(assigned)},{len(finished)},{failed_count}/{total}"
        logging.log(level, "[%s] %s", counts, message)

    servers = 1 if local else None
    if local:
        progress(f"Start running {total} checkpoints on 1 servers")
    while True:
        status = proc.poll()
        if servers is None:
            for line in launcher:
                if line.startswith("Servers: "):
                    servers = len(line.removeprefix("Servers: ").split(", "))
                    progress(
                        f"Start running {total} checkpoints on {servers} servers"
                    )
                    break

        for name, label in (labels.items() if servers is not None else ()):
            if name in finished:
                continue
            work = RESULTS_DIR / name
            log_path = work / "log.txt"
            completed = work / "completed"
            done = completed.is_file()
            failed = (work / "abort").is_file()
            if name not in assigned:
                try:
                    started_at = (work / "running").stat().st_mtime
                except FileNotFoundError:
                    if not done and not failed:
                        continue
                    started_at = None
                node = local_host if local else checkpoint_host(log_path)
                if name in already_done:
                    progress(f"{label} is State.FINISHED, skipping")
                    finished.add(name)
                elif node:
                    progress(f"Started {label} on {node}")
                assigned[name] = (node, started_at)
                if len(assigned) == total:
                    progress(
                        "All checkpoints assigned, waiting for completion..."
                    )

            if (done or failed) and name not in finished:
                node, started_at = assigned[name]
                where = f" \ton {node}" if node else ""
                if done:
                    duration = ""
                    if started_at is not None:
                        elapsed = time.gmtime(
                            max(0, completed.stat().st_mtime - started_at)
                        )
                        duration = " \tin " + time.strftime(
                            "%Hh %Mm %Ss", elapsed
                        )
                    progress(f"{label} succeeded{where}{duration}")
                else:
                    progress(
                        f"{label} failed{where} (log: {log_path})",
                        logging.ERROR,
                    )
                    failed_count += 1
                finished.add(name)

        remaining = total - len(finished)
        if (
            len(assigned) == total
            and 0 < remaining <= 5
            and remaining != last_remaining
        ):
            pending = ", ".join(
                label for name, label in labels.items() if name not in finished
            )
            progress(f"Remaining checkpoints: {pending}")
            last_remaining = remaining
        if status is not None:
            break
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            pass

    if total and len(finished) == total:
        progress("All checkpoints completed")
    # The workflow handles status 75 by retrying with the local launcher.
    if status and status != 75:
        progress(
            f"Launcher exits with code {status} (log: {launcher.name})",
            logging.ERROR,
        )
    return status if status >= 0 else 128 - status


def main() -> int:
    listing, filters, trace_format, *command = sys.argv[1:]
    labels = checkpoint_labels(Path(listing), filters, trace_format)
    already_done = {
        name for name in labels if (RESULTS_DIR / name / "completed").is_file()
    }
    logging.basicConfig(
        level=logging.INFO, format="%(asctime)s - %(levelname)5s - %(message)s"
    )
    log_path = Path("launcher.log").resolve()
    with log_path.open("a") as output, log_path.open(
        errors="replace"
    ) as launcher:
        launcher.seek(0, os.SEEK_END)
        with subprocess.Popen(
            command, stdout=output, stderr=subprocess.STDOUT
        ) as proc:
            return report_progress(
                proc,
                launcher,
                labels,
                already_done,
                local=command[0] == "bash",
            )


if __name__ == "__main__":
    sys.exit(main())
