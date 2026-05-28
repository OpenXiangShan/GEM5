import shlex
from collections.abc import Callable, Iterable
from pathlib import Path

from parallux import goal, workloads


REPO_ROOT = Path(__file__).resolve().parents[2]


def q(value: str) -> str:
    return shlex.quote(value)


def repo_path(relative_path: str) -> str:
    return str(REPO_ROOT / relative_path)


def gem5_binary(build_dir: str = "RISCV", build_type: str = "opt") -> str:
    return repo_path(f"build/{build_dir}/gem5.{build_type}")


def config_path(config_name: str) -> str:
    return repo_path(f"configs/example/{config_name}")


def setup_runners(
    workspace: str,
    max_jobs: int,
    runners: str | None = None,
    *,
    parallel: int | None = None,
):
    runner_specs = []
    runner_names = [item.strip() for item in (runners or "local").split(",")]
    runner_names = [item for item in runner_names if item]
    for name in runner_names:
        if name == "local":
            runner_specs.append(
                goal.local(name="local", workspace=workspace, max_jobs=max_jobs)
            )
            continue

        user = None
        host = name
        if "@" in name:
            user, host = name.split("@", 1)
        runner_specs.append(
            goal.ssh(
                name=name,
                host=host,
                user=user,
                workspace=workspace,
                max_jobs=max_jobs,
            )
        )

    if not runner_specs:
        raise RuntimeError("runners must not be empty")

    goal.setRunner(runner_specs)
    goal.setParallel(parallel or max_jobs * len(runner_specs))
    return runner_specs


def setup_local_runner(log_root: str, max_process: int):
    return setup_runners(log_root, max_process, "local")[0]


def set_env_if_present(keys: Iterable[str]) -> None:
    import os

    for key in keys:
        value = os.environ.get(key)
        if value is not None:
            goal.setEnv(key, value)


def schedule_workloads(
    patterns: Iterable[str],
    *,
    command: Callable[[str], str],
    levels: int,
    work_prefix: str,
    strip_suffix: bool = True,
) -> None:
    scheduled = 0
    for workload in workloads(
        list(patterns),
        levels=levels,
        work_prefix=work_prefix,
        strip_suffix=strip_suffix,
        sort=False,
    ):
        goal.schd(
            command(workload.input_path),
            name=f"{work_prefix}_{workload.name}",
            work_relpath=workload.work_relpath,
        )
        scheduled += 1

    if scheduled == 0:
        raise RuntimeError(f"no workloads matched for {work_prefix}")

    print(f"scheduled {scheduled} workloads for {work_prefix}")
    goal.issue().sync()
