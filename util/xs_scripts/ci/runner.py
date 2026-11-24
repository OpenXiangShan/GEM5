"""Execute prepared commands on the current machine or the shared SSH pool."""
from __future__ import annotations

from collections import Counter, deque
from contextlib import contextmanager
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from math import ceil
import os
from pathlib import Path
import shlex
import signal
import socket
import subprocess
import time
import threading

from . import hosts


DEFAULT_DISTRIBUTED_SERVERS = (
    "node003-node008,node020-node029,node034,node036-node042"
)
CHECKPOINT_ENV = (
    "GCBV_REF_SO",
    "GCBV_MULTI_CORE_REF_SO",
    "GCBH_REF_SO",
    "GCB_MULTI_CORE_RESTORER",
    "GCBV_RESTORER",
    "GCBH_RESTORER",
)


@dataclass(frozen=True)
class RunnerConfig:
    servers: str = ""
    jobs_per_server: int = 0
    require_idle_cpus: int = -1
    idle_probe_mode: str = "physical"
    idle_cpu_threshold: float = 30.0
    server_domain: str = ""
    ssh_config: str = ""
    ssh_user: str = ""
    ssh_options: tuple[str, ...] = ()
    launch_interval: float = 0.5
    probe_interval: float = 15
    retries: int = 3
    retry_delay: float = 30

    def enabled(self):
        return bool(self.servers.strip())

    def names(self):
        spec = self.servers.strip()
        if spec == "default":
            spec = DEFAULT_DISTRIBUTED_SERVERS
        return hosts.apply_server_domain(
            hosts.parse_server_list(spec), self.server_domain
        )

    def validate(self):
        if self.probe_interval < 0:
            raise ValueError("probe interval must be nonnegative")
        if self.jobs_per_server < 0 or self.require_idle_cpus < -1:
            raise ValueError("invalid distributed capacity")
        if (
            self.idle_probe_mode not in ("physical", "logical")
            or not 0 <= self.idle_cpu_threshold <= 100
        ):
            raise ValueError("invalid idle CPU probe settings")


@dataclass(frozen=True)
class Job:
    trial_id: str
    workload_name: str
    checkpoint: Path
    work_dir: Path
    command: list[str]
    env: dict[str, str]


@dataclass(frozen=True)
class Result:
    job: Job
    status: str
    return_code: int
    server_name: str
    started_at: float | None
    finished_at: float
    detail: str = ""


def simulation_env():
    names = (
        "GEM5_HOME",
        "GEM5_BUILD_TYPE",
        *CHECKPOINT_ENV,
        "AM_HOME",
        "NEMU_HOME",
        "LD_LIBRARY_PATH",
        "PYTHONPATH",
        "M5_OVERRIDE_PY_SOURCE",
    )
    return {k: os.environ[k] for k in names if k in os.environ}


def start(job, node, config, attempt):
    directory = job.work_dir
    directory.mkdir(parents=True, exist_ok=True)
    if attempt == 0:
        for marker in ("running", "completed", "abort"):
            (directory / marker).unlink(missing_ok=True)
    command = job.command
    local = node == "local"
    unset = [key for key in CHECKPOINT_ENV if key not in job.env]
    host = socket.gethostname() if local else node
    with (directory / "log.txt").open("a") as log:
        print(
            f"host: {host}\ncommand: {shlex.join(command)}",
            file=log,
            flush=True,
        )
        # The trace debug helpers also read these fields from older logs.
        if "--enable-trace-mode" in command:
            arguments = dict(
                arg.split("=", 1) for arg in command if "=" in arg
            )
            print(f"Trace file: {arguments.get('--trace-file', '')}", file=log)
            print(
                f"Max instructions: {arguments.get('--maxinsts', '')}",
                file=log,
                flush=True,
            )
        if local:
            (directory / "running").touch()
            env = {**os.environ, **job.env}
            for key in unset:
                env.pop(key, None)
        else:
            # Only the launcher commits terminal markers. The worker publishes
            # running before exec so a lost connection cannot replay a started job.
            worker = shlex.join(
                ["env"]
                + [arg for key in unset for arg in ("-u", key)]
                + [f"{k}={v}" for k, v in job.env.items()]
                + ["python3", "-c", hosts.SSH_JOB_GUARD, *command]
            )
            script = f"cd {shlex.quote(str(directory))} && (set -C; : >running) && exec {worker} >>log.txt 2>&1"
            command = hosts.build_ssh_command(
                hosts.make_ssh_target(node, config.ssh_user),
                ["bash", "-c", script],
                config.ssh_config,
                list(config.ssh_options),
                [
                    "BatchMode=yes",
                    "RequestTTY=no",
                    "StdinNull=no",
                    "ConnectionAttempts=3",
                    "TCPKeepAlive=yes",
                    "ServerAliveInterval=300",
                    "ServerAliveCountMax=576",
                ],
            )
        return subprocess.Popen(
            command,
            cwd=directory,
            env=env if local else None,
            stdin=subprocess.DEVNULL if local else subprocess.PIPE,
            stdout=log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )


def stop(proc):
    if proc.stdin:
        proc.stdin.close()
    if proc.poll() is None:
        try:
            os.killpg(proc.pid, signal.SIGTERM)
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    proc.wait()


@contextmanager
def cancellation_signals():
    event = threading.Event()
    previous = {
        sig: signal.signal(sig, lambda *_: event.set())
        for sig in (signal.SIGTERM, signal.SIGINT)
    }
    try:
        yield event
    finally:
        for sig, handler in previous.items():
            signal.signal(sig, handler)


def run_jobs(
    jobs,
    config,
    parallelism,
    *,
    fallback_to_local=0,
    deadline=None,
    cancelled=lambda: False,
    log=print,
):
    config.validate()
    if parallelism < 1:
        raise ValueError("parallelism must be positive")
    nodes = config.names()
    slots = config.jobs_per_server or max(1, ceil(parallelism / len(nodes)))
    minimum = (
        slots if config.require_idle_cpus < 0 else config.require_idle_cpus
    )
    limit = slots if config.jobs_per_server else min(parallelism, slots * 4)
    queue = deque((job, 0, 0.0) for job in jobs)
    active = {}
    results = []
    failed = assigned = 0
    capacities = {"local": slots} if nodes == ["local"] else {}
    retry_after = {}
    next_probe = next_launch = 0
    total = len(queue)

    def probe(node):
        if now < retry_after.get(node, 0):
            return 0
        if node == "local" or minimum == 0:
            return slots
        idle, detail = hosts.probe_idle_cpus(
            node,
            config.idle_probe_mode,
            config.idle_cpu_threshold,
            config.ssh_config,
            list(config.ssh_options),
            config.ssh_user,
            5,
        )
        if idle is None:
            log(f"{node}: {detail}")
            return 0
        capacity = occupied[node] + idle
        return min(limit, capacity) if capacity >= minimum else 0

    def finish(job, node, started, code, status, detail=""):
        nonlocal failed
        job.work_dir.mkdir(parents=True, exist_ok=True)
        (job.work_dir / "running").unlink(missing_ok=True)
        passed = status == "completed"
        (job.work_dir / ("abort" if passed else "completed")).unlink(
            missing_ok=True
        )
        (job.work_dir / ("completed" if passed else "abort")).touch()
        failed += int(not passed)
        results.append(
            Result(job, status, code, node, started, time.monotonic(), detail)
        )
        if detail:
            with (job.work_dir / "log.txt").open("a") as output:
                print(detail, file=output)
        log(
            f"[{assigned},{len(results)},{failed}/{total}] {job.workload_name} {status} on {node} (exit={code})"
        )

    try:
        while queue or active:
            now = time.monotonic()
            stopped = cancelled()
            if stopped or (deadline is not None and now >= deadline):
                status, code = (
                    ("cancelled", 130) if stopped else ("timeout", 124)
                )
                for proc, (job, node, attempt, started) in list(
                    active.items()
                ):
                    stop(proc)
                    finish(job, node, started, code, status)
                active.clear()
                for job, _, _ in queue:
                    finish(job, "<not-launched>", None, code, status)
                break
            for proc, (job, node, attempt, started) in list(active.items()):
                code = proc.poll()
                if code is None:
                    continue
                del active[proc]
                if proc.stdin:
                    proc.stdin.close()
                if (
                    node != "local"
                    and code == 255
                    and not (job.work_dir / "running").exists()
                ):
                    capacities[node] = 0
                    retry_after[node] = now + config.retry_delay
                    if not config.probe_interval:
                        next_probe = retry_after[node]
                    if attempt < config.retries:
                        queue.append(
                            (
                                job,
                                attempt + 1,
                                now + config.retry_delay * (attempt + 1),
                            )
                        )
                        log(
                            f"Retry {job.workload_name} after SSH launch failure on {node}"
                        )
                        continue
                finish(
                    job,
                    node,
                    started,
                    code,
                    "completed" if code == 0 else "failed",
                )
            occupied = Counter(item[1] for item in active.values())
            free_slot = any(cap > occupied[n] for n, cap in capacities.items())
            if (
                queue
                and len(active) < parallelism
                and not free_slot
                and now >= next_probe
                and nodes != ["local"]
                and (config.probe_interval or not any(capacities.values()))
            ):
                with ThreadPoolExecutor(max_workers=len(nodes)) as pool:
                    capacities = dict(zip(nodes, pool.map(probe, nodes)))
                if not any(capacities.values()) and fallback_to_local:
                    nodes = ["local"]
                    slots = parallelism = fallback_to_local
                    capacities = {"local": slots}
                    log("No eligible remote host; running locally")
                next_probe = time.monotonic() + (
                    config.probe_interval or config.retry_delay
                )
            for _ in range(len(queue)):
                node = max(nodes, key=lambda n: capacities[n] - occupied[n])
                if (
                    len(active) >= parallelism
                    or occupied[node] >= capacities[node]
                    or (node != "local" and now < next_launch)
                ):
                    break
                job, attempt, ready = queue.popleft()
                if ready > now:
                    queue.append((job, attempt, ready))
                    continue
                assigned += int(attempt == 0)
                try:
                    proc = start(job, node, config, attempt)
                except OSError as error:
                    finish(job, node, now, 1, "failed", str(error))
                    continue
                active[proc] = job, node, attempt, now
                occupied[node] += 1
                next_launch = now + config.launch_interval
                log(
                    f"[{assigned},{len(results)},{failed}/{total}] Started {job.workload_name} on {node}"
                )
            if queue or active:
                time.sleep(0.1)
    finally:
        for proc, (job, node, _, started) in active.items():
            stop(proc)
            finish(job, node, started, 130, "cancelled")
    return results
