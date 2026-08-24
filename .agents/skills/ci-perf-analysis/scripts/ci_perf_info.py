#!/usr/bin/env python3
"""Locate archived CI performance data and print the score summary."""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path


DEFAULT_REPO = "OpenXiangShan/GEM5"
ARCHIVE_PATTERNS = (
    re.compile(
        r"Performance data already written to\s+"
        r"(/nfs/home/share/gem5_ci/performance_data/[^\s`]+)"
    ),
    re.compile(
        r"Archiving performance data to\s+"
        r"(/nfs/home/share/gem5_ci/performance_data/[^\s`]+)"
    ),
    re.compile(
        r"- Path:\s*`"
        r"(/nfs/home/share/gem5_ci/performance_data/[^`]+)"
        r"`"
    ),
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


def get_jobs(repo: str, run_id: str) -> list[dict]:
    output = run_cmd(
        ["gh", "api", f"repos/{repo}/actions/runs/{run_id}/jobs?per_page=100"]
    )
    data = json.loads(output)
    jobs = data.get("jobs", [])
    if not jobs:
        raise SystemExit(f"no jobs found for run {run_id}")
    return sorted(
        jobs,
        key=lambda job: (
            "perf" not in str(job.get("name", "")).lower(),
            str(job.get("name", "")),
        ),
    )


def find_archive_path(repo: str, jobs: list[dict]) -> tuple[str, str, str]:
    checked_jobs = []
    for job in jobs:
        job_id = str(job["id"])
        job_name = str(job.get("name", ""))
        checked_jobs.append(f"{job_id} ({job_name})")
        log_text = run_cmd(
            ["gh", "api", f"repos/{repo}/actions/jobs/{job_id}/logs"]
        )
        for pattern in ARCHIVE_PATTERNS:
            match = pattern.search(log_text)
            if match:
                return job_id, job_name, match.group(1).strip()

    raise SystemExit(
        "cannot find archive path in logs; checked jobs: "
        + ", ".join(checked_jobs)
    )


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
        "--repo",
        default=DEFAULT_REPO,
        help=f"GitHub repository (default: {DEFAULT_REPO})",
    )
    parser.add_argument(
        "--tail-lines",
        type=int,
        default=42,
        help="how many trailing lines of score.txt to print",
    )
    args = parser.parse_args()

    run_id = parse_run_id(args.run)
    jobs = get_jobs(args.repo, run_id)
    job_id, job_name, archive = find_archive_path(args.repo, jobs)
    archive_path = Path(archive)

    print(f"run_id: {run_id}")
    print(f"job_id: {job_id}")
    print(f"job_name: {job_name}")
    print(f"archive_path: {archive_path}")
    print(f"spec_all: {archive_path / 'spec_all'}")

    score_path = archive_path / "score.txt"
    print_score(score_path, args.tail_lines)


if __name__ == "__main__":
    main()
