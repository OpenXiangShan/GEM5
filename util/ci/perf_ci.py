#!/usr/bin/env python3

"""Local implementation behind the reusable performance workflow."""

import argparse
import datetime
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path


REGISTRY_PATH = Path(__file__).with_name("perf_benchmarks.json")
DEFAULT_DISTRIBUTED_SERVERS = "node020-node034,node036-node039"
ARCHIVE_KEEP_COUNT = 200
REQUIRED_PROFILE_FIELDS = {
    "checkpoint_root",
    "score_script",
    "cluster_config",
    "artifact_name",
    "description",
}
H_PROFILE_FIELDS = {
    "expected_checkpoints",
    "label",
    "maxinsts",
    "ref_so",
    "restorer",
    "selection_json",
}


def load_profile(benchmark_type: str, registry_path: Path = REGISTRY_PATH) -> dict:
    with registry_path.open(encoding="utf-8") as registry_file:
        registry = json.load(registry_file)

    try:
        profile = dict(registry[benchmark_type])
    except KeyError as exc:
        choices = ", ".join(sorted(registry))
        raise ValueError(
            f"unknown benchmark_type {benchmark_type!r}; choose one of: {choices}"
        ) from exc

    missing = REQUIRED_PROFILE_FIELDS - profile.keys()
    if missing:
        raise ValueError(
            f"benchmark profile {benchmark_type!r} is missing: "
            + ", ".join(sorted(missing))
        )
    if not ({"checkpoint_list", "checkpoint_list_json"} & profile.keys()):
        raise ValueError(
            f"benchmark profile {benchmark_type!r} needs a checkpoint list"
        )

    profile.setdefault("execution_mode", "normal")
    if profile["execution_mode"] not in {"normal", "h"}:
        raise ValueError(
            f"benchmark profile {benchmark_type!r} has invalid execution_mode: "
            f"{profile['execution_mode']!r}"
        )
    if profile["execution_mode"] == "h":
        missing = H_PROFILE_FIELDS - profile.keys()
        if missing:
            raise ValueError(
                f"H benchmark profile {benchmark_type!r} is missing: "
                + ", ".join(sorted(missing))
            )
    profile["benchmark_type"] = benchmark_type
    return profile


def generate_checkpoint_list(source_path: Path, output_path: Path) -> None:
    with source_path.open(encoding="utf-8") as source_file:
        source = json.load(source_file)

    lines = []
    for benchmark, metadata in source.items():
        for point in metadata.get("points", {}):
            lines.append(
                f"{benchmark}_{point} {benchmark}/{point} 0 0 20 20\n"
            )

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text("".join(lines), encoding="utf-8")


def resolve_extra_args(raw: str, repo_root: Path) -> str:
    return " ".join(
        raw.replace("${GEM5_HOME}", str(repo_root))
        .replace("$GEM5_HOME", str(repo_root))
        .split()
    )


def _distributed_options(manifest: dict, target_dir: Path) -> list[str]:
    return [
        "--require-idle-cpus",
        str(manifest["distributed_require_idle_cpus"]),
        "--idle-probe-mode",
        manifest["distributed_idle_probe_mode"],
        "--launch-retries",
        "3",
        "--launch-retry-delay",
        "30",
        "--launch-interval",
        "0.5",
        "--ssh-option",
        "StrictHostKeyChecking=accept-new",
        "--ssh-option",
        f"UserKnownHostsFile={target_dir / 'known_hosts'}",
        "--ssh-option",
        "ConnectTimeout=10",
    ]


def build_run_command(manifest: dict, repo_root: Path) -> list[str]:
    profile = manifest["profile"]
    target_dir = Path(manifest["target_dir"])
    config_path = repo_root / manifest["config_path"]
    checkpoint_list = manifest["checkpoint_list"]
    checkpoint_root = profile["checkpoint_root"]
    servers = manifest["distributed_servers"]
    jobs = str(manifest["distributed_jobs_per_server"])
    benchmarks = manifest["specific_benchmarks"]
    extra_args = resolve_extra_args(manifest["extra_args"], repo_root)

    if profile["execution_mode"] == "h":
        command = [
            "python3",
            str(repo_root / "util/xs_scripts/h_spec06_perf.py"),
            "run",
            "--config",
            str(config_path),
            "--checkpoint-list",
            checkpoint_list,
            "--checkpoint-root",
            checkpoint_root,
            "--tag",
            "spec_all",
            "--benchmarks",
            benchmarks,
            "--servers",
            servers or "local",
            "--jobs-per-server",
            jobs,
            "--gem5-home",
            str(repo_root),
            "--build-type",
            "fast",
            "--ref-so",
            profile["ref_so"],
            "--restorer",
            profile["restorer"],
            "--maxinsts",
            str(profile["maxinsts"]),
            "--dram-ini",
            str(
                repo_root
                / "ext/dramsim3/xiangshan_configs/xiangshan_DDR4_8Gb_x8_3200_2ch.ini"
            ),
            "--extra-args",
            manifest["extra_args"],
        ]
        if servers:
            command.extend(_distributed_options(manifest, target_dir))
        return command

    if servers:
        return [
            "python3",
            str(repo_root / "util/xs_scripts/distributed_sim.py"),
            "--servers",
            servers,
            "--jobs-per-server",
            jobs,
            *_distributed_options(manifest, target_dir),
            "--build-type",
            "fast",
            str(config_path.resolve()),
            checkpoint_list,
            checkpoint_root,
            "spec_all",
            benchmarks,
            extra_args,
        ]

    return [
        "bash",
        str(repo_root / "util/xs_scripts/parallel_sim.sh"),
        str(config_path.resolve()),
        checkpoint_list,
        checkpoint_root,
        "spec_all",
        benchmarks,
        extra_args,
    ]


def _safe_config_path(repo_root: Path, config_path: str) -> Path:
    relative_path = Path(config_path)
    if relative_path.is_absolute() or relative_path.suffix != ".py":
        raise ValueError(
            f"config_path must be a repo-root-relative .py file: {config_path!r}"
        )
    resolved = (repo_root / relative_path).resolve()
    try:
        resolved.relative_to(repo_root.resolve())
    except ValueError as exc:
        raise ValueError(f"config_path escapes the repository: {config_path!r}") from exc
    if not resolved.is_file():
        raise ValueError(f"config_path does not exist: {config_path!r}")
    return resolved


def prepare_run(
    *,
    profile: dict,
    repo_root: Path,
    archive_root: Path,
    config_path: str,
    commit: str,
    run_number: str,
    timestamp: str,
    commit_short: str | None = None,
    specific_benchmarks: str = "",
    extra_args: str = "",
    distributed_servers: str = "",
    distributed_jobs_per_server: int = 32,
    github_ref: str = "",
    run_id: str = "",
) -> Path:
    commit_short = commit_short or commit[:7]
    resolved_config = _safe_config_path(repo_root, config_path)
    config_name = re.sub(r"[^A-Za-z0-9._-]", "_", resolved_config.stem)
    benchmark_dir = archive_root / profile["benchmark_type"]
    target_dir = benchmark_dir / (
        f"{timestamp}_{commit_short}_{config_name}_run{run_number}"
    )
    target_dir.mkdir(parents=True, exist_ok=False)

    if profile.get("checkpoint_list_json"):
        checkpoint_list = target_dir / "checkpoint_selection.lst"
        generate_checkpoint_list(
            Path(profile["checkpoint_list_json"]), checkpoint_list
        )
    else:
        checkpoint_list = Path(profile["checkpoint_list"])

    if profile["execution_mode"] == "h":
        expected = int(profile["expected_checkpoints"])
        actual = sum(1 for _ in checkpoint_list.open(encoding="utf-8"))
        if actual != expected:
            raise ValueError(
                f"H checkpoint list has {actual} entries; expected {expected}"
            )
        copied_list = target_dir / "checkpoint_selection.lst"
        if checkpoint_list != copied_list:
            shutil.copyfile(checkpoint_list, copied_list)
        checkpoint_list = copied_list

    servers = distributed_servers.strip()
    if servers == "default":
        servers = DEFAULT_DISTRIBUTED_SERVERS

    manifest = {
        "profile": profile,
        "config_path": str(resolved_config.relative_to(repo_root.resolve())),
        "checkpoint_list": str(checkpoint_list),
        "target_dir": str(target_dir),
        "benchmark_dir": str(benchmark_dir),
        "specific_benchmarks": specific_benchmarks,
        "extra_args": extra_args,
        "distributed_servers_input": distributed_servers,
        "distributed_servers": servers,
        "distributed_jobs_per_server": distributed_jobs_per_server,
        "distributed_require_idle_cpus": distributed_jobs_per_server,
        "distributed_idle_probe_mode": "physical",
        "archive_keep_count": ARCHIVE_KEEP_COUNT,
        "commit": commit,
        "run_number": str(run_number),
        "run_id": str(run_id),
        "timestamp": timestamp,
    }
    manifest_path = target_dir / "perf-run.json"
    manifest_path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )

    metadata = {
        "timestamp": timestamp,
        "commit": commit,
        "commit_short": commit_short,
        "config_path": manifest["config_path"],
        "config_name": config_name,
        "run_number": str(run_number),
        "branch": github_ref.removeprefix("refs/heads/"),
        "benchmark_type": profile["benchmark_type"],
        "specific_benchmarks": specific_benchmarks,
        "distributed_servers_input": distributed_servers,
        "distributed_servers": servers,
        "distributed_jobs_per_server": distributed_jobs_per_server,
        "distributed_require_idle_cpus": distributed_jobs_per_server,
        "distributed_idle_probe_mode": "physical",
        "distributed_server_domain": "",
        "distributed_idle_cpu_threshold": 30,
        "distributed_ssh_config": "",
        "distributed_ssh_user": "",
        "distributed_launch_retries": 3,
        "distributed_launch_retry_delay": 30,
        "distributed_launch_interval": 0.5,
        "extra_args": extra_args,
        "workflow_run_id": run_id,
        "execution_mode": profile["execution_mode"],
    }
    if profile["execution_mode"] == "h":
        metadata.update(
            {
                "h_ref_so": profile["ref_so"],
                "h_restorer": profile["restorer"],
                "h_maxinsts": profile["maxinsts"],
            }
        )
    (target_dir / "metadata.txt").write_text(
        "".join(f"{key}: {value}\n" for key, value in metadata.items()),
        encoding="utf-8",
    )
    return manifest_path


def _append_lines(path: str | None, lines: list[str]) -> None:
    if not path:
        return
    with Path(path).open("a", encoding="utf-8") as output_file:
        output_file.writelines(f"{line}\n" for line in lines)


def _load_manifest(path: str | Path) -> dict:
    with Path(path).open(encoding="utf-8") as manifest_file:
        return json.load(manifest_file)


def command_prepare(args: argparse.Namespace) -> int:
    repo_root = Path(args.repo_root).resolve()
    profile = load_profile(args.benchmark_type)
    commit = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=repo_root,
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    commit_short = subprocess.run(
        ["git", "rev-parse", "--short", "HEAD"],
        cwd=repo_root,
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    manifest_path = prepare_run(
        profile=profile,
        repo_root=repo_root,
        archive_root=Path(args.archive_root),
        config_path=args.config_path,
        commit=commit,
        commit_short=commit_short,
        run_number=args.run_number,
        timestamp=timestamp,
        specific_benchmarks=args.specific_benchmarks,
        extra_args=args.extra_args,
        distributed_servers=args.distributed_servers,
        distributed_jobs_per_server=args.distributed_jobs_per_server,
        github_ref=args.github_ref,
        run_id=args.run_id,
    )
    manifest = _load_manifest(manifest_path)
    profile = manifest["profile"]
    outputs = {
        "manifest": str(manifest_path),
        "target_dir": manifest["target_dir"],
        "benchmark_dir": manifest["benchmark_dir"],
        "artifact_name": profile["artifact_name"],
        "description": profile["description"],
    }
    _append_lines(
        args.github_output,
        [f"{key}={value}" for key, value in outputs.items()],
    )
    _append_lines(
        args.github_summary,
        ["### Live archive output", f"- Path: `{manifest['target_dir']}`"],
    )
    print(json.dumps(outputs, indent=2))
    return 0


def command_build(args: argparse.Namespace) -> int:
    repo_root = Path(args.repo_root).resolve()
    manifest = _load_manifest(args.manifest)
    benchmark_type = manifest["profile"]["benchmark_type"]
    env = os.environ.copy()
    env["GEM5_HOME"] = str(repo_root)
    env["GCBV_REF_SO"] = (
        "/nfs/home/share/gem5_ci/ref/normal/"
        "riscv64-nemu-notama-tvalref-so"
    )
    if benchmark_type.startswith("gcc12-spec06-smt-"):
        env["GCBV_MULTI_CORE_REF_SO"] = (
            "/nfs/home/share/gem5_ci/ref/multi/"
            "riscv64-nemu-interpreter-so"
        )
        env["GCB_MULTI_CORE_RESTORER"] = ""

    scripts = {
        "": "util/pgo/basic_pgo_new.sh",
        "base": "util/pgo/basic_pgo_new.sh",
        "simple": "util/pgo/basic_pgo_new_vector.sh",
    }
    try:
        script = scripts[args.vector_type]
    except KeyError as exc:
        raise ValueError("vector_type must be empty, base, or simple") from exc
    subprocess.run(["bash", script], cwd=repo_root, env=env, check=True)
    return 0


def _runner_environment(manifest: dict, repo_root: Path) -> dict[str, str]:
    env = os.environ.copy()
    env.update(
        {
            "GCBV_REF_SO": (
                "/nfs/home/share/gem5_ci/ref/normal/"
                "riscv64-nemu-notama-tvalref-so"
            ),
            "GEM5_HOME": str(repo_root),
            "GEM5_BUILD_TYPE": "fast",
            "PERF_ARCHIVE_DIR": manifest["target_dir"],
        }
    )
    if manifest["profile"]["benchmark_type"].startswith(
        "gcc12-spec06-smt-"
    ):
        env["GCBV_MULTI_CORE_REF_SO"] = (
            "/nfs/home/share/gem5_ci/ref/multi/"
            "riscv64-nemu-interpreter-so"
        )
        env["GCB_MULTI_CORE_RESTORER"] = ""
    return env


def _check_run_inputs(manifest: dict) -> None:
    profile = manifest["profile"]
    required_paths = [
        (Path(manifest["checkpoint_list"]), "checkpoint list"),
        (Path(profile["checkpoint_root"]), "checkpoint root"),
    ]
    if profile["execution_mode"] == "h":
        required_paths.extend(
            [
                (Path(profile["ref_so"]), "H reference"),
                (Path(profile["restorer"]), "H restorer"),
            ]
        )
    for path, description in required_paths:
        if not path.exists():
            raise FileNotFoundError(f"missing {description}: {path}")


def command_run(args: argparse.Namespace) -> int:
    repo_root = Path(args.repo_root).resolve()
    manifest = _load_manifest(args.manifest)
    _check_run_inputs(manifest)
    target_dir = Path(manifest["target_dir"])
    resolved_args = resolve_extra_args(manifest["extra_args"], repo_root)
    with (target_dir / "metadata.txt").open("a", encoding="utf-8") as metadata:
        metadata.write(f"resolved_extra_args: {resolved_args}\n")

    command = build_run_command(manifest, repo_root)
    env = _runner_environment(manifest, repo_root)
    print("Running:", " ".join(command))
    if manifest["profile"]["execution_mode"] != "h":
        subprocess.run(command, cwd=target_dir, env=env, check=True)
        return 0

    with (target_dir / "runner.log").open("a", encoding="utf-8") as log_file:
        process = subprocess.Popen(
            command,
            cwd=target_dir,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        assert process.stdout is not None
        for line in process.stdout:
            sys.stdout.write(line)
            log_file.write(line)
        return_code = process.wait()
    if return_code:
        raise subprocess.CalledProcessError(return_code, command)
    return 0


def _resolve_repo_or_absolute(repo_root: Path, value: str) -> Path:
    path = Path(value)
    return path if path.is_absolute() else repo_root / path


def _run_h_score(manifest: dict, repo_root: Path) -> None:
    profile = manifest["profile"]
    target_dir = Path(manifest["target_dir"])
    status_dir = target_dir / "h_score/h_status"
    subprocess.run(
        [
            "python3",
            str(repo_root / "util/xs_scripts/h_spec06_perf.py"),
            "score",
            "--json",
            str(_resolve_repo_or_absolute(repo_root, profile["selection_json"])),
            "--result-dir",
            str(target_dir / "spec_all"),
            "--out-dir",
            str(status_dir),
            "--benchmarks",
            manifest["specific_benchmarks"],
            "--label",
            profile["label"],
        ],
        check=True,
    )


def _score_text(manifest: dict, repo_root: Path) -> str:
    profile = manifest["profile"]
    target_dir = Path(manifest["target_dir"])
    data_proc = repo_root / "gem5_data_proc"
    shutil.copytree(
        "/nfs/home/share/gem5_ci/gem5_data_proc",
        data_proc,
        dirs_exist_ok=True,
        symlinks=True,
    )
    shutil.rmtree(data_proc / "results", ignore_errors=True)
    env = os.environ.copy()
    env["GEM5_HOME"] = str(repo_root)
    env["PYTHONPATH"] = ":".join(
        filter(
            None,
            [
                str(data_proc),
                "/nfs/home/yanyue/.local/lib/python3.12/site-packages",
                env.get("PYTHONPATH", ""),
            ],
        )
    )
    command = [
        "bash",
        "-e",
        str(data_proc / "example-scripts" / profile["score_script"]),
        str(target_dir / "spec_all"),
        str(_resolve_repo_or_absolute(repo_root, profile["cluster_config"])),
    ]
    try:
        result = subprocess.run(
            command,
            cwd=data_proc,
            env=env,
            check=True,
            capture_output=True,
            text=True,
        )
    except subprocess.CalledProcessError as exc:
        diagnostic = [
            f"command: {shlex.join(command)}",
            f"exit code: {exc.returncode}",
            "",
            "stdout:",
            exc.stdout or "",
            "",
            "stderr:",
            exc.stderr or "",
        ]
        (target_dir / "score-error.log").write_text(
            "\n".join(diagnostic),
            encoding="utf-8",
        )
        if exc.stdout:
            (target_dir / "score.txt").write_text(exc.stdout, encoding="utf-8")
            print(exc.stdout, end="", file=sys.stderr)
        if exc.stderr:
            print(exc.stderr, end="", file=sys.stderr)
        raise RuntimeError(
            f"performance score command failed with exit code {exc.returncode}"
        ) from exc
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    return result.stdout


def extract_final_score(score_text: str) -> str:
    for line in score_text.splitlines():
        if "Estimated Int score per GHz:" in line:
            return line.split()[-1]
    raise RuntimeError(
        "performance score output is missing 'Estimated Int score per GHz:'"
    )


def command_score(args: argparse.Namespace) -> int:
    repo_root = Path(args.repo_root).resolve()
    manifest = _load_manifest(args.manifest)
    profile = manifest["profile"]
    target_dir = Path(manifest["target_dir"])
    if profile["execution_mode"] == "h":
        _run_h_score(manifest, repo_root)

    score_text = _score_text(manifest, repo_root)
    score_path = target_dir / "score.txt"
    score_path.write_text(score_text, encoding="utf-8")
    shutil.copyfile(score_path, repo_root / "score.txt")
    print(score_text, end="")
    summary_lines = [
        "### performance test result",
        "```",
        *score_text.splitlines()[-42:],
        "```",
    ]
    final_score = extract_final_score(score_text)
    summary_lines.extend(
        ["### Key indicators", f"- Final Int score per GHz: **{final_score}**"]
    )
    _append_lines(args.github_summary, summary_lines)

    if profile["execution_mode"] == "h":
        subprocess.run(
            [
                "python3",
                str(repo_root / "util/xs_scripts/h_spec06_perf.py"),
                "validate",
                "--summary",
                str(target_dir / "h_score/h_status/summary.json"),
            ],
            check=True,
        )

    aborts = list((target_dir / "spec_all").rglob("abort"))
    if aborts:
        _append_lines(
            args.github_summary,
            [
                "### Test Failures Detected",
                f"Failed test count: {len(aborts)}",
                "First 10 failed tests:",
                *(f"- {path.parent.name}" for path in aborts[:10]),
            ],
        )
        if args.check_result:
            raise RuntimeError(f"{len(aborts)} performance checkpoints aborted")
    return 0


def cleanup_old_archives(
    benchmark_dir: Path,
    target_dir: Path,
    keep_count: int = ARCHIVE_KEEP_COUNT,
) -> list[Path]:
    if keep_count < 1:
        raise ValueError("archive keep count must be positive")

    benchmark_dir = benchmark_dir.resolve()
    target_dir = target_dir.resolve()
    if target_dir.parent != benchmark_dir:
        raise ValueError(
            f"archive target {target_dir} is not directly under {benchmark_dir}"
        )
    if not benchmark_dir.is_dir():
        return []

    run_dirs = sorted(
        (
            path
            for path in benchmark_dir.iterdir()
            if path.is_dir() and not path.is_symlink()
        ),
        key=lambda path: path.stat().st_mtime_ns,
        reverse=True,
    )
    stale_dirs = [path for path in run_dirs[keep_count:] if path != target_dir]
    for stale_dir in stale_dirs:
        shutil.rmtree(stale_dir)
        print(f"Deleted old performance archive: {stale_dir}")
    return stale_dirs


def command_finalize(args: argparse.Namespace) -> int:
    manifest = _load_manifest(args.manifest)
    target_dir = Path(manifest["target_dir"])
    benchmark_dir = Path(manifest["benchmark_dir"])
    cleanup_old_archives(
        benchmark_dir,
        target_dir,
        int(manifest.get("archive_keep_count", ARCHIVE_KEEP_COUNT)),
    )
    result_dir = target_dir / "spec_all"
    size = subprocess.run(
        ["du", "-sh", str(result_dir)],
        capture_output=True,
        text=True,
    )
    archive_size = size.stdout.split()[0] if size.returncode == 0 else "missing"
    _append_lines(
        args.github_summary,
        [
            "### Archive output",
            f"- Path: `{target_dir}`",
            f"- Size: {archive_size}",
        ],
    )
    print(f"Performance data archived at {target_dir} ({archive_size})")
    return 0


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", default=os.getcwd())
    subparsers = parser.add_subparsers(dest="command", required=True)

    prepare = subparsers.add_parser("prepare")
    prepare.add_argument("--benchmark-type", required=True)
    prepare.add_argument("--config-path", required=True)
    prepare.add_argument("--specific-benchmarks", default="")
    prepare.add_argument("--extra-args", default="")
    prepare.add_argument("--distributed-servers", default="")
    prepare.add_argument("--distributed-jobs-per-server", type=int, default=32)
    prepare.add_argument("--archive-root", required=True)
    prepare.add_argument("--run-number", required=True)
    prepare.add_argument("--run-id", default="")
    prepare.add_argument("--github-ref", default="")
    prepare.add_argument("--github-output", default=os.getenv("GITHUB_OUTPUT"))
    prepare.add_argument("--github-summary", default=os.getenv("GITHUB_STEP_SUMMARY"))
    prepare.set_defaults(func=command_prepare)

    build = subparsers.add_parser("build")
    build.add_argument("--manifest", required=True)
    build.add_argument("--vector-type", default="")
    build.set_defaults(func=command_build)

    run = subparsers.add_parser("run")
    run.add_argument("--manifest", required=True)
    run.set_defaults(func=command_run)

    score = subparsers.add_parser("score")
    score.add_argument("--manifest", required=True)
    score.add_argument(
        "--check-result",
        choices=("true", "false"),
        default="true",
    )
    score.add_argument("--github-summary", default=os.getenv("GITHUB_STEP_SUMMARY"))
    score.set_defaults(func=command_score)

    finalize = subparsers.add_parser("finalize")
    finalize.add_argument("--manifest", required=True)
    finalize.add_argument("--github-summary", default=os.getenv("GITHUB_STEP_SUMMARY"))
    finalize.set_defaults(func=command_finalize)
    return parser


def main() -> int:
    args = make_parser().parse_args()
    if hasattr(args, "check_result"):
        args.check_result = args.check_result == "true"
    try:
        return args.func(args)
    except (FileNotFoundError, ValueError, RuntimeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
