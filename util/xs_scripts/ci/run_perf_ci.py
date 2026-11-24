"""Run one performance profile from the CLI or a reusable Actions workflow."""
from __future__ import annotations

import argparse
from collections import defaultdict
from contextlib import contextmanager, redirect_stdout
from dataclasses import replace
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time

from . import hosts, stats_probe

from .benchmarks import CHECKPOINT_MODES, resolve_benchmark, workloads
from .runner import (
    CHECKPOINT_ENV,
    Job,
    RunnerConfig,
    cancellation_signals,
    run_jobs,
    simulation_env,
)
from .trace_report import summarize_trace_results


ARCHIVES = Path("/nfs/home/share/gem5_ci/performance_data")
SCORER = Path("/nfs/home/share/gem5_ci/gem5_data_proc")


def git(repo, *args):
    return subprocess.check_output(
        ["git", "-C", repo, *args], text=True
    ).strip()


def append(path, message):
    if path:
        with open(path, "a") as output:
            print(message, file=output)


def prepare_command(repo, profile, args):
    config = Path(args.config_path)
    if config.is_absolute() or ".." in config.parts or config.suffix != ".py":
        raise ValueError(f"expected repo-relative Python config: {config}")
    config = repo / config
    if not config.is_file():
        raise FileNotFoundError(config)
    extra = [
        a.replace("${GEM5_HOME}", str(repo)).replace("$GEM5_HOME", str(repo))
        for a in shlex.split(args.extra_args)
    ]
    env = simulation_env()
    env.update(GEM5_HOME=str(repo), GEM5_BUILD_TYPE=args.build_type)
    for key in CHECKPOINT_ENV:
        env.pop(key, None)
    binary = repo / "build/RISCV" / f"gem5.{args.build_type}"
    cmd = [str(binary)]
    if profile.trace_format:
        cmd.extend(["--outdir=.", "--stats-file=stats.txt"])
        return cmd + [str(config)], extra, env, None
    mode_name = profile.mode
    topology = argparse.ArgumentParser(add_help=False, allow_abbrev=False)
    topology.add_argument("--smt", action="store_true")
    topology.add_argument("-n", "--num-cpus", type=int, default=1)
    shape, _ = topology.parse_known_args(extra)
    if config.name == "smt_idealkmhv3.py" or shape.smt or shape.num_cpus > 1:
        if mode_name == "h" or profile.single_core_ref:
            raise ValueError(
                f"{profile.benchmark_type} requires a single-thread configuration"
            )
        mode_name = "smt"
    mode = CHECKPOINT_MODES[mode_name]
    custom_ref = mode_name == "single" and args.gcbv_ref_so
    ref_path = custom_ref or profile.single_core_ref
    if ref_path:
        ref = Path(ref_path)
    else:
        ref = Path(
            subprocess.check_output(
                [
                    sys.executable,
                    repo / "util/nemu_ref/resolve.py",
                    mode.variant,
                ],
                text=True,
            ).strip()
        )
    if not ref.is_absolute() or not ref.is_file():
        raise ValueError(f"unreadable reference: {ref}")
    env[mode.ref_env] = str(ref)
    if mode_name == "smt":
        # Multi-context configs require the key even for an embedded restorer.
        env["GCB_MULTI_CORE_RESTORER"] = ""
    if not custom_ref:
        extra.extend(arg.format(ref=ref, repo=repo) for arg in mode.extra_args)
    extra.extend(profile.extra_args)
    return cmd + [str(config)], extra, env, ref


def score(profile, directory, repo, passed, partial=""):
    score_path = directory / "score.txt"
    if profile.trace_format:
        with score_path.open("w") as output, redirect_stdout(output):
            return summarize_trace_results(
                profile, directory / "spec_all", passed
            )
    with tempfile.TemporaryDirectory() as tmp:
        scorer = Path(tmp) / "gem5_data_proc"
        shutil.copytree(
            SCORER,
            scorer,
            symlinks=True,
            ignore=shutil.ignore_patterns(".git", "results"),
        )
        python_path = os.pathsep.join(
            [
                str(scorer),
                "/nfs/home/yanyue/.local/lib/python3.12/site-packages",
                os.environ.get("PYTHONPATH", ""),
            ]
        )
        # The external scorer discovers every stats.txt, including aborted runs.
        inputs = Path(tmp) / "spec_all"
        inputs.mkdir()
        for name in passed:
            (inputs / name).symlink_to(
                directory / "spec_all" / name, target_is_directory=True
            )
        with score_path.open("w") as output:
            if partial:
                print(partial, file=output, flush=True)
            return subprocess.run(
                [
                    "bash",
                    "-e",
                    scorer / "example-scripts" / profile.score_script,
                    inputs,
                    directory / "cluster.json",
                ],
                cwd=scorer,
                env={
                    **os.environ,
                    "GEM5_HOME": str(repo),
                    "PYTHONPATH": python_path,
                },
                stdout=output,
            ).returncode


def prune(parent):
    runs = sorted(
        p
        for p in parent.iterdir()
        if not p.is_symlink()
        and p.is_dir()
        and re.fullmatch(r"\d{8}_\d{6}_[0-9a-f]+_.*run\d+", p.name)
        and (p / "metadata.txt").is_file()
        and (
            (p / "finished").is_file()
            or (
                not (p / "launcher.active").exists()
                and (p / "score.txt").is_file()
            )
        )
    )
    for old in runs[:-200]:
        shutil.rmtree(old, ignore_errors=True)


@contextmanager
def archive(parent, prefix, number):
    attempt = 1
    while True:
        tag = "" if attempt == 1 else f"_attempt{attempt}"
        directory = parent / f"{prefix}{tag}_run{number}"
        try:
            directory.mkdir(parents=True)
            break
        except FileExistsError:
            attempt += 1
    active = directory / "launcher.active"
    active.touch()
    try:
        yield directory
    finally:
        active.rename(directory / "finished")
        prune(parent)


def worker_stats(node, paths, config):
    # Batch exceptional paths into one read-only SSH request per worker.
    command = hosts.build_ssh_command(
        hosts.make_ssh_target(node, config.ssh_user),
        ["python3", "-c", Path(stats_probe.__file__).read_text()],
        config.ssh_config,
        list(config.ssh_options),
        ["BatchMode=yes", "ConnectionAttempts=1"],
    )
    try:
        response = subprocess.run(
            command,
            input=json.dumps([str(p) for p in paths]),
            text=True,
            capture_output=True,
            check=True,
            timeout=10,
        )
        states = json.loads(response.stdout)
        if (
            not isinstance(states, list)
            or len(states) != len(paths)
            or not all(isinstance(state, str) for state in states)
        ):
            raise ValueError("invalid worker stats response")
        return states
    except (OSError, subprocess.SubprocessError, ValueError) as error:
        print(
            f"Stats probe on {node} failed ({type(error).__name__})",
            flush=True,
        )
        return ["unconfirmed"] * len(paths)


def check_archive(results, stats_path, config, timeout=60):
    """Return scoring inputs and details for runs whose output is unavailable."""
    pending = {
        r.job.workload_name: r for r in results if r.status == "completed"
    }
    ready = set()
    deadline = time.monotonic() + timeout
    while pending:
        for name, result in list(pending.items()):
            if (
                stats_probe.stats_state(result.job.work_dir / stats_path)
                == "complete"
            ):
                ready.add(name)
                del pending[name]
        remaining = deadline - time.monotonic()
        if not pending or remaining <= 0:
            break
        time.sleep(min(1, remaining))

    by_node = defaultdict(list)
    for name, result in pending.items():
        by_node[result.server_name].append(name)
    issues = {}
    for node, names in by_node.items():
        paths = [pending[name].job.work_dir / stats_path for name in names]
        remote = worker_stats(node, paths, config) if node != "local" else None
        for idx, (name, path) in enumerate(zip(names, paths)):
            state = stats_probe.stats_state(path)
            if state == "complete":
                ready.add(name)
                continue
            detail = f"exit=0 on {node}, archive stats {state}"
            if remote is not None:
                detail += f", worker stats {remote[idx]}"
            issues[name] = detail
    return ready, issues


def finish_perf(profile, directory, repo, results, config, strict, summary):
    stats_path = "stats.txt" if profile.trace_format else "m5out/stats.txt"
    ready, issues = check_archive(results, stats_path, config)
    for result in results:
        if result.status != "completed":
            issues[
                result.job.workload_name
            ] = f"{result.status} on {result.server_name} (exit={result.return_code})"
    coverage = f"Scorable {len(ready)}/{len(results)} selected workloads."
    details = [f"- {name}: {issues[name]}" for name in sorted(issues)]
    print(coverage, *details, sep="\n", flush=True)
    append(
        directory / "metadata.txt",
        "\n".join([f"coverage: {coverage}", *details]),
    )
    append(summary, "\n".join([coverage, *details[:20]]))
    if len(details) > 20:
        append(
            summary,
            f"See metadata.txt for all {len(details)} affected workloads.",
        )
    if not ready or (issues and (strict or profile.trace_format)):
        append(
            summary,
            "Score withheld because the selected workload set is incomplete.",
        )
        return 1
    partial = f"Partial score. {coverage}" if issues else ""
    status = score(profile, directory, repo, ready, partial)
    lines = (directory / "score.txt").read_text(errors="replace").splitlines()
    tail = lines if profile.trace_format else lines[-42:]
    append(
        summary,
        (
            "### Partial performance result"
            if issues
            else "### Performance result"
        )
        + "\n```\n"
        + "\n".join(tail)
        + "\n```",
    )
    return status


def run(args):
    repo = args.repo.resolve()
    profile = resolve_benchmark(
        args.benchmark_type, args.checkpoint_path, args.json_path
    )
    selected = list(workloads(profile, args.specific_benchmarks))
    cmd, extra, env, ref = prepare_command(repo, profile, args)
    config = RunnerConfig(
        servers=args.distributed_servers,
        jobs_per_server=args.distributed_jobs_per_server,
        probe_interval=0,
    )
    if args.distributed_jobs_per_server < 1 or args.local_jobs < 1:
        raise ValueError("parallelism must be positive")
    nodes = config.names()
    trace_format = profile.trace_format
    if args.dry_run:
        print(
            f"{profile.benchmark_type}: {len(selected)} workloads; hosts={','.join(nodes)}"
        )
        print(shlex.join(cmd + selected[0].arguments(trace_format) + extra))
        return 0

    commit = git(repo, "rev-parse", "HEAD")
    short = git(repo, "rev-parse", "--short", "HEAD")
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    number = os.environ.get("GITHUB_RUN_NUMBER", "0")
    name = re.sub(r"[^A-Za-z0-9._-]", "_", Path(args.config_path).stem)
    parent = args.archive_root.resolve() / (
        profile.archive_subdir or profile.benchmark_type
    )
    with archive(parent, f"{stamp}_{short}_{name}", number) as directory:
        print(f"Archiving performance data to {directory}", flush=True)
        listing = directory / "checkpoints.lst"
        listing.write_text("".join(w.row + "\n" for w in selected))
        if profile.weights:
            shutil.copyfile(profile.weights, directory / "cluster.json")
        summary = os.environ.get("GITHUB_STEP_SUMMARY")
        append(summary, f"### Live archive output\n- Path: `{directory}`")
        artifact_prefix = {
            "kmhv2": "score",
            "kmhv3": "score",
            "idealkmhv3": "score-ideal",
            "smt_idealkmhv3": "score-smt-ideal",
        }.get(name, "score-unknown")
        append(
            os.environ.get("GITHUB_OUTPUT"),
            f"artifact_name={artifact_prefix}-{args.benchmark_type}\narchive={directory}",
        )
        metadata = dict(
            timestamp=stamp,
            commit=commit,
            commit_short=short,
            config_path=args.config_path,
            config_name=name,
            run_number=number,
            benchmark_type=profile.benchmark_type,
            checkpoint_root=profile.root,
            checkpoint_list=listing,
            cluster_config=profile.weights or "",
            specific_benchmarks=args.specific_benchmarks,
            extra_args=args.extra_args,
            resolved_extra_args=shlex.join(extra),
            distributed_servers_input=config.servers,
            distributed_servers=",".join(nodes) if config.servers else "",
            distributed_jobs_per_server=config.jobs_per_server,
            distributed_idle_probe_mode=config.idle_probe_mode,
            distributed_require_idle_cpus=config.jobs_per_server,
            branch=os.environ.get("GITHUB_REF_NAME", ""),
            workflow_run_id=os.environ.get("GITHUB_RUN_ID", ""),
            workflow_sha=os.environ.get("PERF_WORKFLOW_SHA", ""),
        )
        if ref:
            metadata["ref_so"] = str(ref)
            if profile.mode == "h":
                metadata["profile_ref_so"] = ref
            manifest = ref.parent / "manifest.json"
            if manifest.is_file():
                shutil.copyfile(manifest, directory / "ref-manifest.json")
        with (directory / "metadata.txt").open("w") as output:
            for key, value in metadata.items():
                print(f"{key}: {value}", file=output)
            if ref:
                print(
                    f"{hashlib.sha256(ref.read_bytes()).hexdigest()}  {ref}",
                    file=output,
                )

        config = replace(
            config,
            ssh_options=(
                "StrictHostKeyChecking=accept-new",
                f"UserKnownHostsFile={directory}/known_hosts",
                "ConnectTimeout=10",
            ),
        )
        jobs = [
            Job(
                "",
                w.name,
                w.image,
                directory / "spec_all" / w.name,
                cmd + w.arguments(trace_format) + extra,
                env,
            )
            for w in selected
        ]
        remote = config.enabled()
        slots = (
            len(nodes) * config.jobs_per_server if remote else args.local_jobs
        )
        if not remote:
            config = replace(config, jobs_per_server=slots)
        with cancellation_signals() as event:
            with (directory / "launcher.log").open(
                "w", buffering=1
            ) as launcher:

                def log(line):
                    print(line, flush=True)
                    print(line, file=launcher)

                results = run_jobs(
                    jobs,
                    config,
                    slots,
                    fallback_to_local=args.local_jobs,
                    cancelled=event.is_set,
                    log=log,
                )
        if event.is_set():
            return 130
        return finish_perf(
            profile,
            directory,
            repo,
            results,
            config,
            args.check_result,
            summary,
        )


def main():
    # The reusable workflow passes its typed inputs without shell interpolation.
    defaults = json.loads(os.environ.get("PERF_INPUTS", "{}"))
    parser = argparse.ArgumentParser(description=__doc__)
    for field in (
        "benchmark_type",
        "config_path",
        "checkpoint_path",
        "json_path",
        "specific_benchmarks",
        "extra_args",
        "gcbv_ref_so",
        "distributed_servers",
    ):
        parser.add_argument(
            "--" + field.replace("_", "-"), default=defaults.get(field, "")
        )
    parser.add_argument(
        "--check-result",
        action=argparse.BooleanOptionalAction,
        default=defaults.get("check_result", True),
    )
    parser.add_argument(
        "--distributed-jobs-per-server",
        type=int,
        default=defaults.get("distributed_jobs_per_server", 32),
    )
    parser.add_argument(
        "--local-jobs",
        type=int,
        default=int(os.environ.get("xsgem5_para_jobs", 63)),
    )
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    parser.add_argument("--archive-root", type=Path, default=ARCHIVES)
    parser.add_argument("--build-type", default="fast")
    parser.add_argument("--dry-run", action="store_true")
    return run(parser.parse_args())


if __name__ == "__main__":
    sys.exit(main())
