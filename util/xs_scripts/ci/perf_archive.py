"""Create performance archives and admit completed workloads to scoring."""
from datetime import datetime
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import time

ARCHIVES = Path("/nfs/home/share/gem5_ci/performance_data")
ARTIFACTS = {
    "kmhv2": "score",
    "kmhv3": "score",
    "ideal": "score-upperbound",
    "idealkmhv3": "score-ideal",
    "smt_idealkmhv3": "score-smt-ideal",
}


def append(path, message):
    if path:
        with open(path, "a") as output:
            print(message, file=output)


def create_archive(args, repo, profile, selected, extra, ref, nodes):
    commit, short = subprocess.check_output(
        ["git", "-C", repo, "show", "-s", "--format=%H %h", "HEAD"], text=True
    ).split()
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    number = os.environ.get("GITHUB_RUN_NUMBER", "0")
    name = re.sub(r"[^A-Za-z0-9._-]", "_", Path(args.config_path).stem)
    parent = args.archive_root.resolve() / (
        profile.archive_subdir or profile.benchmark_type
    )
    directory = parent / f"{stamp}_{short}_{name}_run{number}"
    directory.mkdir(parents=True, exist_ok=True)
    metadata = dict(
        timestamp=stamp,
        commit=commit,
        commit_short=short,
        config_path=args.config_path,
        config_name=name,
        run_number=number,
        branch=os.getenv("GITHUB_REF_NAME", ""),
        workflow_run_id=os.getenv("GITHUB_RUN_ID", ""),
        benchmark_type=profile.benchmark_type,
        checkpoint_root=profile.root,
        extra_args=shlex.join(extra),
        ref_so=ref or "",
        distributed_servers=",".join(nodes),
    )
    with (directory / "metadata.txt").open("x") as output:
        for key, value in metadata.items():
            print(f"{key}: {value}", file=output)
    print(f"Archiving performance data to {directory}", flush=True)
    append(
        os.getenv("GITHUB_STEP_SUMMARY"),
        f"### Live archive output\n- Path: `{directory}`",
    )
    (directory / "checkpoints.lst").write_text(
        "".join(w.row + "\n" for w in selected)
    )
    if profile.weights:
        shutil.copyfile(profile.weights, directory / "cluster.json")
    if ref and (ref.parent / "manifest.json").is_file():
        shutil.copyfile(
            ref.parent / "manifest.json", directory / "ref-manifest.json"
        )
    prune(parent)
    outputs = dict(
        archive=directory,
        artifact_name=f"{ARTIFACTS.get(name, 'score-unknown')}-{args.benchmark_type}",
        score_script=profile.score_script,
        trace_format=profile.trace_format,
        benchmark_type=profile.benchmark_type,
    )
    append(
        os.getenv("GITHUB_OUTPUT"),
        "\n".join(f"{k}={v}" for k, v in outputs.items()),
    )
    return directory


def complete_stats(path):
    try:
        with path.open("rb") as f:
            f.seek(max(0, os.fstat(f.fileno()).st_size - 256))
            return (
                f.read().rstrip().split(b"\n")[-1].strip(b"- ")
                == b"End Simulation Statistics"
            )
    except OSError:
        return False


def verify_results(directory, results, trace_format, strict, timeout=60):
    stats = "stats.txt" if trace_format else "m5out/stats.txt"
    pending = [r for r in results if r.status == "completed"]
    passed = []
    deadline = time.monotonic() + timeout
    while pending:
        waiting = []
        for r in pending:
            if complete_stats(r.job.work_dir / stats):
                passed.append(r.job.workload_name)
            else:
                waiting.append(r)
        pending = waiting
        remaining = deadline - time.monotonic()
        if not pending or remaining <= 0:
            break
        time.sleep(min(1, remaining))
    issues = [
        f"- {r.job.workload_name}: {r.status} on {r.server_name} (exit={r.return_code})"
        for r in results
        if r.status != "completed"
    ]
    issues.extend(
        f"- {r.job.workload_name}: exit=0 on {r.server_name}, archive stats missing or incomplete"
        for r in pending
    )
    issues.sort()
    coverage = f"Scorable {len(passed)}/{len(results)} selected workloads."
    print(coverage, *issues, sep="\n", flush=True)
    append(
        directory / "metadata.txt",
        "\n".join(["coverage: " + coverage, *issues]),
    )
    summary = issues[:20]
    if len(issues) > 20:
        summary.append(
            f"See metadata.txt for all {len(issues)} affected workloads."
        )
    append(os.getenv("GITHUB_STEP_SUMMARY"), "\n".join([coverage, *summary]))
    (directory / "passed.lst").write_text(
        "".join(name + "\n" for name in sorted(passed))
    )
    scorable = bool(passed) and (not issues or not (strict or trace_format))
    partial = f"Partial score. {coverage}" if scorable and issues else ""
    append(os.getenv("GITHUB_OUTPUT"), f"partial={partial}")
    return 0 if scorable else 1


def prune(parent):
    runs = sorted(
        p
        for p in parent.glob("*_run[0-9]*")
        if p.is_dir()
        and not p.is_symlink()
        and re.fullmatch(r"\d{8}_\d{6}_[0-9a-f]+_.*run\d+", p.name)
    )
    cutoff = time.time() - 86400
    for old in runs[:-200]:
        try:
            if old.stat().st_mtime < cutoff:
                shutil.rmtree(old)
        except FileNotFoundError:
            pass
        except OSError as error:
            print(f"Could not prune {old}: {error}", flush=True)
