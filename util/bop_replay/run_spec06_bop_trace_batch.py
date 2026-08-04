#!/usr/bin/env python3
"""Export and certify BOP replay traces for selected SPEC06 checkpoints.

Each case is independent: GEM5 writes its SQLite database into the case
directory and the raw learner replay must pass before the case is marked
complete.  The runner deliberately limits external GEM5 processes rather
than sharing a simulation output directory between workers.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import logging
import os
import shutil
import sqlite3
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_REF_SO = Path(
    "/nfs/home/share/gem5_ci/ref/normal/riscv64-nemu-notama-tvalref-so"
)
DEFAULT_MANIFEST = Path(__file__).with_name("spec06_bop_policy_manifest.json")


@dataclass(frozen=True)
class Case:
    name: str
    checkpoint: Path


@dataclass(frozen=True)
class Result:
    case: str
    status: str
    detail: str


def load_cases(manifest_path: Path, selected: set[str]) -> list[Case]:
    payload = json.loads(manifest_path.read_text())
    root = Path(payload["checkpoint_root"])
    cases = [
        Case(str(entry["name"]), root / str(entry["checkpoint"]))
        for entry in payload["cases"]
    ]
    if selected:
        known = {case.name for case in cases}
        unknown = sorted(selected - known)
        if unknown:
            raise ValueError("unknown case(s): " + ", ".join(unknown))
        cases = [case for case in cases if case.name in selected]
    if not cases:
        raise ValueError("no cases selected")
    missing = [str(case.checkpoint) for case in cases if not case.checkpoint.is_file()]
    if missing:
        raise FileNotFoundError("checkpoint(s) missing:\n" + "\n".join(missing))
    return cases


def trace_schema_version(trace_db: Path) -> int | None:
    if not trace_db.is_file():
        return None
    try:
        with sqlite3.connect(trace_db) as connection:
            row = connection.execute(
                "SELECT SchemaVersion FROM BOPReplayMeta LIMIT 1"
            ).fetchone()
    except sqlite3.Error:
        return None
    return int(row[0]) if row else None


def completed(case_dir: Path) -> bool:
    trace_db = case_dir / "trace.db"
    baseline = case_dir / "raw-baseline.json"
    if trace_schema_version(trace_db) != 5 or not baseline.is_file():
        return False
    try:
        report = json.loads(baseline.read_text())
    except json.JSONDecodeError:
        return False
    verification = report.get("verification")
    return bool(verification and verification.get("pass"))


def run_case(
    case: Case, output_root: Path, gem5_binary: Path, config_script: Path,
    warmup_insts: int, max_insts: int, mem_type: str, resume: bool,
    ref_so: Path | None,
) -> Result:
    case_dir = output_root / case.name
    if resume and completed(case_dir):
        return Result(case.name, "skipped", "existing V5 trace passed raw verification")

    case_dir.mkdir(parents=True, exist_ok=True)
    trace_db = case_dir / "trace.db"
    stdout_path = case_dir / "gem5.stdout"
    stderr_path = case_dir / "gem5.stderr"
    raw_path = case_dir / "raw-baseline.json"
    command_path = case_dir / "command.json"

    command = [
        str(gem5_binary),
        f"--outdir={case_dir}",
        str(config_script),
        f"--generic-rv-cpt={case.checkpoint}",
        "--enable-arch-db",
        f"--arch-db-file={trace_db}",
        "--dump-bop-replay-trace",
        f"--warmup-insts-no-switch={warmup_insts}",
        f"--maxinsts={max_insts}",
        f"--mem-type={mem_type}",
    ]
    if ref_so is not None:
        command.append(f"--difftest-ref-so={ref_so}")
    command_path.write_text(json.dumps(command, indent=2) + "\n")
    env = os.environ.copy()
    if not env.get("GCBV_REF_SO") and ref_so is not None:
        env["GCBV_REF_SO"] = str(ref_so)

    with stdout_path.open("w") as stdout, stderr_path.open("w") as stderr:
        result = subprocess.run(command, stdout=stdout, stderr=stderr, env=env)
    if result.returncode != 0:
        return Result(case.name, "gem5_failed", f"exit={result.returncode}")
    if trace_schema_version(trace_db) != 5:
        return Result(case.name, "trace_failed", "missing or non-V5 BOP trace")

    verify_command = [
        sys.executable,
        str(REPO_ROOT / "util" / "bop_replay" / "bop_replay.py"),
        str(trace_db),
        "--mode=learner-replay",
        "--candidate-stage=raw",
        "--evaluation-phase=stable",
        "--verify-online",
        "--horizons=512,2048",
        f"--output={raw_path}",
    ]
    verify_result = subprocess.run(
        verify_command, capture_output=True, text=True, env=env
    )
    (case_dir / "raw-verify.stdout").write_text(verify_result.stdout)
    (case_dir / "raw-verify.stderr").write_text(verify_result.stderr)
    if verify_result.returncode != 0:
        return Result(case.name, "raw_verify_failed", f"exit={verify_result.returncode}")
    if not completed(case_dir):
        return Result(case.name, "raw_verify_failed", "report does not certify raw replay")
    return Result(case.name, "passed", "V5 trace and raw replay certified")


def write_summary(output_root: Path, results: list[Result]) -> None:
    payload = {
        "cases": [
            {"case": result.case, "status": result.status, "detail": result.detail}
            for result in sorted(results, key=lambda item: item.case)
        ],
        "counts": {
            status: sum(result.status == status for result in results)
            for status in sorted({result.status for result in results})
        },
    }
    (output_root / "trace-batch-summary.json").write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n"
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run independent SPEC06 BOP trace exports with raw certification"
    )
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument(
        "--output-root", type=Path,
        default=Path("/nfs/home/lijiangtao/temp/bop-replay/spec06-bop-policy-sweep-20260813"),
    )
    parser.add_argument("--workers", type=int, default=8)
    parser.add_argument("--case", action="append", default=[])
    parser.add_argument("--warmup-insts", type=int, default=20_000_000)
    parser.add_argument("--max-insts", type=int, default=40_000_000)
    parser.add_argument(
        "--mem-type", default="DRAMsim3",
        help="memory model passed to kmhv3; default is DRAMsim3",
    )
    parser.add_argument("--resume", action="store_true")
    parser.add_argument(
        "--gem5-binary", type=Path,
        default=REPO_ROOT / "build" / "RISCV" / "gem5.opt",
    )
    parser.add_argument(
        "--config-script", type=Path,
        default=REPO_ROOT / "configs" / "example" / "kmhv3.py",
    )
    parser.add_argument(
        "--ref-so", type=Path, default=DEFAULT_REF_SO,
        help="difftest reference shared library passed explicitly to GEM5",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.workers <= 0:
        raise ValueError("workers must be positive")
    if args.max_insts <= args.warmup_insts:
        raise ValueError("max-insts must exceed warmup-insts")
    if not args.gem5_binary.is_file():
        raise FileNotFoundError(f"gem5 binary not found: {args.gem5_binary}")
    if not args.config_script.is_file():
        raise FileNotFoundError(f"config script not found: {args.config_script}")
    ref_so = args.ref_so if args.ref_so.is_file() else None
    if ref_so is None:
        raise FileNotFoundError(
            f"difftest ref-so is missing: {args.ref_so}"
        )

    cases = load_cases(args.manifest, set(args.case))
    args.output_root.mkdir(parents=True, exist_ok=True)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    logging.info("launching %d case(s) with workers=%d", len(cases), args.workers)
    results: list[Result] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as executor:
        futures = {
            executor.submit(
                run_case, case, args.output_root, args.gem5_binary,
                args.config_script, args.warmup_insts, args.max_insts,
                args.mem_type, args.resume, ref_so,
            ): case
            for case in cases
        }
        for future in concurrent.futures.as_completed(futures):
            case = futures[future]
            try:
                result = future.result()
            except Exception as error:
                result = Result(case.name, "runner_failed", str(error))
            results.append(result)
            logging.info("%s: %s (%s)", result.case, result.status, result.detail)
            write_summary(args.output_root, results)
    write_summary(args.output_root, results)
    failures = [result for result in results if result.status not in {"passed", "skipped"}]
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
