from __future__ import annotations

from dataclasses import dataclass, field
from math import ceil
from pathlib import Path
import os
import shlex
import signal
import subprocess
import time
from typing import Callable

import util.xs_scripts.distributed_sim as dist


DEFAULT_DISTRIBUTED_SERVERS = "node020-node034,node036-node039"
DEFAULT_IDLE_CPU_THRESHOLD = 30.0


@dataclass(frozen=True)
class DistributedExecutionConfig:
    servers: str = ""
    jobs_per_server: int = 0
    require_idle_cpus: int = -1
    idle_probe_mode: str = "physical"
    idle_cpu_threshold: float = DEFAULT_IDLE_CPU_THRESHOLD
    server_domain: str = ""
    ssh_config: str = ""
    ssh_user: str = ""
    dispatch_host: str = ""
    ssh_options: tuple[str, ...] = ()
    poll_interval: float = 5.0
    load_probe_interval: float = 15.0
    load_probe_timeout: float = 10.0
    marker_timeout: float = 30.0
    launch_retries: int = 3
    launch_retry_delay: float = 30.0
    launch_interval: float = 0.5

    def enabled(self) -> bool:
        return bool(self.servers.strip())

    def validate(self) -> None:
        if self.jobs_per_server < 0:
            raise ValueError("distributed_jobs_per_server must be >= 0")
        if self.require_idle_cpus < -1:
            raise ValueError("distributed_require_idle_cpus must be >= -1")
        if self.idle_probe_mode not in {"physical", "logical"}:
            raise ValueError("distributed_idle_probe_mode must be physical or logical")
        if not 0 <= self.idle_cpu_threshold <= 100:
            raise ValueError("distributed_idle_cpu_threshold must be between 0 and 100")
        if self.poll_interval <= 0:
            raise ValueError("distributed_poll_interval must be > 0")
        if self.load_probe_interval <= 0:
            raise ValueError("distributed_load_probe_interval must be > 0")
        if self.load_probe_timeout <= 0:
            raise ValueError("distributed_load_probe_timeout must be > 0")
        if self.marker_timeout < 0:
            raise ValueError("distributed_marker_timeout must be >= 0")
        if self.launch_retries < 0:
            raise ValueError("distributed_launch_retries must be >= 0")
        if self.launch_retry_delay < 0:
            raise ValueError("distributed_launch_retry_delay must be >= 0")
        if self.launch_interval < 0:
            raise ValueError("distributed_launch_interval must be >= 0")


@dataclass(frozen=True)
class DistributedWorkloadJob:
    trial_id: str
    workload_name: str
    checkpoint: Path
    work_dir: Path
    command: list[str]
    env: dict[str, str]


@dataclass(frozen=True)
class DistributedWorkloadResult:
    trial_id: str
    workload_name: str
    checkpoint: Path
    status: str
    return_code: int
    server_name: str
    detail: str = ""
    started_at: float | None = None
    finished_at: float | None = None


@dataclass(frozen=True)
class _ScheduledJob:
    job: DistributedWorkloadJob
    ready_at: float = 0.0
    attempt: int = 1


@dataclass
class _ServerState:
    name: str
    pending: list["_PendingJob"] = field(default_factory=list)
    capacity: int = 0
    idle_cpus: int | None = None
    last_probe_at: float = 0.0
    last_probe_detail: str = ""


@dataclass
class _PendingJob:
    scheduled: _ScheduledJob
    server: _ServerState
    proc: subprocess.Popen[bytes]
    started_at: float


def resolve_server_names(config: DistributedExecutionConfig) -> list[str]:
    servers = config.servers.strip()
    if not servers:
        return []
    if servers == "default":
        servers = DEFAULT_DISTRIBUTED_SERVERS
    return dist.apply_server_domain(
        dist.parse_server_list(servers),
        config.server_domain,
    )


def resolve_jobs_per_server(
    config: DistributedExecutionConfig,
    *,
    total_parallelism: int,
    server_count: int,
) -> int:
    if server_count < 1:
        raise ValueError("server_count must be >= 1")
    if total_parallelism < 1:
        raise ValueError("total_parallelism must be >= 1")
    if config.jobs_per_server > 0:
        return config.jobs_per_server
    return max(1, ceil(total_parallelism / server_count))


def resolve_require_idle_cpus(
    config: DistributedExecutionConfig,
    *,
    jobs_per_server: int,
) -> int:
    if config.require_idle_cpus >= 0:
        return config.require_idle_cpus
    return jobs_per_server


def _append_launcher_output(job: DistributedWorkloadJob, stdout: bytes, stderr: bytes) -> None:
    if not stdout and not stderr:
        return
    job.work_dir.mkdir(parents=True, exist_ok=True)
    with (job.work_dir / "log.txt").open("ab") as handle:
        handle.write(b"\n===== solver distributed launcher output =====\n")
        if stdout:
            handle.write(b"--- stdout ---\n")
            handle.write(stdout)
            if not stdout.endswith(b"\n"):
                handle.write(b"\n")
        if stderr:
            handle.write(b"--- stderr ---\n")
            handle.write(stderr)
            if not stderr.endswith(b"\n"):
                handle.write(b"\n")


def _mark_failure(job: DistributedWorkloadJob, message: str) -> None:
    job.work_dir.mkdir(parents=True, exist_ok=True)
    (job.work_dir / "running").unlink(missing_ok=True)
    (job.work_dir / "completed").unlink(missing_ok=True)
    (job.work_dir / "abort").touch()
    with (job.work_dir / "log.txt").open("a", encoding="utf-8") as handle:
        handle.write("\n===== solver distributed launcher failure =====\n")
        handle.write(message)
        if not message.endswith("\n"):
            handle.write("\n")


def _wait_for_marker(work_dir: Path, timeout: float) -> str:
    deadline = time.time() + timeout
    while True:
        if (work_dir / "completed").exists():
            return "completed"
        if (work_dir / "abort").exists():
            return "abort"
        if timeout == 0 or time.time() >= deadline:
            return ""
        time.sleep(min(0.5, max(deadline - time.time(), 0.0)))


def _has_any_marker(work_dir: Path) -> bool:
    return any(
        (work_dir / marker).exists()
        for marker in ("running", "completed", "abort")
    )


def _clear_stale_markers(work_dir: Path) -> None:
    for marker in ("running", "completed", "abort"):
        (work_dir / marker).unlink(missing_ok=True)


def _make_job_script(job: DistributedWorkloadJob, server_name: str) -> str:
    command_text = " ".join(shlex.quote(part) for part in job.command)
    header = [
        f"solver distributed workload: {job.trial_id}/{job.workload_name}",
        f"checkpoint: {job.checkpoint}",
        f"server: {server_name}",
        f"command: {command_text}",
    ]
    header_lines = "\n".join(
        f"printf '%s\\n' {shlex.quote(line)}"
        for line in header
    )
    return f"""set -u
mkdir -p {shlex.quote(str(job.work_dir))}
cd {shlex.quote(str(job.work_dir))}
exec >{shlex.quote("log.txt")} 2>&1
{header_lines}
echo "host: $(hostname)"
echo "start: $(date -Is)"
{dist.shell_exports(job.env)}
rm -f abort completed
touch running
{command_text}
status=$?
rm -f running
if [ "$status" -eq 0 ]; then
    touch completed
else
    touch abort
fi
echo "finish: $(date -Is)"
echo "exit_status: $status"
exit "$status"
"""


class DistributedWorkloadScheduler:
    def __init__(
        self,
        config: DistributedExecutionConfig,
        *,
        total_parallelism: int,
        log: Callable[[str], None],
        process_started: Callable[[subprocess.Popen[bytes]], None] | None = None,
        process_finished: Callable[[subprocess.Popen[bytes]], None] | None = None,
        cancel_requested: Callable[[], bool] | None = None,
    ) -> None:
        config.validate()
        server_names = resolve_server_names(config)
        if not server_names:
            raise ValueError("distributed scheduler requires a non-empty server list")
        self.config = config
        self.total_parallelism = max(1, total_parallelism)
        self.jobs_per_server = resolve_jobs_per_server(
            config,
            total_parallelism=self.total_parallelism,
            server_count=len(server_names),
        )
        self.require_idle_cpus = resolve_require_idle_cpus(
            config,
            jobs_per_server=self.jobs_per_server,
        )
        self.servers = [_ServerState(name=name) for name in server_names]
        self.log = log
        self.process_started = process_started or (lambda process: None)
        self.process_finished = process_finished or (lambda process: None)
        self.cancel_requested = cancel_requested or (lambda: False)
        self._last_launch_at = 0.0
        self._next_server_index = 0

    def describe(self) -> dict[str, object]:
        return {
            "mode": "distributed",
            "servers": [server.name for server in self.servers],
            "jobs_per_server": self.jobs_per_server,
            "require_idle_cpus": self.require_idle_cpus,
            "idle_probe_mode": self.config.idle_probe_mode,
            "idle_cpu_threshold": self.config.idle_cpu_threshold,
            "dispatch_host": self.config.dispatch_host,
        }

    def _refresh_server_capacity(self, server: _ServerState, *, force: bool = False) -> None:
        now = time.time()
        if (
            not force
            and server.last_probe_at
            and now - server.last_probe_at < self.config.load_probe_interval
        ):
            return
        server.last_probe_at = now
        if self.require_idle_cpus <= 0:
            server.capacity = self.jobs_per_server
            server.last_probe_detail = "idle probe disabled"
            return
        idle_cpus, detail = dist.probe_idle_cpus(
            server_name=server.name,
            idle_probe_mode=self.config.idle_probe_mode,
            idle_cpu_threshold=self.config.idle_cpu_threshold,
            ssh_config=self.config.ssh_config,
            ssh_options=list(self.config.ssh_options),
            ssh_user=self.config.ssh_user,
            dispatch_host=self.config.dispatch_host,
            timeout=self.config.load_probe_timeout,
        )
        server.idle_cpus = idle_cpus
        server.last_probe_detail = detail
        if idle_cpus is None:
            server.capacity = 0
            self.log(f"[solver-distributed] skip {server.name}: {detail}")
            return
        if idle_cpus < self.require_idle_cpus:
            server.capacity = 0
            self.log(
                f"[solver-distributed] busy {server.name}: {detail}, "
                f"require>={self.require_idle_cpus}"
            )
            return
        server.capacity = min(self.jobs_per_server, idle_cpus)
        self.log(
            f"[solver-distributed] use {server.name}: capacity={server.capacity}, "
            f"{detail}"
        )

    def _running_count(self) -> int:
        return sum(len(server.pending) for server in self.servers)

    def _select_server(self) -> _ServerState | None:
        if self._running_count() >= self.total_parallelism:
            return None
        server_count = len(self.servers)
        for offset in range(server_count):
            index = (self._next_server_index + offset) % server_count
            server = self.servers[index]
            self._refresh_server_capacity(server)
            if len(server.pending) < server.capacity:
                self._next_server_index = (index + 1) % server_count
                return server
        return None

    def _launch(self, scheduled: _ScheduledJob, server: _ServerState) -> None:
        job = scheduled.job
        job.work_dir.mkdir(parents=True, exist_ok=True)
        _clear_stale_markers(job.work_dir)
        script = _make_job_script(job, server.name)
        if server.name == "local":
            proc = subprocess.Popen(
                ["bash", "-lc", script],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                start_new_session=True,
            )
        else:
            ssh_cmd = dist.build_ssh_command(
                target=dist.make_ssh_target(server.name, self.config.ssh_user),
                remote_command=["bash", "-lc", script],
                ssh_config=self.config.ssh_config,
                ssh_options=list(self.config.ssh_options),
                fixed_options=[
                    "BatchMode=yes",
                    "ConnectionAttempts=3",
                    "TCPKeepAlive=yes",
                    "ServerAliveInterval=300",
                    "ServerAliveCountMax=576",
                ],
            )
            if self.config.dispatch_host:
                ssh_cmd = dist.wrap_with_dispatch_host(
                    ssh_cmd=ssh_cmd,
                    dispatch_host=self.config.dispatch_host,
                    ssh_config=self.config.ssh_config,
                    ssh_options=list(self.config.ssh_options),
                )
            proc = subprocess.Popen(
                ssh_cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                start_new_session=True,
            )
        self.process_started(proc)
        server.pending.append(
            _PendingJob(
                scheduled=scheduled,
                server=server,
                proc=proc,
                started_at=time.monotonic(),
            )
        )
        self.log(
            f"[solver-distributed] start {job.trial_id}/{job.workload_name} "
            f"on {server.name} attempt={scheduled.attempt}"
        )

    def _poll(self) -> tuple[list[DistributedWorkloadResult], list[_ScheduledJob]]:
        results: list[DistributedWorkloadResult] = []
        retries: list[_ScheduledJob] = []
        for server in self.servers:
            still_pending = []
            for pending in server.pending:
                return_code = pending.proc.poll()
                if return_code is None:
                    still_pending.append(pending)
                    continue
                self.process_finished(pending.proc)
                finished_at = time.monotonic()
                stdout, stderr = pending.proc.communicate()
                scheduled = pending.scheduled
                job = scheduled.job
                _append_launcher_output(job, stdout, stderr)
                marker_timeout = (
                    self.config.marker_timeout
                    if return_code == 0 else min(self.config.marker_timeout, 2.0)
                )
                marker = _wait_for_marker(job.work_dir, marker_timeout)
                if (
                    server.name != "local"
                    and return_code == 255
                    and marker == ""
                    and not _has_any_marker(job.work_dir)
                    and scheduled.attempt <= self.config.launch_retries
                ):
                    delay = self.config.launch_retry_delay * scheduled.attempt
                    retries.append(
                        _ScheduledJob(
                            job=job,
                            ready_at=time.time() + delay,
                            attempt=scheduled.attempt + 1,
                        )
                    )
                    self.log(
                        f"[solver-distributed] retry {job.trial_id}/{job.workload_name} "
                        f"after launcher exit=255 delay={delay:.1f}s"
                    )
                    continue
                if return_code == 0 and marker == "completed":
                    status = "completed"
                    code = 0
                else:
                    status = "failed"
                    code = return_code if return_code not in (None, 0) else 1
                    if marker == "" and not _has_any_marker(job.work_dir):
                        _mark_failure(
                            job,
                            (
                                "Distributed launcher exited before remote "
                                f"status markers became visible (exit={return_code})."
                            ),
                        )
                results.append(
                    DistributedWorkloadResult(
                        trial_id=job.trial_id,
                        workload_name=job.workload_name,
                        checkpoint=job.checkpoint,
                        status=status,
                        return_code=code,
                        server_name=server.name,
                        detail=f"marker={marker or '<none>'}",
                        started_at=pending.started_at,
                        finished_at=finished_at,
                    )
                )
                self.log(
                    f"[solver-distributed] {status} {job.trial_id}/{job.workload_name} "
                    f"on {server.name} return_code={code}"
                )
            server.pending = still_pending
        return results, retries

    def _stop_pending(
        self,
        message: str,
        *,
        status: str = "cancelled",
        return_code: int = 130,
    ) -> list[DistributedWorkloadResult]:
        results = []
        for server in self.servers:
            for pending in server.pending:
                proc = pending.proc
                if proc.poll() is None:
                    try:
                        os.killpg(proc.pid, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                self.process_finished(proc)
                job = pending.scheduled.job
                _mark_failure(job, message)
                results.append(
                    DistributedWorkloadResult(
                        trial_id=job.trial_id,
                        workload_name=job.workload_name,
                        checkpoint=job.checkpoint,
                        status=status,
                        return_code=return_code,
                        server_name=server.name,
                        detail=message,
                        started_at=pending.started_at,
                        finished_at=time.monotonic(),
                    )
                )
            server.pending = []
        return results

    def run(
        self,
        jobs: list[DistributedWorkloadJob],
        *,
        deadline: float | None = None,
    ) -> list[DistributedWorkloadResult]:
        scheduled_jobs = [_ScheduledJob(job=job) for job in jobs]
        results: list[DistributedWorkloadResult] = []
        while scheduled_jobs or self._running_count():
            polled, retries = self._poll()
            results.extend(polled)
            scheduled_jobs.extend(retries)
            if self.cancel_requested():
                results.extend(self._stop_pending("solver distributed run cancelled"))
                for scheduled in scheduled_jobs:
                    job = scheduled.job
                    _mark_failure(job, "solver distributed run cancelled before launch")
                    results.append(
                        DistributedWorkloadResult(
                            trial_id=job.trial_id,
                            workload_name=job.workload_name,
                            checkpoint=job.checkpoint,
                            status="cancelled",
                            return_code=130,
                            server_name="<not-launched>",
                            detail="cancelled before launch",
                            started_at=None,
                            finished_at=time.monotonic(),
                        )
                    )
                break
            if deadline is not None and time.monotonic() >= deadline:
                results.extend(
                    self._stop_pending(
                        "solver distributed run timed out",
                        status="timeout",
                        return_code=124,
                    )
                )
                for scheduled in scheduled_jobs:
                    job = scheduled.job
                    _mark_failure(job, "solver distributed run timed out before launch")
                    results.append(
                        DistributedWorkloadResult(
                            trial_id=job.trial_id,
                            workload_name=job.workload_name,
                            checkpoint=job.checkpoint,
                            status="timeout",
                            return_code=124,
                            server_name="<not-launched>",
                            detail="timeout before launch",
                            started_at=None,
                            finished_at=time.monotonic(),
                        )
                    )
                break

            launched = False
            while scheduled_jobs:
                ready_index = next(
                    (
                        index
                        for index, scheduled in enumerate(scheduled_jobs)
                        if scheduled.ready_at <= time.time()
                    ),
                    -1,
                )
                if ready_index < 0:
                    break
                if (
                    self._last_launch_at
                    and time.time() - self._last_launch_at < self.config.launch_interval
                ):
                    break
                server = self._select_server()
                if server is None:
                    break
                scheduled = scheduled_jobs.pop(ready_index)
                self._launch(scheduled, server)
                self._last_launch_at = time.time()
                launched = True

            if scheduled_jobs or self._running_count():
                if not launched:
                    sleep_for = self.config.poll_interval
                    if scheduled_jobs:
                        next_ready = min(item.ready_at for item in scheduled_jobs)
                        sleep_for = min(sleep_for, max(next_ready - time.time(), 0.0))
                    if self._last_launch_at:
                        launch_delay = self.config.launch_interval - (
                            time.time() - self._last_launch_at
                        )
                        if launch_delay > 0:
                            sleep_for = min(sleep_for, launch_delay)
                    time.sleep(max(sleep_for, 0.05))
        return results
