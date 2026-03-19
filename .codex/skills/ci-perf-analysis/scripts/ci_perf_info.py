#!/usr/bin/env python3
"""Locate archived CI performance data and print the score summary."""

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path


REPO = "OpenXiangShan/GEM5"
ARCHIVE_RE = re.compile(
    r"Archiving performance data to "
    r"(/nfs/home/share/gem5_ci/performance_data/\S+)"
)


def run_cmd(cmd: list[str]) -> str:
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(result.stderr.strip() or f"command failed: {' '.join(cmd)}")
    return result.stdout


def parse_run_id(text: str) -> str:
    match = re.search(r"/actions/runs/(\d+)", text)
    if match:
        return match.group(1)
    if text.isdigit():
        return text
    raise SystemExit(f"cannot parse run id from: {text}")


def get_job_id(run_id: str) -> str:
    output = run_cmd(["gh", "api", f"repos/{REPO}/actions/runs/{run_id}/jobs"])
    data = json.loads(output)
    jobs = data.get("jobs", [])
    if not jobs:
        raise SystemExit(f"no jobs found for run {run_id}")
    return str(jobs[0]["id"])


def get_archive_path(job_id: str) -> str:
    log_text = run_cmd(["gh", "api", f"repos/{REPO}/actions/jobs/{job_id}/logs"])
    match = ARCHIVE_RE.search(log_text)
    if not match:
        raise SystemExit(f"cannot find archive path in logs for job {job_id}")
    return match.group(1)


def print_score(score_path: Path, tail_lines: int) -> None:
    if not score_path.is_file():
        print(f"score.txt not found: {score_path}", file=sys.stderr)
        return

    lines = score_path.read_text().splitlines()
    print(f"score.txt: {score_path}")
    print(f"--- tail -n {tail_lines} ---")
    for line in lines[-tail_lines:]:
        print(line)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Print CI archive path and score summary for a GEM5 perf run."
    )
    parser.add_argument("run", help="GitHub Actions run URL or run id")
    parser.add_argument(
        "--tail-lines",
        type=int,
        default=42,
        help="how many trailing lines of score.txt to print",
    )
    args = parser.parse_args()

    run_id = parse_run_id(args.run)
    job_id = get_job_id(run_id)
    archive_path = Path(get_archive_path(job_id))

    print(f"run_id: {run_id}")
    print(f"job_id: {job_id}")
    print(f"archive_path: {archive_path}")
    print(f"spec_all: {archive_path / 'spec_all'}")

    score_path = archive_path / "score.txt"
    print_score(score_path, args.tail_lines)


if __name__ == "__main__":
    main()
