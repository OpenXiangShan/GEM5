"""Run a performance profile and publish its verified scoring inputs."""
import argparse
from dataclasses import replace
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

from .perf_archive import ARCHIVES, create_archive, verify_results
from .benchmarks import CHECKPOINT_MODES, resolve_benchmark, workloads
from .runner import (
    CHECKPOINT_ENV,
    Job,
    RunnerConfig,
    cancellation_signals,
    run_jobs,
    simulation_env,
)


def run(args):
    repo = args.repo.resolve()
    profile = resolve_benchmark(
        args.benchmark_type, args.checkpoint_path, args.json_path
    )
    selected = list(workloads(profile, args.specific_benchmarks))
    cmd, extra, env, ref = prepare_command(repo, profile, args)
    if min(args.distributed_jobs_per_server, args.local_jobs) < 1:
        raise ValueError("parallelism must be positive")
    servers = args.distributed_servers.strip()
    slots = args.distributed_jobs_per_server if servers else args.local_jobs
    config = RunnerConfig(
        servers=servers, jobs_per_server=slots, probe_interval=0
    )
    nodes = config.names()
    if args.dry_run:
        print(
            f"{profile.benchmark_type}: {len(selected)} workloads; hosts={','.join(nodes)}"
        )
        print(
            shlex.join(
                cmd + selected[0].arguments(profile.trace_format) + extra
            )
        )
        return 0

    directory = create_archive(
        args, repo, profile, selected, extra, ref, nodes
    )
    jobs = [
        Job(
            "",
            w.name,
            w.image,
            directory / "spec_all" / w.name,
            cmd + w.arguments(profile.trace_format) + extra,
            env,
        )
        for w in selected
    ]
    config = replace(
        config,
        ssh_options=(
            "StrictHostKeyChecking=accept-new",
            f"UserKnownHostsFile={directory}/known_hosts",
            "ConnectTimeout=10",
        ),
    )
    with cancellation_signals() as event, (directory / "launcher.log").open(
        "w", buffering=1
    ) as launcher:

        def log(line):
            print(line, flush=True)
            print(line, file=launcher)

        results = run_jobs(
            jobs,
            config,
            slots * len(nodes),
            fallback_to_local=args.local_jobs,
            cancelled=event.is_set,
            log=log,
        )
    if event.is_set():
        return 130
    return verify_results(
        directory, results, profile.trace_format, args.check_result
    )


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
    if config.name == "smt_idealkmhv3.py":
        if mode_name in ("h", "single-32g"):
            raise ValueError(
                f"{profile.benchmark_type} requires a single-thread configuration"
            )
        mode_name = "smt"
    mode = CHECKPOINT_MODES[mode_name]
    custom_ref = mode.ref_env == "GCBV_REF_SO" and args.gcbv_ref_so
    if custom_ref:
        ref = Path(args.gcbv_ref_so)
    elif isinstance(mode.reference, Path):
        ref = mode.reference
    else:
        resolver = repo / "util/nemu_ref/resolve.py"
        output = subprocess.check_output(
            [sys.executable, resolver, mode.reference], text=True
        )
        ref = Path(output.strip())
    if not ref.is_absolute() or not ref.is_file():
        raise ValueError(f"unreadable reference: {ref}")
    env[mode.ref_env] = str(ref)
    if mode_name == "smt":
        # Multi-context configs require the key even for an embedded restorer.
        env["GCB_MULTI_CORE_RESTORER"] = ""
    extra.extend(
        arg.format(ref=ref, repo=repo)
        for arg in mode.extra_args
        if not (custom_ref and arg == "--enable-mem-dedup")
    )
    return cmd + [str(config)], extra, env, ref


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in (
        "benchmark-type",
        "config-path",
        "checkpoint-path",
        "json-path",
        "specific-benchmarks",
        "extra-args",
        "gcbv-ref-so",
        "distributed-servers",
    ):
        parser.add_argument("--" + flag, default="")
    parser.add_argument(
        "--check-result", action=argparse.BooleanOptionalAction, default=True
    )
    parser.add_argument("--distributed-jobs-per-server", type=int, default=32)
    parser.add_argument(
        "--local-jobs",
        type=int,
        default=int(os.getenv("xsgem5_para_jobs", 63)),
    )
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    parser.add_argument("--archive-root", type=Path, default=ARCHIVES)
    parser.add_argument("--build-type", default="fast")
    parser.add_argument("--dry-run", action="store_true")
    parser.set_defaults(**json.loads(os.getenv("PERF_INPUTS", "{}")))
    return run(parser.parse_args())


if __name__ == "__main__":
    sys.exit(main())
