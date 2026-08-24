#!/usr/bin/env python3
"""Validate and dispatch the GEM5 manual solver workflow.

The skill is responsible for deciding whether a user's request is complete.
This helper handles only deterministic input validation, command construction,
and the post-dispatch run lookup.  It never edits a solver spec.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timedelta, timezone
import json
from pathlib import Path
import shlex
import subprocess
import sys
import time


CONFIGURATIONS = {"kmhv2.py", "kmhv3.py", "idealkmhv3.py"}
SOLVER_KINDS = {"auto", "grid", "random", "bayes", "ga", "nsga2"}
SMT_PREFIX = "gcc12-spec06-smt-"


def _nonempty(value: str, field: str) -> str:
    value = (value or "").strip()
    if not value:
        raise ValueError(f"{field} must not be empty")
    return value


def _positive(value: str, field: str) -> str:
    value = _nonempty(value, field)
    try:
        number = int(value)
    except ValueError as exc:
        raise ValueError(f"{field} must be an integer, got {value!r}") from exc
    if number <= 0:
        raise ValueError(f"{field} must be > 0, got {number}")
    return str(number)


def _nonnegative(value: str, field: str) -> str:
    value = _nonempty(value, field)
    try:
        number = int(value)
    except ValueError as exc:
        raise ValueError(f"{field} must be an integer, got {value!r}") from exc
    if number < 0:
        raise ValueError(f"{field} must be >= 0, got {number}")
    return str(number)


def _validate_local_spec(args: argparse.Namespace, values: dict[str, str]) -> None:
    """Reuse the repository parser for semantic checks before dispatch.

    The target ref is the source of truth for CI, but the skill requires the
    spec to be locally parseable before it dispatches. This catches conflicts
    such as `custom_bin + score_txt` and multi-objective `bayes`/`ga` before
    `gh workflow run` is invoked.
    """

    repo_root = Path(__file__).resolve().parents[4]
    if str(repo_root) not in sys.path:
        sys.path.insert(0, str(repo_root))
    try:
        from util.solver.parser.load_spec import parse_problem
        from util.solver.run_solver import apply_runtime_overrides
    except Exception as exc:  # pragma: no cover - repository import failure
        print(f"warning: local solver parser is unavailable: {exc}", file=sys.stderr)
        return

    runtime_args = argparse.Namespace(
        max_trials=int(values["max_trials"]) if values["max_trials"] else None,
        config_path=values["configuration"],
        benchmark_type=values["benchmark_type"],
        custom_bin=values["custom_bin"],
        specific_benchmarks=values["specific_benchmarks"],
        extra_args=values["extra_args"],
    )
    try:
        problem = parse_problem(values["problem_ref"])
        problem = apply_runtime_overrides(problem, runtime_args)
    except Exception as exc:
        raise ValueError(f"problem_ref/spec semantic validation failed: {exc}") from exc

    if values["solver_kind"] in {"bayes", "ga"} and problem.is_multi_objective():
        raise ValueError(
            f"solver_kind={values['solver_kind']} supports only single-objective specs; "
            "use nsga2 for this multi-objective problem"
        )

    print(
        "local spec check: "
        f"{problem.name}, {len(problem.parameters)} parameter(s), "
        f"{len(problem.objective_list())} objective(s), "
        f"benchmark_type={problem.benchmark_type}"
    )


def validate(args: argparse.Namespace) -> dict[str, str]:
    """Validate workflow semantics and return normalized string inputs."""

    normalized = {
        "note": (args.note or "Manual Solver Run").strip(),
        "problem_ref": _nonempty(args.problem_ref, "problem_ref"),
        "configuration": _nonempty(args.configuration, "configuration"),
        "benchmark_type": _nonempty(args.benchmark_type, "benchmark_type"),
        "max_parallel_trials": _positive(
            args.max_parallel_trials, "max_parallel_trials"
        ),
        "max_parallel_workloads": _positive(
            args.max_parallel_workloads, "max_parallel_workloads"
        ),
        "distributed_servers": (args.distributed_servers or "").strip(),
        "distributed_jobs_per_server": _nonnegative(
            args.distributed_jobs_per_server, "distributed_jobs_per_server"
        ),
        "solver_kind": _nonempty(args.solver_kind, "solver_kind"),
        "specific_benchmarks": (args.specific_benchmarks or "").strip(),
        "custom_bin": (args.custom_bin or "").strip(),
        "extra_args": (args.extra_args or "").strip(),
        "max_trials": (args.max_trials or "").strip(),
        "branch": _nonempty(args.branch, "branch"),
    }

    if normalized["configuration"] not in CONFIGURATIONS:
        allowed = ", ".join(sorted(CONFIGURATIONS))
        raise ValueError(
            f"unsupported configuration {normalized['configuration']!r}; "
            f"choose one of: {allowed}"
        )
    if normalized["solver_kind"] not in SOLVER_KINDS:
        raise ValueError(
            f"unsupported solver_kind {normalized['solver_kind']!r}; "
            f"choose one of: {', '.join(sorted(SOLVER_KINDS))}"
        )
    if normalized["benchmark_type"].startswith(SMT_PREFIX):
        raise ValueError(
            f"SMT benchmark_type {normalized['benchmark_type']!r} is not "
            "supported by the solver workflow"
        )

    is_custom = normalized["benchmark_type"] == "custom_bin"
    if is_custom and not normalized["custom_bin"]:
        raise ValueError("custom_bin is required when benchmark_type=custom_bin")
    if is_custom and normalized["specific_benchmarks"]:
        raise ValueError(
            "specific_benchmarks must be empty when benchmark_type=custom_bin"
        )
    if not is_custom and normalized["custom_bin"]:
        print(
            "warning: custom_bin is ignored for a built-in benchmark_type",
            file=sys.stderr,
        )
        normalized["custom_bin"] = ""
    if normalized["max_trials"]:
        normalized["max_trials"] = _positive(
            normalized["max_trials"], "max_trials"
        )
    _validate_local_spec(args, normalized)
    return normalized


def workflow_command(args: argparse.Namespace, values: dict[str, str]) -> list[str]:
    ref = (args.ref or values["branch"]).strip()
    command = [
        "gh",
        "workflow",
        "run",
        args.workflow,
        "--repo",
        args.repo,
        "--ref",
        ref,
    ]
    for key in (
        "note",
        "problem_ref",
        "configuration",
        "benchmark_type",
        "max_parallel_trials",
        "max_parallel_workloads",
        "distributed_servers",
        "distributed_jobs_per_server",
        "solver_kind",
        "specific_benchmarks",
        "custom_bin",
        "extra_args",
        "max_trials",
        "branch",
    ):
        command.extend(["-f", f"{key}={values[key]}"])
    return command


def _run(command: list[str], *, check: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(command, check=check, text=True, capture_output=True)


def check_gh_and_workflow(args: argparse.Namespace) -> None:
    try:
        _run(["gh", "--version"])
    except (FileNotFoundError, subprocess.CalledProcessError) as exc:
        raise RuntimeError("gh is not installed or cannot be executed") from exc

    auth = _run(["gh", "auth", "status"], check=False)
    if auth.returncode != 0:
        detail = (auth.stderr or auth.stdout).strip()
        raise RuntimeError(
            "gh authentication is not ready; run `gh auth login` first"
            + (f": {detail}" if detail else "")
        )

    ref = (args.ref or args.branch).strip()
    workflow = _run(
        [
            "gh",
            "workflow",
            "view",
            args.workflow,
            "--repo",
            args.repo,
            "--ref",
            ref,
            "--yaml",
        ],
        check=False,
    )
    if workflow.returncode != 0:
        detail = (workflow.stderr or workflow.stdout).strip()
        raise RuntimeError(
            f"workflow {args.workflow!r} is not registered or not visible in "
            f"{args.repo}; local workflow files are not sufficient"
            + (f": {detail}" if detail else "")
        )


def _run_list(args: argparse.Namespace) -> list[dict]:
    result = _run(
        [
            "gh",
            "run",
            "list",
            "--workflow",
            args.workflow,
            "--repo",
            args.repo,
            "--event",
            "workflow_dispatch",
            "--limit",
            "10",
            "--json",
            "databaseId,status,conclusion,url,createdAt,headBranch,headSha,displayTitle",
        ],
        check=False,
    )
    if result.returncode != 0:
        detail = (result.stderr or result.stdout).strip()
        print(f"run lookup failed: {detail}", file=sys.stderr)
        return []
    try:
        payload = json.loads(result.stdout or "[]")
    except json.JSONDecodeError:
        print("run lookup returned non-JSON output:", result.stdout, file=sys.stderr)
        return []
    return payload if isinstance(payload, list) else []


def find_recent_run(args: argparse.Namespace, started: datetime) -> dict | None:
    candidates = _run_list(args)
    lower_bound = started - timedelta(seconds=120)
    ref = (args.ref or args.branch).strip()
    for run in candidates:
        created_raw = run.get("createdAt", "")
        try:
            created = datetime.fromisoformat(created_raw.replace("Z", "+00:00"))
        except (TypeError, ValueError):
            continue
        if created < lower_bound:
            continue
        if ref and run.get("headBranch") not in {None, ref}:
            # A SHA ref may not have a matching headBranch; keep those records.
            if not all(character in "0123456789abcdefABCDEF" for character in ref):
                continue
        return run
    return None


def add_common_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--repo", default="OpenXiangShan/GEM5")
    parser.add_argument("--workflow", default="manual-solve.yml")
    parser.add_argument("--ref", default="", help="workflow ref; defaults to --branch")
    parser.add_argument("--branch", required=True)
    parser.add_argument("--problem-ref", required=True)
    parser.add_argument("--configuration", required=True)
    parser.add_argument("--benchmark-type", required=True)
    parser.add_argument("--max-parallel-trials", default="4")
    parser.add_argument("--max-parallel-workloads", default="4")
    parser.add_argument("--distributed-servers", default="")
    parser.add_argument("--distributed-jobs-per-server", default="0")
    parser.add_argument("--solver-kind", default="auto")
    parser.add_argument("--specific-benchmarks", default="")
    parser.add_argument("--custom-bin", default="")
    parser.add_argument("--extra-args", default="")
    parser.add_argument("--max-trials", default="")
    parser.add_argument("--note", default="Manual Solver Run")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    validate_parser = subparsers.add_parser("validate")
    add_common_arguments(validate_parser)

    dispatch_parser = subparsers.add_parser("dispatch")
    add_common_arguments(dispatch_parser)
    dispatch_parser.add_argument(
        "--dry-run",
        action="store_true",
        help="print the gh command without checking auth or dispatching",
    )
    dispatch_parser.add_argument(
        "--yes",
        action="store_true",
        help="authorize the actual workflow dispatch",
    )
    dispatch_parser.add_argument(
        "--wait-seconds",
        type=int,
        default=8,
        help="seconds to wait before looking up the newly created run",
    )

    args = parser.parse_args()
    try:
        values = validate(args)
        print(json.dumps(values, ensure_ascii=False, indent=2))
        if args.command == "validate":
            return 0

        command = workflow_command(args, values)
        print("command:")
        print(shlex.join(command))
        if args.dry_run:
            return 0
        if not args.yes:
            raise RuntimeError(
                "refusing to dispatch without --yes; show the command first and "
                "run it again after explicit user authorization"
            )
        check_gh_and_workflow(args)
        started = datetime.now(timezone.utc)
        result = subprocess.run(command, text=True, capture_output=True)
        if result.stdout:
            print(result.stdout.rstrip())
        if result.returncode != 0:
            if result.stderr:
                print(result.stderr.rstrip(), file=sys.stderr)
            return result.returncode
        if args.wait_seconds > 0:
            time.sleep(args.wait_seconds)
        run = find_recent_run(args, started)
        if run is None:
            print("dispatch completed, but no recent workflow_dispatch run was found")
            return 0
        print("run:")
        print(json.dumps(run, ensure_ascii=False, indent=2))
        return 0
    except (RuntimeError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
