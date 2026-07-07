#!/usr/bin/env python3
"""Run GEM5 checkpoint workloads across local or SSH-reachable hosts.

This script intentionally mirrors the output layout of parallel_sim.sh:

    <archive>/<tag>/<workload>/log.txt
    <archive>/<tag>/<workload>/m5out/stats.txt
    <archive>/<tag>/<workload>/{running,completed,abort}

Version 1 keeps scheduling simple and explicit: callers pass a server list and a
fixed number of jobs per server.  The remote hosts are expected to see the same
NFS paths as the launcher host.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
import time


CHECKPOINT_SUFFIXES = ("gz", "zstd", "bin")
DEFAULT_POLL_INTERVAL_SEC = 5.0
DEFAULT_MARKER_TIMEOUT_SEC = 30.0
DEFAULT_LAUNCH_RETRIES = 2
DEFAULT_LAUNCH_RETRY_DELAY_SEC = 20.0
DEFAULT_LAUNCH_INTERVAL_SEC = 0.2
ENV_KEY_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


@dataclass(frozen=True)
class Workload:
    name: str
    checkpoint_key: str
    line: str


@dataclass
class ServerState:
    name: str
    pending: list["PendingJob"]
    idle_cpus: int | None = None
    idle_probe_error: str = ""


@dataclass
class PendingJob:
    workload: Workload
    server: ServerState
    work_dir: Path
    proc: subprocess.Popen[bytes]
    started_at: float
    attempt: int


@dataclass(frozen=True)
class ScheduledWorkload:
    workload: Workload
    ready_at: float = 0.0
    attempt: int = 1


def parse_launcher_args(argv: list[str]) -> tuple[argparse.Namespace, list[str]]:
    parser = argparse.ArgumentParser(
        description=(
            "Distributed GEM5 checkpoint runner. Positional arguments are "
            "compatible with util/xs_scripts/parallel_sim.sh."
        )
    )
    parser.add_argument(
        "--servers",
        default="local",
        help=(
            "Comma-separated server list, range, or 'local'. "
            "Examples: local, node020,node021, node020-node039."
        ),
    )
    parser.add_argument(
        "--server-domain",
        default="",
        help=(
            "Append this DNS domain to server names that do not already contain "
            "a dot. Example: --servers node020-node039 --server-domain bosccluster.com."
        ),
    )
    parser.add_argument(
        "--jobs-per-server",
        type=int,
        default=1,
        help="Maximum concurrent GEM5 jobs per server.",
    )
    parser.add_argument(
        "--require-idle-cpus",
        type=int,
        default=0,
        help=(
            "Skip remote servers with fewer than this many idle-ish CPU units. "
            "The default probe mode counts physical cores, so this avoids "
            "depending on SMT sibling threads. 0 disables idle probing."
        ),
    )
    parser.add_argument(
        "--idle-probe-mode",
        choices=("physical", "logical"),
        default="physical",
        help=(
            "Idle CPU probe mode. 'physical' counts a core as idle only when "
            "all of its SMT siblings are idle-ish; 'logical' counts individual "
            "logical CPUs."
        ),
    )
    parser.add_argument(
        "--idle-cpu-threshold",
        type=float,
        default=30.0,
        help=(
            "A logical CPU is treated as idle-ish when sampled utilization is "
            "below this percentage."
        ),
    )
    parser.add_argument(
        "--max-tasks",
        type=int,
        default=0,
        help="Run at most this many selected workloads. 0 means no limit.",
    )
    parser.add_argument(
        "--gem5",
        default="",
        help="Absolute GEM5 binary path. Defaults to $GEM5_HOME/build/RISCV/gem5.$GEM5_BUILD_TYPE.",
    )
    parser.add_argument(
        "--gem5-home",
        default="",
        help="GEM5 repository root. Defaults to $GEM5_HOME or this script's repo root.",
    )
    parser.add_argument(
        "--build-type",
        default=os.environ.get("GEM5_BUILD_TYPE", "opt"),
        help="GEM5 build type used when --gem5 is not provided.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Resolve workloads and print planned commands without launching jobs.",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Rerun workloads even if a completed marker already exists.",
    )
    parser.add_argument(
        "--poll-interval",
        type=float,
        default=DEFAULT_POLL_INTERVAL_SEC,
        help="Seconds between scheduler polls.",
    )
    parser.add_argument(
        "--marker-timeout",
        type=float,
        default=DEFAULT_MARKER_TIMEOUT_SEC,
        help=(
            "Seconds to wait for completed/abort markers to become visible after "
            "a remote process exits. This covers normal NFS metadata delay."
        ),
    )
    parser.add_argument(
        "--launch-retries",
        type=int,
        default=DEFAULT_LAUNCH_RETRIES,
        help=(
            "Number of extra attempts for launcher-side SSH failures before "
            "the remote job creates any status marker."
        ),
    )
    parser.add_argument(
        "--launch-retry-delay",
        type=float,
        default=DEFAULT_LAUNCH_RETRY_DELAY_SEC,
        help="Base seconds to wait before retrying a launcher-side SSH failure.",
    )
    parser.add_argument(
        "--launch-interval",
        type=float,
        default=DEFAULT_LAUNCH_INTERVAL_SEC,
        help=(
            "Seconds to wait between starting jobs. This avoids large SSH "
            "connection bursts through a dispatch host."
        ),
    )
    parser.add_argument(
        "--ssh-option",
        action="append",
        default=[],
        help="Extra ssh option, for example --ssh-option ConnectTimeout=10.",
    )
    parser.add_argument(
        "--ssh-config",
        default="",
        help="Optional ssh config file passed as -F <path>.",
    )
    parser.add_argument(
        "--ssh-user",
        default="",
        help=(
            "Optional SSH user for worker servers. If omitted, ssh uses its "
            "normal user/config resolution."
        ),
    )
    parser.add_argument(
        "--dispatch-host",
        default="",
        help=(
            "Optional SSH host used as a dispatch point. When set, worker "
            "commands are launched by first ssh'ing to this host, then ssh'ing "
            "from there to the worker server. This is useful when compute nodes "
            "are only reachable from a login host."
        ),
    )
    parser.add_argument(
        "--env",
        action="append",
        default=[],
        metavar="KEY=VALUE",
        help="Extra environment variable exported for each GEM5 job.",
    )
    parser.add_argument(
        "--extra-gem5-args",
        default="",
        help="Extra GEM5 arguments. Overrides the legacy sixth positional argument when set.",
    )

    args, rest = parser.parse_known_args(argv)
    if len(rest) < 4:
        parser.error(
            "expected: <config.py|script.sh> <workload_list.lst> "
            "<checkpoint_top_dir> <task_tag> [benchmark_filters] [extra_gem5_args]"
        )
    if args.jobs_per_server < 1:
        parser.error("--jobs-per-server must be >= 1")
    if args.require_idle_cpus < 0:
        parser.error("--require-idle-cpus must be >= 0")
    if not 0 <= args.idle_cpu_threshold <= 100:
        parser.error("--idle-cpu-threshold must be between 0 and 100")
    if args.max_tasks < 0:
        parser.error("--max-tasks must be >= 0")
    if args.poll_interval <= 0:
        parser.error("--poll-interval must be > 0")
    if args.marker_timeout < 0:
        parser.error("--marker-timeout must be >= 0")
    if args.launch_retries < 0:
        parser.error("--launch-retries must be >= 0")
    if args.launch_retry_delay < 0:
        parser.error("--launch-retry-delay must be >= 0")
    if args.launch_interval < 0:
        parser.error("--launch-interval must be >= 0")
    return args, rest


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[2]


def resolve_config_or_script(first_param: str, gem5_home: Path) -> Path:
    path = Path(first_param)
    if not path.is_absolute() and (gem5_home / path).exists():
        path = gem5_home / path
    return path.resolve()


def validate_ssh_target_name(name: str, option_name: str) -> None:
    if name != "local" and name.startswith("-"):
        raise ValueError(f"{option_name} contains an invalid SSH target: {name!r}")


def parse_server_list(spec: str) -> list[str]:
    spec = spec.strip()
    if spec == "" or spec == "local":
        return ["local"]

    servers: list[str] = []
    for raw_token in spec.split(","):
        token = raw_token.strip()
        if not token:
            continue
        range_match = re.fullmatch(
            r"([A-Za-z_-]*)(\d+)-(?:(\1)?(\d+)|([A-Za-z_-]+)(\d+))",
            token,
        )
        if range_match:
            prefix = range_match.group(1)
            start_text = range_match.group(2)
            if range_match.group(4) is not None:
                end_prefix = prefix
                end_text = range_match.group(4)
            else:
                end_prefix = range_match.group(5)
                end_text = range_match.group(6)
            if end_prefix != prefix:
                raise ValueError(f"server range prefixes differ: {token}")
            start = int(start_text)
            end = int(end_text)
            step = 1 if end >= start else -1
            width = max(len(start_text), len(end_text))
            for number in range(start, end + step, step):
                servers.append(f"{prefix}{number:0{width}d}")
            continue

        short_range_match = re.fullmatch(r"([A-Za-z_-]*)(\d+)-(\d+)", token)
        if short_range_match:
            prefix = short_range_match.group(1)
            start_text = short_range_match.group(2)
            end_text = short_range_match.group(3)
            start = int(start_text)
            end = int(end_text)
            step = 1 if end >= start else -1
            width = max(len(start_text), len(end_text))
            for number in range(start, end + step, step):
                servers.append(f"{prefix}{number:0{width}d}")
            continue

        servers.append(token)

    if not servers:
        raise ValueError(f"empty server list: {spec!r}")
    for server in servers:
        validate_ssh_target_name(server, "--servers")
    return servers


def apply_server_domain(servers: list[str], domain: str) -> list[str]:
    domain = domain.strip().lstrip(".")
    if not domain:
        return servers
    return [
        server if server == "local" or "." in server else f"{server}.{domain}"
        for server in servers
    ]


def read_workload_list(path: Path) -> list[Workload]:
    workloads: list[Workload] = []
    with path.open("r", encoding="utf-8") as handle:
        for raw_line in handle:
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            fields = line.split()
            if len(fields) < 2:
                raise ValueError(f"bad workload line in {path}: {raw_line.rstrip()}")
            workloads.append(Workload(name=fields[0], checkpoint_key=fields[1], line=line))
    return workloads


def apply_benchmark_filter(
    workloads: list[Workload], benchmark_filters: str
) -> list[Workload]:
    tokens = [
        token.strip().lower()
        for token in benchmark_filters.split(",")
        if token.strip()
    ]
    if not tokens:
        return workloads

    selected = [
        workload
        for workload in workloads
        if any(token in workload.line.lower() for token in tokens)
    ]
    if not selected:
        raise ValueError(
            f"benchmark filter {benchmark_filters!r} matched no workloads"
        )
    return selected


def find_checkpoint(cpt_dir: Path, checkpoint_key: str) -> Path:
    direct_dir = cpt_dir / checkpoint_key
    if direct_dir.is_dir():
        for suffix in CHECKPOINT_SUFFIXES:
            match = next(direct_dir.rglob(f"*.{suffix}"), None)
            if match is not None:
                return match.resolve()

    key = checkpoint_key.rstrip("/")
    for suffix in CHECKPOINT_SUFFIXES:
        proc = subprocess.run(
            [
                "find",
                "-L",
                str(cpt_dir),
                "-wholename",
                f"*{key}*.{suffix}",
                "-print",
                "-quit",
            ],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if proc.returncode not in (0, 1):
            raise RuntimeError(
                f"find failed while resolving {checkpoint_key!r}: {proc.stderr}"
            )
        first = next((line.strip() for line in proc.stdout.splitlines() if line.strip()), "")
        if first:
            return Path(first).resolve()

    raise FileNotFoundError(
        f"checkpoint {checkpoint_key!r} not found under {cpt_dir}"
    )


def parse_env_overrides(items: list[str]) -> dict[str, str]:
    env: dict[str, str] = {}
    for item in items:
        if "=" not in item:
            raise ValueError(f"--env expects KEY=VALUE, got {item!r}")
        key, value = item.split("=", 1)
        if not ENV_KEY_RE.fullmatch(key):
            raise ValueError(
                f"--env expects a valid shell variable KEY, got {item!r}"
            )
        env[key] = value
    return env


def collect_job_env(gem5_home: Path, build_type: str, overrides: dict[str, str]) -> dict[str, str]:
    names = [
        "GEM5_HOME",
        "GEM5_BUILD_TYPE",
        "GCBV_REF_SO",
        "GCBV_MULTI_CORE_REF_SO",
        "GCB_MULTI_CORE_RESTORER",
        "GCBV_RESTORER",
        "GCBH_REF_SO",
        "GCBH_RESTORER",
        "NEMU_HOME",
        "AM_HOME",
        "LD_LIBRARY_PATH",
        "PYTHONPATH",
    ]
    env = {name: os.environ[name] for name in names if name in os.environ}
    env.setdefault("GEM5_HOME", str(gem5_home))
    env.setdefault("GEM5_BUILD_TYPE", build_type)
    env.update(overrides)
    return env


def shell_exports(env: dict[str, str]) -> str:
    lines = []
    for key in sorted(env):
        if not ENV_KEY_RE.fullmatch(key):
            raise ValueError(f"invalid environment variable name: {key!r}")
        lines.append(f"export {key}={shlex.quote(env[key])}")
    return "\n".join(lines)


def build_gem5_command(
    gem5: Path,
    config_or_script: Path,
    checkpoint: Path,
    extra_gem5_args: str,
) -> list[str]:
    if config_or_script.suffix == ".sh":
        return ["bash", str(config_or_script), str(checkpoint)]
    if config_or_script.suffix != ".py":
        raise ValueError(
            f"first argument must be a .py config or .sh wrapper: {config_or_script}"
        )
    command = [
        str(gem5),
        str(config_or_script),
        f"--generic-rv-cpt={checkpoint}",
        *shlex.split(extra_gem5_args),
    ]
    if checkpoint.suffix == ".bin":
        command.insert(2, "--raw-cpt")
    return command


def make_job_script(
    work_dir: Path,
    workload: Workload,
    checkpoint: Path,
    command: list[str],
    env: dict[str, str],
) -> str:
    command_text = " ".join(shlex.quote(part) for part in command)
    workload_message = shlex.quote(f"distributed_sim workload: {workload.name}")
    checkpoint_message = shlex.quote(f"checkpoint: {checkpoint}")
    command_message = shlex.quote(f"command: {command_text}")
    return f"""set -u
mkdir -p {shlex.quote(str(work_dir))}
cd {shlex.quote(str(work_dir))}
exec >{shlex.quote("log.txt")} 2>&1
printf '%s\\n' {workload_message}
printf '%s\\n' {checkpoint_message}
echo "host: $(hostname)"
echo "start: $(date -Is)"
printf '%s\\n' {command_message}
{shell_exports(env)}
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


def launch_job(
    server: ServerState,
    workload: Workload,
    work_dir: Path,
    script: str,
    ssh_config: str,
    ssh_options: list[str],
    ssh_user: str,
    dispatch_host: str,
    attempt: int,
) -> PendingJob:
    if server.name == "local":
        proc = subprocess.Popen(
            ["bash", "-lc", script],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            start_new_session=True,
        )
    else:
        ssh_cmd = build_ssh_command(
            target=make_ssh_target(server.name, ssh_user),
            remote_command=["bash", "-lc", script],
            ssh_config=ssh_config,
            ssh_options=ssh_options,
            fixed_options=[
                "BatchMode=yes",
                "ConnectionAttempts=3",
                "TCPKeepAlive=yes",
                "ServerAliveInterval=300",
                "ServerAliveCountMax=576",
            ],
        )
        if dispatch_host:
            ssh_cmd = wrap_with_dispatch_host(
                ssh_cmd=ssh_cmd,
                dispatch_host=dispatch_host,
                ssh_config=ssh_config,
                ssh_options=ssh_options,
            )
        proc = subprocess.Popen(
            ssh_cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            start_new_session=True,
        )
    job = PendingJob(
        workload=workload,
        server=server,
        work_dir=work_dir,
        proc=proc,
        started_at=time.time(),
        attempt=attempt,
    )
    server.pending.append(job)
    return job


def run_host_command(
    server_name: str,
    command: list[str],
    ssh_config: str,
    ssh_options: list[str],
    ssh_user: str,
    dispatch_host: str,
    timeout: float,
) -> subprocess.CompletedProcess[bytes]:
    if server_name == "local":
        return subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
            check=False,
        )

    ssh_cmd = build_ssh_command(
        target=make_ssh_target(server_name, ssh_user),
        remote_command=command,
        ssh_config=ssh_config,
        ssh_options=ssh_options,
        fixed_options=[
            "BatchMode=yes",
            "ConnectionAttempts=1",
            "TCPKeepAlive=yes",
        ],
    )
    if dispatch_host:
        ssh_cmd = wrap_with_dispatch_host(
            ssh_cmd=ssh_cmd,
            dispatch_host=dispatch_host,
            ssh_config=ssh_config,
            ssh_options=ssh_options,
        )
    return subprocess.run(
        ssh_cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
        check=False,
    )


def make_ssh_target(server_name: str, ssh_user: str) -> str:
    if not ssh_user or server_name == "local" or "@" in server_name:
        target = server_name
    else:
        target = f"{ssh_user}@{server_name}"
    validate_ssh_target_name(target, "--servers")
    return target


def build_ssh_command(
    target: str,
    remote_command: list[str],
    ssh_config: str,
    ssh_options: list[str],
    fixed_options: list[str],
) -> list[str]:
    ssh_cmd = ["ssh"]
    if ssh_config:
        ssh_cmd.extend(["-F", ssh_config])
    for option in fixed_options:
        ssh_cmd.extend(["-o", option])
    for option in ssh_options:
        ssh_cmd.extend(["-o", option])
    remote_command_text = " ".join(shlex.quote(part) for part in remote_command)
    ssh_cmd.extend([target, remote_command_text])
    return ssh_cmd


def wrap_with_dispatch_host(
    ssh_cmd: list[str],
    dispatch_host: str,
    ssh_config: str,
    ssh_options: list[str],
) -> list[str]:
    validate_ssh_target_name(dispatch_host, "--dispatch-host")
    dispatch_script = "exec " + " ".join(shlex.quote(part) for part in ssh_cmd)
    return build_ssh_command(
        target=dispatch_host,
        remote_command=["bash", "-lc", dispatch_script],
        ssh_config=ssh_config,
        ssh_options=ssh_options,
        fixed_options=[
            "BatchMode=yes",
            "ConnectionAttempts=1",
            "TCPKeepAlive=yes",
        ],
    )


def probe_idle_cpus(
    server_name: str,
    idle_probe_mode: str,
    idle_cpu_threshold: float,
    ssh_config: str,
    ssh_options: list[str],
    ssh_user: str,
    dispatch_host: str,
    timeout: float = 10.0,
) -> tuple[int | None, str]:
    script = (
        "python3 -c "
        + shlex.quote(
            "import os,time\n"
            "def read_stat():\n"
            "    rows={}\n"
            "    with open('/proc/stat') as f:\n"
            "        for line in f:\n"
            "            if not line.startswith('cpu') or line.startswith('cpu '):\n"
            "                continue\n"
            "            parts=line.split()\n"
            "            cpu=int(parts[0][3:])\n"
            "            vals=list(map(int, parts[1:]))\n"
            "            idle=vals[3]+(vals[4] if len(vals)>4 else 0)\n"
            "            total=sum(vals)\n"
            "            rows[cpu]=(idle,total)\n"
            "    return rows\n"
            "def parse_cpu_list(text):\n"
            "    cpus=[]\n"
            "    for part in text.strip().split(','):\n"
            "        if not part:\n"
            "            continue\n"
            "        if '-' in part:\n"
            "            start,end=map(int, part.split('-', 1))\n"
            "            cpus.extend(range(start, end + 1))\n"
            "        else:\n"
            "            cpus.append(int(part))\n"
            "    return cpus\n"
            "def read_core_groups(cpus):\n"
            "    groups={}\n"
            "    online=set(cpus)\n"
            "    for cpu in cpus:\n"
            "        path=f'/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list'\n"
            "        try:\n"
            "            with open(path) as f:\n"
            "                siblings=parse_cpu_list(f.read())\n"
            "        except OSError:\n"
            "            siblings=[cpu]\n"
            "        group=tuple(sorted(set(siblings) & online)) or (cpu,)\n"
            "        groups[group]=group\n"
            "    return list(groups)\n"
            "a=read_stat()\n"
            "time.sleep(1.0)\n"
            "b=read_stat()\n"
            f"threshold={idle_cpu_threshold!r}\n"
            f"mode={idle_probe_mode!r}\n"
            "busy_by_cpu={}\n"
            "for cpu,(bi,bt) in b.items():\n"
            "    if cpu not in a:\n"
            "        continue\n"
            "    ai,at=a[cpu]\n"
            "    dt=bt-at\n"
            "    busy=0.0 if dt<=0 else 100.0*(1.0-(bi-ai)/dt)\n"
            "    busy_by_cpu[cpu]=busy\n"
            "logical_idle=sum(1 for busy in busy_by_cpu.values() if busy < threshold)\n"
            "logical_total=len(busy_by_cpu)\n"
            "if mode == 'physical':\n"
            "    groups=read_core_groups(sorted(busy_by_cpu))\n"
            "    idle_count=sum(1 for group in groups if all(busy_by_cpu[cpu] < threshold for cpu in group))\n"
            "    total_count=len(groups)\n"
            "else:\n"
            "    idle_count=logical_idle\n"
            "    total_count=logical_total\n"
            "print(idle_count, total_count, logical_idle, logical_total, os.getloadavg()[0])\n"
        )
    )
    try:
        result = run_host_command(
            server_name=server_name,
            command=["bash", "-lc", script],
            ssh_config=ssh_config,
            ssh_options=ssh_options,
            ssh_user=ssh_user,
            dispatch_host=dispatch_host,
            timeout=timeout,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        return None, str(exc)

    if result.returncode != 0:
        stderr = result.stderr.decode(errors="replace").strip()
        stdout = result.stdout.decode(errors="replace").strip()
        return None, stderr or stdout or f"probe exited with {result.returncode}"

    text = result.stdout.decode(errors="replace").strip()
    match = re.search(r"(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+([0-9.]+)", text)
    if match is None:
        return None, f"unexpected probe output: {text!r}"
    idle = int(match.group(1))
    total = int(match.group(2))
    logical_idle = int(match.group(3))
    logical_total = int(match.group(4))
    load1 = match.group(5)
    if idle_probe_mode == "physical":
        return (
            idle,
            f"idle_physical_cores={idle}/{total}, "
            f"idle_logical_cpus={logical_idle}/{logical_total}, load1={load1}",
        )
    return (
        idle,
        f"idle_logical_cpus={idle}/{total}, load1={load1}",
    )


def filter_servers_by_idle(
    servers: list[ServerState],
    require_idle_cpus: int,
    idle_probe_mode: str,
    idle_cpu_threshold: float,
    ssh_config: str,
    ssh_options: list[str],
    ssh_user: str,
    dispatch_host: str,
) -> list[ServerState]:
    if require_idle_cpus <= 0:
        return servers

    selected: list[ServerState] = []
    for server in servers:
        idle_cpus, detail = probe_idle_cpus(
            server_name=server.name,
            idle_probe_mode=idle_probe_mode,
            idle_cpu_threshold=idle_cpu_threshold,
            ssh_config=ssh_config,
            ssh_options=ssh_options,
            ssh_user=ssh_user,
            dispatch_host=dispatch_host,
        )
        server.idle_cpus = idle_cpus
        if idle_cpus is None:
            server.idle_probe_error = detail
            print(f"[idle-skip] {server.name}: {detail}", flush=True)
            continue
        if idle_cpus < require_idle_cpus:
            print(
                f"[idle-skip] {server.name}: {detail}, require>={require_idle_cpus}",
                flush=True,
            )
            continue
        print(f"[idle-use] {server.name}: {detail}", flush=True)
        selected.append(server)

    if not selected:
        details = "; ".join(
            f"{server.name}: {server.idle_probe_error or server.idle_cpus}"
            for server in servers
        )
        raise RuntimeError(
            f"no servers satisfy --require-idle-cpus={require_idle_cpus}. {details}"
        )
    return selected


def append_launcher_output(job: PendingJob, stdout: bytes, stderr: bytes) -> None:
    if not stdout and not stderr:
        return
    job.work_dir.mkdir(parents=True, exist_ok=True)
    log_path = job.work_dir / "log.txt"
    with log_path.open("ab") as handle:
        handle.write(b"\n===== distributed_sim launcher output =====\n")
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


def mark_launcher_failure(job: PendingJob, message: str) -> None:
    job.work_dir.mkdir(parents=True, exist_ok=True)
    (job.work_dir / "running").unlink(missing_ok=True)
    (job.work_dir / "completed").unlink(missing_ok=True)
    (job.work_dir / "abort").touch()
    with (job.work_dir / "log.txt").open("a", encoding="utf-8") as handle:
        handle.write("\n===== distributed_sim launcher failure =====\n")
        handle.write(message)
        if not message.endswith("\n"):
            handle.write("\n")


def wait_for_visible_marker(job: PendingJob, timeout: float) -> str:
    deadline = time.time() + timeout
    while True:
        if (job.work_dir / "completed").exists():
            return "completed"
        if (job.work_dir / "abort").exists():
            return "abort"
        if timeout == 0 or time.time() >= deadline:
            return ""
        time.sleep(min(0.5, max(deadline - time.time(), 0.0)))


def has_any_marker(work_dir: Path) -> bool:
    return any(
        (work_dir / marker).exists()
        for marker in ("running", "completed", "abort")
    )


def clear_stale_markers(work_dir: Path, force: bool) -> None:
    if force:
        (work_dir / "completed").unlink(missing_ok=True)
    for marker in ("running", "abort"):
        (work_dir / marker).unlink(missing_ok=True)


def append_launcher_retry(job: PendingJob, result: int, delay: float) -> None:
    job.work_dir.mkdir(parents=True, exist_ok=True)
    with (job.work_dir / "log.txt").open("a", encoding="utf-8") as handle:
        handle.write("\n===== distributed_sim launcher retry =====\n")
        handle.write(
            f"attempt {job.attempt} failed before remote status markers "
            f"were created (exit={result}); retrying after {delay:.1f}s.\n"
        )


def poll_jobs(
    servers: list[ServerState],
    marker_timeout: float,
    launch_retries: int,
    launch_retry_delay: float,
) -> tuple[int, int, list[ScheduledWorkload]]:
    completed = 0
    failed = 0
    retry_workloads: list[ScheduledWorkload] = []
    for server in servers:
        still_pending: list[PendingJob] = []
        for job in server.pending:
            result = job.proc.poll()
            if result is None:
                still_pending.append(job)
                continue
            stdout, stderr = job.proc.communicate()
            append_launcher_output(job, stdout, stderr)
            elapsed = time.time() - job.started_at
            marker_wait = marker_timeout if result == 0 else min(marker_timeout, 2.0)
            marker = wait_for_visible_marker(job, marker_wait)
            if result == 0 and marker != "completed":
                result = 1
                mark_launcher_failure(
                    job,
                    (
                        "Remote process exited successfully, but the completed "
                        f"marker did not become visible within {marker_timeout:.1f}s "
                        f"at {job.work_dir}. The launcher cwd may not be shared "
                        "with the remote host, or NFS metadata propagation is too slow."
                    ),
                )
            if result == 0:
                completed += 1
                print(
                    f"[done] {job.workload.name} on {server.name} "
                    f"elapsed={elapsed:.1f}s",
                    flush=True,
                )
            else:
                if (
                    server.name != "local"
                    and result == 255
                    and job.attempt <= launch_retries
                    and marker == ""
                    and not has_any_marker(job.work_dir)
                ):
                    delay = launch_retry_delay * job.attempt
                    append_launcher_retry(job, result, delay)
                    retry_workloads.append(
                        ScheduledWorkload(
                            workload=job.workload,
                            ready_at=time.time() + delay,
                            attempt=job.attempt + 1,
                        )
                    )
                    print(
                        f"[retry] {job.workload.name} on {server.name} "
                        f"exit={result} attempt={job.attempt}/{launch_retries + 1} "
                        f"delay={delay:.1f}s elapsed={elapsed:.1f}s",
                        flush=True,
                    )
                    continue
                if marker == "" and not has_any_marker(job.work_dir):
                    mark_launcher_failure(
                        job,
                        (
                            "Launcher failed before the remote job created any "
                            f"status marker (exit={result}) after attempt "
                            f"{job.attempt}/{launch_retries + 1}."
                        ),
                    )
                failed += 1
                print(
                    f"[fail] {job.workload.name} on {server.name} "
                    f"exit={result} attempt={job.attempt}/{launch_retries + 1} "
                    f"elapsed={elapsed:.1f}s",
                    flush=True,
                )
        server.pending = still_pending
    return completed, failed, retry_workloads


def stop_pending_jobs(servers: list[ServerState]) -> None:
    for server in servers:
        for job in server.pending:
            if job.proc.poll() is None:
                try:
                    os.killpg(job.proc.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass


def abort_pending_jobs(servers: list[ServerState], message: str) -> None:
    for server in servers:
        for job in server.pending:
            if not (job.work_dir / "completed").exists():
                mark_launcher_failure(job, message)


def select_server(servers: list[ServerState], jobs_per_server: int) -> ServerState | None:
    available = [server for server in servers if len(server.pending) < jobs_per_server]
    if not available:
        return None
    return min(available, key=lambda server: (len(server.pending), server.name))


def run_scheduler(
    workloads: list[Workload],
    cpt_dir: Path,
    full_work_dir: Path,
    gem5: Path,
    config_or_script: Path,
    servers: list[ServerState],
    jobs_per_server: int,
    poll_interval: float,
    marker_timeout: float,
    extra_gem5_args: str,
    env: dict[str, str],
    ssh_config: str,
    ssh_options: list[str],
    ssh_user: str,
    dispatch_host: str,
    launch_retries: int,
    launch_retry_delay: float,
    launch_interval: float,
    force: bool,
) -> int:
    pending_workloads = [ScheduledWorkload(workload) for workload in workloads]
    total = len(pending_workloads)
    skipped = 0
    first_launches = 0
    launch_attempts = 0
    completed = 0
    failed = 0
    last_launch_at = 0.0
    checkpoint_by_workload: dict[Workload, Path] = {}
    for workload in workloads:
        work_dir = full_work_dir / workload.name
        if (work_dir / "completed").exists() and not force:
            continue
        checkpoint_by_workload[workload] = find_checkpoint(
            cpt_dir, workload.checkpoint_key
        )

    try:
        while pending_workloads or any(server.pending for server in servers):
            new_completed, new_failed, retry_workloads = poll_jobs(
                servers=servers,
                marker_timeout=marker_timeout,
                launch_retries=launch_retries,
                launch_retry_delay=launch_retry_delay,
            )
            completed += new_completed
            failed += new_failed
            pending_workloads.extend(retry_workloads)

            launched_this_round = False
            while pending_workloads:
                server = select_server(servers, jobs_per_server)
                if server is None:
                    break

                now = time.time()
                ready_index = next(
                    (
                        index
                        for index, item in enumerate(pending_workloads)
                        if item.ready_at <= now
                    ),
                    -1,
                )
                if ready_index < 0:
                    break
                if last_launch_at and now - last_launch_at < launch_interval:
                    break

                scheduled = pending_workloads.pop(ready_index)
                workload = scheduled.workload
                work_dir = full_work_dir / workload.name
                completed_marker = work_dir / "completed"
                if completed_marker.exists() and not force:
                    skipped += 1
                    print(f"[skip] {workload.name} already completed", flush=True)
                    continue
                clear_stale_markers(work_dir, force=force)

                checkpoint = checkpoint_by_workload[workload]
                command = build_gem5_command(
                    gem5, config_or_script, checkpoint, extra_gem5_args
                )
                script = make_job_script(
                    work_dir=work_dir,
                    workload=workload,
                    checkpoint=checkpoint,
                    command=command,
                    env=env,
                )
                launch_job(
                    server=server,
                    workload=workload,
                    work_dir=work_dir,
                    script=script,
                    ssh_config=ssh_config,
                    ssh_options=ssh_options,
                    ssh_user=ssh_user,
                    dispatch_host=dispatch_host,
                    attempt=scheduled.attempt,
                )
                launch_attempts += 1
                if scheduled.attempt == 1:
                    first_launches += 1
                launched_this_round = True
                last_launch_at = time.time()
                if scheduled.attempt == 1:
                    print(
                        f"[start] {workload.name} on {server.name} "
                        f"({first_launches}/{total})",
                        flush=True,
                    )
                else:
                    print(
                        f"[start] {workload.name} on {server.name} "
                        f"retry={scheduled.attempt}/{launch_retries + 1} "
                        f"launch_attempt={launch_attempts}",
                        flush=True,
                    )

            if pending_workloads or any(server.pending for server in servers):
                if not launched_this_round:
                    sleep_for = poll_interval
                    if pending_workloads:
                        next_ready = min(item.ready_at for item in pending_workloads)
                        sleep_for = min(
                            sleep_for,
                            max(next_ready - time.time(), 0.0),
                        )
                    if last_launch_at:
                        launch_delay = launch_interval - (time.time() - last_launch_at)
                        if launch_delay > 0:
                            sleep_for = min(sleep_for, launch_delay)
                    time.sleep(max(sleep_for, 0.05))

    except KeyboardInterrupt:
        print("Interrupted; terminating launcher-side ssh/bash processes.", file=sys.stderr)
        stop_pending_jobs(servers)
        abort_pending_jobs(servers, "Scheduler interrupted; marking pending job aborted.")
        raise
    except Exception:
        print(
            "Scheduler exception; terminating launcher-side ssh/bash processes.",
            file=sys.stderr,
        )
        stop_pending_jobs(servers)
        abort_pending_jobs(
            servers,
            "Scheduler raised an exception; marking pending job aborted.",
        )
        raise

    retry_attempts = launch_attempts - first_launches
    print(
        f"Summary: total={total} launch_attempts={launch_attempts} skipped={skipped} "
        f"completed={completed} failed={failed} "
        f"retry_attempts={max(retry_attempts, 0)}",
        flush=True,
    )
    return 0


def main(argv: list[str]) -> int:
    args, rest = parse_launcher_args(argv)
    if args.dispatch_host:
        validate_ssh_target_name(args.dispatch_host, "--dispatch-host")

    first_param = rest[0]
    workload_list = Path(rest[1]).resolve()
    cpt_dir = Path(rest[2]).resolve()
    tag = rest[3]
    benchmark_filters = rest[4] if len(rest) >= 5 else ""
    legacy_extra_args = " ".join(rest[5:]) if len(rest) >= 6 else ""
    if len(rest) == 5 and benchmark_filters.startswith("-"):
        legacy_extra_args = benchmark_filters
        benchmark_filters = ""
    extra_gem5_args = args.extra_gem5_args if args.extra_gem5_args else legacy_extra_args

    gem5_home = Path(args.gem5_home or os.environ.get("GEM5_HOME", "") or repo_root_from_script()).resolve()
    config_or_script = resolve_config_or_script(first_param, gem5_home)
    gem5 = Path(args.gem5).resolve() if args.gem5 else (
        gem5_home / "build" / "RISCV" / f"gem5.{args.build_type}"
    ).resolve()
    server_names = apply_server_domain(parse_server_list(args.servers), args.server_domain)
    servers = [ServerState(name=name, pending=[]) for name in server_names]
    servers = filter_servers_by_idle(
        servers=servers,
        require_idle_cpus=args.require_idle_cpus,
        idle_probe_mode=args.idle_probe_mode,
        idle_cpu_threshold=args.idle_cpu_threshold,
        ssh_config=args.ssh_config,
        ssh_options=args.ssh_option,
        ssh_user=args.ssh_user,
        dispatch_host=args.dispatch_host,
    )

    full_work_dir = Path.cwd().resolve() / tag
    full_work_dir.mkdir(parents=True, exist_ok=True)

    workloads = read_workload_list(workload_list)
    workloads = apply_benchmark_filter(workloads, benchmark_filters)
    if args.max_tasks:
        workloads = workloads[: args.max_tasks]
    if not workloads:
        raise ValueError("no workloads selected")

    env = collect_job_env(
        gem5_home=gem5_home,
        build_type=args.build_type,
        overrides=parse_env_overrides(args.env),
    )

    print(f"Using gem5 binary: {gem5}")
    print(f"Using config/script: {config_or_script}")
    print(f"Work directory: {full_work_dir}")
    print(f"Selected workloads: {len(workloads)}")
    print(
        "Servers: "
        + ", ".join(f"{server.name}({args.jobs_per_server})" for server in servers)
    )
    if benchmark_filters.strip():
        print(f"Benchmark filters: {benchmark_filters}")
    if extra_gem5_args.strip():
        print(f"Extra gem5 args: {extra_gem5_args}")

    if args.dry_run:
        for index, workload in enumerate(workloads, start=1):
            checkpoint = find_checkpoint(cpt_dir, workload.checkpoint_key)
            command = build_gem5_command(
                gem5, config_or_script, checkpoint, extra_gem5_args
            )
            server = servers[(index - 1) % len(servers)].name
            print(
                f"[dry-run] {workload.name} -> {server} "
                f"checkpoint={checkpoint} command={' '.join(shlex.quote(part) for part in command)}"
            )
        return 0

    return run_scheduler(
        workloads=workloads,
        cpt_dir=cpt_dir,
        full_work_dir=full_work_dir,
        gem5=gem5,
        config_or_script=config_or_script,
        servers=servers,
        jobs_per_server=args.jobs_per_server,
        poll_interval=args.poll_interval,
        marker_timeout=args.marker_timeout,
        extra_gem5_args=extra_gem5_args,
        env=env,
        ssh_config=args.ssh_config,
        ssh_options=args.ssh_option,
        ssh_user=args.ssh_user,
        dispatch_host=args.dispatch_host,
        launch_retries=args.launch_retries,
        launch_retry_delay=args.launch_retry_delay,
        launch_interval=args.launch_interval,
        force=args.force,
    )


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except Exception as exc:
        print(f"distributed_sim.py: error: {exc}", file=sys.stderr)
        raise SystemExit(1)
