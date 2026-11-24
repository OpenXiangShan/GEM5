"""SSH transport and idle-core discovery on the shared CI pool."""
import re
import shlex
import subprocess

# SSH stdin is a lifetime pipe, not simulator input. Run this on the worker
# so losing the launcher never requires a second SSH connection to clean up.
SSH_JOB_GUARD = """\
import os
import select
import signal
import subprocess
import sys

with subprocess.Popen(
    sys.argv[1:], stdin=subprocess.DEVNULL, start_new_session=True
) as sim:
    while sim.poll() is None:
        if not select.select([0], [], [], 1)[0]:
            continue
        if sim.poll() is None:
            try:
                os.killpg(sim.pid, signal.SIGTERM)
                try:
                    sim.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    os.killpg(sim.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        break
status = sim.returncode
sys.exit(status if status >= 0 else 128 - status)
"""


def validate_ssh_target_name(name: str, option_name: str) -> None:
    if name != "local" and name.startswith("-"):
        raise ValueError(
            f"{option_name} contains an invalid SSH target: {name!r}"
        )


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
            r"([A-Za-z_-]*)(\d+)-([A-Za-z_-]*)(\d+)",
            token,
        )
        if range_match:
            prefix, start_text, end_prefix, end_text = range_match.groups()
            if end_prefix and end_prefix != prefix:
                raise ValueError(f"server range prefixes differ: {token}")
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
    return list(dict.fromkeys(servers))


def apply_server_domain(servers: list[str], domain: str) -> list[str]:
    domain = domain.strip().lstrip(".")
    if not domain:
        return list(dict.fromkeys(servers))
    return [
        server if server == "local" or "." in server else f"{server}.{domain}"
        for server in servers
    ]


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
    ssh_cmd.extend([target, shlex.join(remote_command)])
    return ssh_cmd


# Send the probe on stdin so workers only need Linux procfs and Python.
IDLE_PROBE = """\
from pathlib import Path
import sys
import time


def counters():
    result = {}
    for line in Path('/proc/stat').read_text().splitlines():
        name, *fields = line.split()
        if name.startswith('cpu') and name[3:].isdigit():
            ticks = list(map(int, fields))
            result[name] = (ticks[3] + ticks[4], sum(ticks[:8]))
    return result


mode, threshold = sys.argv[1:]
before = counters()
time.sleep(1)
groups = {}
for cpu, (idle, total) in counters().items():
    if cpu not in before:
        continue
    previous_idle, previous_total = before[cpu]
    elapsed = total - previous_total
    busy = 100 * (1 - (idle - previous_idle) / elapsed) if elapsed else 0
    group = cpu
    if mode == 'physical':
        try:
            group = (Path('/sys/devices/system/cpu') / cpu / 'topology'
                     / 'thread_siblings_list').read_text().strip()
        except OSError:
            pass
    groups[group] = groups.get(group, True) and busy < float(threshold)
print(sum(groups.values()))
"""


def probe_idle_cpus(
    server_name, mode, threshold, ssh_config, ssh_options, ssh_user, timeout=5
):
    command = ["python3", "-", mode, str(threshold)]
    if server_name != "local":
        command = build_ssh_command(
            make_ssh_target(server_name, ssh_user),
            command,
            ssh_config,
            ssh_options,
            ["BatchMode=yes", "ConnectionAttempts=1", "StdinNull=no"],
        )
    try:
        result = subprocess.run(
            command,
            input=IDLE_PROBE,
            capture_output=True,
            text=True,
            timeout=timeout,
            check=True,
        )
        return int(result.stdout.strip()), ""
    except subprocess.CalledProcessError as error:
        return (
            None,
            f"idle probe exited {error.returncode}: {error.stderr.strip()}",
        )
    except (OSError, subprocess.TimeoutExpired, ValueError) as error:
        return None, f"idle probe failed: {error}"
