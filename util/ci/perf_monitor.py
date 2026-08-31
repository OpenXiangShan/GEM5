#!/usr/bin/env python3
"""Analyze one completed GEM5 performance CI run against a compatible baseline."""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
from typing import Any, Iterable


DEFAULT_REPO = "OpenXiangShan/GEM5"
DEFAULT_DATA_PROC = "/nfs/home/share/gem5_ci/gem5_data_proc"
DEFAULT_GROUPS = "basic,branch,frontend,intel_topdown"

# Archives created before archive_schema_version=2 did not record their profile
# inputs. Keep only the profile used by the two automatic post-merge workflows;
# new/manual archives use the exact paths recorded at benchmark time.
LEGACY_PROFILE_INPUTS = {
    "spec06-rva23-novec-gcc16-0.3c": {
        "checkpoint_list": (
            "/nfs/home/share/gem5_ci/spec06_cpts/gcc16_rva23_novec/"
            "spec06_0.3c.lst"
        ),
        "cluster_config": (
            "/nfs/home/share/checkpoints_profiles/"
            "spec06_gcc16_rva23_novec_260820/json/checkpoints_cov0.3.json"
        ),
    },
}

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

COMPATIBILITY_KEYS = (
    "config_path",
    "benchmark_type",
    "specific_benchmarks",
    "vector_type",
    "resolved_extra_args",
    "checkpoint_list",
    "cluster_config",
)

SUMMARY_ROWS = {"int_avg", "fp_avg", "overall_avg"}

COUNTER_COLUMNS = (
    "ipc",
    "cycles",
    "committedInsts",
    "frontendBound",
    "frontendLatencyBound",
    "frontendBandwidthBound",
    "badSpecBound",
    "branchMissPrediction",
    "backendBound",
    "coreBound",
    "memoryBound",
    "branch_MPKI",
    "cond_MPKI",
    "branch_mispredict_rate",
    "fetch_nisn_mean",
    "icacheStallCycles",
    "decodeStallRate",
    "fsqNotValid",
    "ftqNotValid",
)

DEFAULT_POLICY = {
    "overall_warning_abs_pct": 0.5,
    "overall_critical_regression_pct": -1.0,
    "workload_warning_abs_pct": 2.0,
    "workload_critical_regression_pct": -5.0,
    "max_counter_deltas_per_workload": 8,
    "max_baseline_candidates": 50,
}


class MonitorError(RuntimeError):
    pass


def _run(cmd: list[str], cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        cmd,
        cwd=str(cwd) if cwd else None,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )


def gh_json(repo: str, endpoint: str) -> Any:
    result = _run(["gh", "api", f"repos/{repo}/{endpoint}"])
    if result.returncode != 0:
        raise MonitorError(result.stdout.strip() or f"gh api failed: {endpoint}")
    return json.loads(result.stdout)


def get_run(repo: str, run_id: str) -> dict[str, Any]:
    return gh_json(repo, f"actions/runs/{run_id}")


def get_jobs(repo: str, run_id: str) -> list[dict[str, Any]]:
    data = gh_json(repo, f"actions/runs/{run_id}/jobs?per_page=100")
    return list(data.get("jobs", []))


def get_job_log(repo: str, job_id: int) -> str:
    result = _run(["gh", "api", f"repos/{repo}/actions/jobs/{job_id}/logs"])
    if result.returncode != 0:
        raise MonitorError(result.stdout.strip() or f"cannot read logs for job {job_id}")
    return result.stdout


def locate_archive(repo: str, run_id: str) -> tuple[Path, dict[str, Any]]:
    jobs = sorted(
        get_jobs(repo, run_id),
        key=lambda job: (
            "perf" not in str(job.get("name", "")).lower(),
            str(job.get("name", "")),
        ),
    )
    for job in jobs:
        log = get_job_log(repo, int(job["id"]))
        for pattern in ARCHIVE_PATTERNS:
            match = pattern.search(log)
            if match:
                return Path(match.group(1).strip()), job
    checked = ", ".join(str(job.get("name", job.get("id"))) for job in jobs)
    raise MonitorError(f"cannot find NFS archive path; checked jobs: {checked}")


def parse_metadata(path: Path) -> dict[str, str]:
    metadata_path = path / "metadata.txt"
    if not metadata_path.is_file():
        raise MonitorError(f"metadata is missing: {metadata_path}")
    metadata: dict[str, str] = {}
    for line in metadata_path.read_text(errors="replace").splitlines():
        key, separator, value = line.partition(":")
        if separator:
            metadata[key.strip()] = value.strip()
    metadata.setdefault("specific_benchmarks", "")
    metadata.setdefault("vector_type", "base")
    metadata.setdefault(
        "resolved_extra_args", metadata.get("extra_args", "").strip()
    )
    legacy_profile = LEGACY_PROFILE_INPUTS.get(metadata.get("benchmark_type", ""), {})
    metadata.setdefault("checkpoint_list", legacy_profile.get("checkpoint_list", ""))
    metadata.setdefault("cluster_config", legacy_profile.get("cluster_config", ""))
    return metadata


def metadata_compatible(candidate: dict[str, str], baseline: dict[str, str]) -> bool:
    return all(candidate.get(key, "") == baseline.get(key, "") for key in COMPATIBILITY_KEYS)


def is_xs_dev_push(run: dict[str, Any]) -> bool:
    return run.get("event") == "push" and run.get("head_branch") == "xs-dev"


def _timestamp(metadata: dict[str, str]) -> str:
    return metadata.get("timestamp", "")


def find_baseline(
    repo: str,
    candidate_path: Path,
    candidate_metadata: dict[str, str],
    explicit_run_id: str | None,
    max_candidates: int,
) -> tuple[Path, dict[str, str], dict[str, Any]] | None:
    if explicit_run_id:
        run = get_run(repo, explicit_run_id)
        if run.get("status") != "completed" or run.get("conclusion") != "success":
            raise MonitorError(f"explicit baseline run {explicit_run_id} is not successful")
        path, _ = locate_archive(repo, explicit_run_id)
        metadata = parse_metadata(path)
        if not metadata_compatible(candidate_metadata, metadata):
            raise MonitorError(f"explicit baseline run {explicit_run_id} is not compatible")
        return path, metadata, run

    candidate_timestamp = _timestamp(candidate_metadata)
    candidates: list[tuple[str, Path, dict[str, str]]] = []
    for path in candidate_path.parent.iterdir():
        if not path.is_dir() or path == candidate_path:
            continue
        try:
            metadata = parse_metadata(path)
        except MonitorError:
            continue
        timestamp = _timestamp(metadata)
        if candidate_timestamp and timestamp >= candidate_timestamp:
            continue
        if metadata.get("branch") != "xs-dev":
            continue
        if not metadata_compatible(candidate_metadata, metadata):
            continue
        candidates.append((timestamp, path, metadata))

    for _, path, metadata in sorted(candidates, reverse=True)[:max_candidates]:
        run_id = metadata.get("workflow_run_id")
        if not run_id:
            continue
        try:
            run = get_run(repo, run_id)
        except MonitorError:
            continue
        if (
            run.get("status") == "completed"
            and run.get("conclusion") == "success"
            and is_xs_dev_push(run)
            and archive_finalized(path, metadata)
            and not find_aborts(path)
        ):
            return path, metadata, run
    return None


def find_aborts(archive: Path) -> list[str]:
    spec_all = archive / "spec_all"
    if not spec_all.is_dir():
        return []
    aborts = []
    for path in spec_all.glob("*/abort"):
        aborts.append(path.parent.name)
    return sorted(aborts)


def failed_steps(jobs: Iterable[dict[str, Any]]) -> list[dict[str, str]]:
    failures = []
    for job in jobs:
        for step in job.get("steps", []):
            conclusion = str(step.get("conclusion", ""))
            if conclusion in {"failure", "cancelled", "timed_out"}:
                failures.append(
                    {
                        "job": str(job.get("name", "")),
                        "step": str(step.get("name", "")),
                        "conclusion": conclusion,
                    }
                )
    return failures


def source_run_was_skipped(run: dict[str, Any], jobs: list[dict[str, Any]]) -> bool:
    return run.get("conclusion") == "skipped" or (
        run.get("conclusion") == "success"
        and bool(jobs)
        and all(job.get("conclusion") == "skipped" for job in jobs)
    )


def run_data_proc(
    data_proc: Path,
    archive: Path,
    out_dir: Path,
    tag: str,
    groups: str,
    metadata: dict[str, str],
) -> dict[str, Any]:
    run_py = data_proc / "run.py"
    if not run_py.is_file():
        raise MonitorError(f"gem5_data_proc run.py is missing: {run_py}")
    out_dir.mkdir(parents=True, exist_ok=True)
    cmd = [
        sys.executable,
        str(run_py),
        str(archive),
        "--out-dir",
        str(out_dir),
        "--tag",
        tag,
        "-g",
        groups,
    ]
    cluster_config = metadata.get("cluster_config", "")
    if cluster_config and Path(cluster_config).is_file():
        cmd.extend(["-j", cluster_config])
    result = _run(cmd, cwd=data_proc)
    log_path = out_dir / f"{tag}-data-proc.log"
    log_path.write_text(result.stdout)
    if result.returncode != 0:
        raise MonitorError(
            f"gem5_data_proc failed for {tag}; see {log_path}:\n"
            + "\n".join(result.stdout.splitlines()[-20:])
        )
    paths = {
        "raw": out_dir / f"{tag}.csv",
        "weighted": out_dir / f"{tag}-weighted.csv",
        "score": out_dir / f"{tag}-score.csv",
        "log": log_path,
    }
    missing = [str(path) for path in paths.values() if not path.is_file()]
    if missing:
        raise MonitorError("gem5_data_proc output is missing: " + ", ".join(missing))
    warnings = [line for line in result.stdout.splitlines() if "warning:" in line.lower()]
    return {"paths": paths, "warnings": warnings}


def _float(value: str | None) -> float | None:
    if value is None or not value.strip():
        return None
    try:
        parsed = float(value)
    except ValueError:
        return None
    return parsed if math.isfinite(parsed) else None


def read_csv_rows(path: Path) -> dict[str, dict[str, str]]:
    with path.open(newline="") as stream:
        reader = csv.DictReader(stream)
        if not reader.fieldnames:
            raise MonitorError(f"CSV has no header: {path}")
        index_column = reader.fieldnames[0]
        rows = {}
        for row in reader:
            name = str(row.get(index_column, "")).strip()
            if name:
                rows[name] = row
        return rows


def score_completeness(
    candidate: dict[str, dict[str, str]],
    baseline: dict[str, dict[str, str]],
) -> dict[str, Any]:
    expected = sorted(name for name in baseline if name not in SUMMARY_ROWS)
    missing_rows = [name for name in expected if name not in candidate]
    invalid_scores = [
        name
        for name in expected
        if name in candidate and _float(candidate[name].get("score")) is None
    ]
    invalid_coverage = [
        name
        for name in expected
        if name in candidate and _float(candidate[name].get("coverage")) is None
    ]
    baseline_invalid_scores = [
        name for name in expected if _float(baseline[name].get("score")) is None
    ]
    baseline_invalid_coverage = [
        name for name in expected if _float(baseline[name].get("coverage")) is None
    ]
    coverage_mismatch = []
    for name in expected:
        if name not in candidate:
            continue
        lhs = _float(candidate[name].get("coverage"))
        rhs = _float(baseline[name].get("coverage"))
        if lhs is not None and rhs is not None and abs(lhs - rhs) > 1e-6:
            coverage_mismatch.append(
                {"workload": name, "baseline": rhs, "candidate": lhs}
            )
    complete = not (
        missing_rows
        or invalid_scores
        or invalid_coverage
        or baseline_invalid_scores
        or baseline_invalid_coverage
        or coverage_mismatch
    )
    return {
        "complete": complete,
        "expected_workloads": len(expected),
        "missing_rows": missing_rows,
        "invalid_scores": invalid_scores,
        "invalid_coverage": invalid_coverage,
        "baseline_invalid_scores": baseline_invalid_scores,
        "baseline_invalid_coverage": baseline_invalid_coverage,
        "coverage_mismatch": coverage_mismatch,
    }


def archive_finalized(archive: Path, metadata: dict[str, str]) -> bool:
    """Require the final marker for archives using the versioned schema."""
    try:
        schema_version = int(metadata.get("archive_schema_version", "1"))
    except ValueError:
        return False
    return schema_version < 2 or (archive / "PERF_COMPLETE").is_file()


def score_deltas(
    candidate: dict[str, dict[str, str]],
    baseline: dict[str, dict[str, str]],
) -> list[dict[str, Any]]:
    deltas = []
    for name in sorted(set(candidate) & set(baseline)):
        candidate_score = _float(candidate[name].get("score"))
        baseline_score = _float(baseline[name].get("score"))
        if candidate_score is None or baseline_score in {None, 0.0}:
            continue
        delta_pct = (candidate_score / baseline_score - 1.0) * 100.0
        deltas.append(
            {
                "workload": name,
                "baseline": baseline_score,
                "candidate": candidate_score,
                "delta_pct": delta_pct,
            }
        )
    return sorted(deltas, key=lambda item: abs(item["delta_pct"]), reverse=True)


def counter_deltas(
    candidate: dict[str, dict[str, str]],
    baseline: dict[str, dict[str, str]],
    workloads: Iterable[str],
    max_per_workload: int,
) -> list[dict[str, Any]]:
    result = []
    for workload in workloads:
        if workload not in candidate or workload not in baseline:
            continue
        changes = []
        for counter in COUNTER_COLUMNS:
            lhs = _float(baseline[workload].get(counter))
            rhs = _float(candidate[workload].get(counter))
            if lhs is None or rhs is None:
                continue
            delta_pct = None if abs(lhs) < 1e-12 else (rhs / lhs - 1.0) * 100.0
            changes.append(
                {
                    "counter": counter,
                    "baseline": lhs,
                    "candidate": rhs,
                    "absolute_delta": rhs - lhs,
                    "delta_pct": delta_pct,
                }
            )
        changes.sort(
            key=lambda item: abs(item["delta_pct"] or 0.0), reverse=True
        )
        result.append(
            {"workload": workload, "changes": changes[:max_per_workload]}
        )
    return result


def classify(
    run: dict[str, Any],
    completeness: dict[str, Any] | None,
    deltas: list[dict[str, Any]],
    aborts: list[str],
    policy: dict[str, Any],
) -> tuple[str, list[str]]:
    if run.get("conclusion") != "success":
        return "critical", [f"source workflow concluded {run.get('conclusion') or 'unknown'}"]
    if aborts:
        return "critical", [f"{len(aborts)} workload(s) contain abort markers"]
    if completeness is not None and not completeness.get("complete", False):
        return "critical", ["candidate/baseline data is incomplete or coverage differs"]

    reasons = []
    severity = "normal"
    by_name = {item["workload"]: item for item in deltas}
    overall = by_name.get("overall_avg")
    if overall:
        delta = overall["delta_pct"]
        if delta <= policy["overall_critical_regression_pct"]:
            severity = "critical"
            reasons.append(f"overall score regressed by {delta:.3f}%")
        elif abs(delta) >= policy["overall_warning_abs_pct"]:
            severity = "warning"
            reasons.append(f"overall score moved by {delta:+.3f}%")

    workload_changes = [item for item in deltas if item["workload"] not in SUMMARY_ROWS]
    critical = [
        item
        for item in workload_changes
        if item["delta_pct"] <= policy["workload_critical_regression_pct"]
    ]
    warnings = [
        item
        for item in workload_changes
        if abs(item["delta_pct"]) >= policy["workload_warning_abs_pct"]
    ]
    if critical:
        severity = "critical"
        reasons.append(
            f"{len(critical)} workload(s) exceeded the critical regression threshold"
        )
    elif warnings and severity == "normal":
        severity = "warning"
        reasons.append(f"{len(warnings)} workload(s) moved beyond the warning threshold")
    return severity, reasons


def _run_summary(
    run: dict[str, Any],
    metadata: dict[str, str] | None,
    archive: Path | None,
) -> dict[str, Any]:
    github_head_sha = str(run.get("head_sha", ""))
    tested_sha = str(metadata.get("commit", "")) if metadata else ""
    return {
        "run_id": str(run.get("id", "")),
        "url": str(run.get("html_url", "")),
        "workflow": str(run.get("name", "")),
        "status": str(run.get("status", "")),
        "conclusion": str(run.get("conclusion", "")),
        # Manual runs may checkout a commit different from the dispatch ref.
        # The archive records the commit that the benchmark actually tested.
        "head_sha": tested_sha or github_head_sha,
        "github_head_sha": github_head_sha,
        "archive": str(archive) if archive else None,
        "metadata": metadata,
    }


def render_markdown(analysis: dict[str, Any]) -> str:
    lines = ["# GEM5 performance CI analysis", ""]
    lines.append(f"- Severity: **{analysis['severity']}**")
    candidate = analysis["candidate"]
    lines.append(f"- Source run: [{candidate['run_id']}]({candidate['url']})")
    lines.append(f"- Workflow conclusion: `{analysis['candidate']['conclusion']}`")
    if analysis.get("baseline"):
        baseline = analysis["baseline"]
        lines.append(f"- Baseline run: [{baseline['run_id']}]({baseline['url']})")
    else:
        lines.append("- Baseline run: not found")
    if analysis.get("reasons"):
        lines.extend(["", "## Reasons", ""])
        lines.extend(f"- {reason}" for reason in analysis["reasons"])
    completeness = analysis.get("completeness")
    if completeness:
        lines.extend(["", "## Completeness", ""])
        lines.append(f"- Complete: `{completeness['complete']}`")
        lines.append(f"- Expected workloads: {completeness['expected_workloads']}")
        for key in (
            "missing_rows",
            "invalid_scores",
            "invalid_coverage",
            "baseline_invalid_scores",
            "baseline_invalid_coverage",
            "baseline_aborts",
        ):
            values = completeness.get(key, [])
            if values:
                lines.append(f"- {key}: {', '.join(values)}")
        if completeness.get("coverage_mismatch"):
            lines.append(f"- coverage_mismatch: {len(completeness['coverage_mismatch'])}")
    deltas = analysis.get("score_deltas", [])
    if deltas:
        lines.extend(
            [
                "",
                "## Largest score movements",
                "",
                "| Workload | Baseline | Candidate | Change |",
                "|---|---:|---:|---:|",
            ]
        )
        for item in deltas[:12]:
            lines.append(
                f"| {item['workload']} | {item['baseline']:.3f} | "
                f"{item['candidate']:.3f} | {item['delta_pct']:+.3f}% |"
            )
    failed = analysis.get("failed_steps", [])
    if failed:
        lines.extend(["", "## Failed steps", ""])
        lines.extend(
            f"- `{item['job']}` / `{item['step']}`: `{item['conclusion']}`"
            for item in failed
        )
    return "\n".join(lines) + "\n"


def write_github_outputs(analysis: dict[str, Any]) -> None:
    output_path = os.environ.get("GITHUB_OUTPUT")
    if not output_path:
        return
    with open(output_path, "a") as stream:
        stream.write(f"severity={analysis['severity']}\n")
        stream.write(
            "should_investigate="
            + ("true" if analysis["severity"] in {"warning", "critical"} else "false")
            + "\n"
        )
        baseline = analysis.get("baseline") or {}
        stream.write(f"baseline_run_id={baseline.get('run_id', '')}\n")


def analyze(args: argparse.Namespace) -> dict[str, Any]:
    policy = dict(DEFAULT_POLICY)
    if args.policy:
        policy.update(json.loads(Path(args.policy).read_text()))

    run = get_run(args.repo, args.run_id)
    if run.get("status") != "completed":
        raise MonitorError(f"source run {args.run_id} is not completed")
    jobs = get_jobs(args.repo, args.run_id)
    skipped = source_run_was_skipped(run, jobs)

    archive = None
    metadata = None
    if not skipped:
        try:
            archive, _ = locate_archive(args.repo, args.run_id)
            metadata = parse_metadata(archive)
            recorded_run_id = metadata.get("workflow_run_id")
            if recorded_run_id and recorded_run_id != str(args.run_id):
                raise MonitorError(
                    f"archive belongs to run {recorded_run_id}, not source run {args.run_id}"
                )
        except MonitorError:
            if run.get("conclusion") == "success":
                raise

    analysis: dict[str, Any] = {
        "schema_version": 1,
        "severity": "normal",
        "reasons": [],
        "candidate": _run_summary(run, metadata, archive),
        "baseline": None,
        "failed_steps": failed_steps(jobs),
        "aborts": find_aborts(archive) if archive else [],
        "completeness": None,
        "score_deltas": [],
        "counter_deltas": [],
        "data_proc_warnings": {},
        "policy": policy,
    }

    if skipped:
        analysis["ignored"] = True
        analysis["reasons"] = ["source workflow contained no executed benchmark job"]
        return analysis

    if run.get("conclusion") != "success":
        analysis["severity"], analysis["reasons"] = classify(
            run, None, [], analysis["aborts"], policy
        )
        return analysis

    assert archive is not None and metadata is not None
    baseline_info = find_baseline(
        args.repo,
        archive,
        metadata,
        args.baseline_run_id,
        int(policy["max_baseline_candidates"]),
    )
    if baseline_info is None:
        analysis["severity"] = "warning"
        analysis["reasons"] = ["no earlier successful compatible xs-dev baseline was found"]
        return analysis

    baseline_path, baseline_metadata, baseline_run = baseline_info
    analysis["baseline"] = _run_summary(
        baseline_run, baseline_metadata, baseline_path
    )

    data_proc = Path(args.gem5_data_proc_home)
    candidate_proc = run_data_proc(
        data_proc, archive, Path(args.output_dir) / "candidate", "candidate", args.groups, metadata
    )
    baseline_proc = run_data_proc(
        data_proc,
        baseline_path,
        Path(args.output_dir) / "baseline",
        "baseline",
        args.groups,
        baseline_metadata,
    )
    analysis["data_proc_warnings"] = {
        "candidate": candidate_proc["warnings"][:50],
        "baseline": baseline_proc["warnings"][:50],
    }

    candidate_scores = read_csv_rows(candidate_proc["paths"]["score"])
    baseline_scores = read_csv_rows(baseline_proc["paths"]["score"])
    analysis["completeness"] = score_completeness(candidate_scores, baseline_scores)
    analysis["completeness"]["archive_finalized"] = archive_finalized(
        archive, metadata
    )
    analysis["completeness"]["baseline_archive_finalized"] = archive_finalized(
        baseline_path, baseline_metadata
    )
    analysis["completeness"]["baseline_aborts"] = find_aborts(baseline_path)
    if (
        not analysis["completeness"]["archive_finalized"]
        or not analysis["completeness"]["baseline_archive_finalized"]
        or analysis["completeness"]["baseline_aborts"]
    ):
        analysis["completeness"]["complete"] = False
    analysis["score_deltas"] = score_deltas(candidate_scores, baseline_scores)

    interesting = [
        item["workload"]
        for item in analysis["score_deltas"]
        if item["workload"] not in SUMMARY_ROWS
        and abs(item["delta_pct"]) >= policy["workload_warning_abs_pct"]
    ][:8]
    candidate_weighted = read_csv_rows(candidate_proc["paths"]["weighted"])
    baseline_weighted = read_csv_rows(baseline_proc["paths"]["weighted"])
    analysis["counter_deltas"] = counter_deltas(
        candidate_weighted,
        baseline_weighted,
        interesting,
        int(policy["max_counter_deltas_per_workload"]),
    )
    analysis["severity"], analysis["reasons"] = classify(
        run,
        analysis["completeness"],
        analysis["score_deltas"],
        analysis["aborts"],
        policy,
    )
    return analysis


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--baseline-run-id")
    parser.add_argument("--repo", default=DEFAULT_REPO)
    parser.add_argument("--gem5-data-proc-home", default=DEFAULT_DATA_PROC)
    parser.add_argument("--groups", default=DEFAULT_GROUPS)
    parser.add_argument("--policy")
    parser.add_argument("--output-dir", required=True)
    args = parser.parse_args()

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    try:
        analysis = analyze(args)
    except Exception as error:
        analysis = {
            "schema_version": 1,
            "severity": "critical",
            "reasons": [f"monitor failed: {error}"],
            "candidate": {
                "run_id": args.run_id,
                "url": f"https://github.com/{args.repo}/actions/runs/{args.run_id}",
                "conclusion": "monitor_error",
            },
            "baseline": None,
            "failed_steps": [],
            "aborts": [],
            "completeness": None,
            "score_deltas": [],
            "counter_deltas": [],
            "data_proc_warnings": {},
            "policy": dict(DEFAULT_POLICY),
        }
    (output_dir / "analysis.json").write_text(
        json.dumps(analysis, indent=2, sort_keys=True) + "\n"
    )
    (output_dir / "summary.md").write_text(render_markdown(analysis))
    write_github_outputs(analysis)
    print(json.dumps({"severity": analysis["severity"], "reasons": analysis["reasons"]}))


if __name__ == "__main__":
    main()
