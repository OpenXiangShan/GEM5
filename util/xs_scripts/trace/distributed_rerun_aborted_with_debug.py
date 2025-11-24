#!/usr/bin/env python3
"""
Distributed rerun helper for aborted trace-driven workloads with debug window.

Scan a work root (per-task subdirs with abort/log.txt), derive debug window
around abort, and dispatch reruns across a server list, similar to
distributed_trace_scheduler.py.

Example:
  python3 util/xs_scripts/trace/distributed_rerun_aborted_with_debug.py \
    --work-root /path/to/work_root \
    --server-list servers.txt \
    --arch-script util/xs_scripts/trace/run_trace_champsim.sh \
    --max-global 32 --max-threads-per-host 4 \
    --debug-flags IEW,Fetch,Commit,CommitTrace,DecoupleBP,TraceReader,Decode
"""

from __future__ import annotations

import argparse
import math
import os
import re
import shlex
import subprocess
import sys
import time
import signal
from pathlib import Path
from typing import Dict, List, Optional, Tuple


DEFAULT_DEBUG_FLAGS = "IEW,Fetch,Commit,CommitTrace,DecoupleBP,TraceReader,Decode"


def run(cmd: List[str], capture: bool = True, timeout: Optional[int] = None) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=capture, text=True, timeout=timeout)


def ssh(host: str, remote_cmd: str, timeout: int = 10) -> Tuple[int, str, str]:
    full = ["ssh", "-o", "BatchMode=yes", host, remote_cmd]
    proc = run(full, capture=True, timeout=timeout)
    return proc.returncode, proc.stdout.strip(), proc.stderr.strip()


def get_host_capacity(host: str, max_threads: int) -> Tuple[int, str]:
    """Return (capacity, info). Capacity respects half logical cores minus load."""
    rc, out_core, err = ssh(host, "nproc --all || getconf _NPROCESSORS_ONLN", timeout=5)
    if rc != 0:
        return 0, f"cores query failed: {err or out_core}"
    try:
        logical = int(out_core.strip().split()[0])
    except Exception:
        return 0, f"parse cores failed: {out_core}"
    rc, out_load, err = ssh(host, "awk '{print $1}' /proc/loadavg", timeout=5)
    if rc != 0:
        return 0, f"loadavg failed: {err or out_load}"
    try:
        load1 = float(out_load.strip().split()[0])
    except Exception:
        return 0, f"parse load failed: {out_load}"
    half = logical / 2.0
    avail = math.floor(half - load1)
    cap = max(0, min(max_threads, avail))
    info = f"cores={logical} half={half:.1f} load1={load1:.2f} cap={cap}"
    return cap, info


def parse_abort_info(task_dir: Path, trace_format: str, require_abort: bool = True) -> Optional[Dict[str, object]]:
    log_file = task_dir / "log.txt"
    abort_file = task_dir / "abort"
    if not log_file.exists() or (require_abort and not abort_file.exists()):
        return None

    abort_tick = None
    suggest_start = None
    suggest_end = None
    trace_file = None
    maxinsts = None

    pat_abort = re.compile(r"Program aborted at tick\s+(\d+)")
    pat_suggest = re.compile(r"--debug-start=(\d+).*--debug-end=(\d+)")
    pat_trace = re.compile(r"Trace file:\s+(\S+)")
    pat_max = re.compile(r"Max instructions:\s+(\d+)")

    with open(log_file, "r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            if abort_tick is None:
                m = pat_abort.search(line)
                if m:
                    abort_tick = int(m.group(1))
            if suggest_start is None:
                m = pat_suggest.search(line)
                if m:
                    suggest_start = int(m.group(1))
                    suggest_end = int(m.group(2))
            if trace_file is None:
                m = pat_trace.search(line)
                if m:
                    trace_file = m.group(1)
            if maxinsts is None:
                m = pat_max.search(line)
                if m:
                    maxinsts = int(m.group(1))

    if abort_tick is None or trace_file is None:
        return None

    trace_path = Path(trace_file)
    if not trace_path.exists():
        return None

    if maxinsts is None:
        maxinsts = 1_000_000

    start_tick = max(abort_tick - 1_000_000, 0)
    end_tick = abort_tick + 1000
    if suggest_start is not None and suggest_end is not None:
        start_tick, end_tick = suggest_start, suggest_end

    debug_dir = task_dir / "debug"

    return {
        "abort_tick": abort_tick,
        "start_tick": start_tick,
        "end_tick": end_tick,
        "trace_path": trace_path,
        "maxinsts": maxinsts,
        "debug_dir": debug_dir,
        "trace_format": trace_format,
    }


def launch_task(
    host: str,
    arch_script: Path,
    task_dir: Path,
    info: Dict[str, object],
    debug_flags: str,
) -> Tuple[bool, Optional[int]]:
    debug_dir: Path = info["debug_dir"]  # type: ignore
    trace_path: Path = info["trace_path"]  # type: ignore
    start_tick: int = info["start_tick"]  # type: ignore
    end_tick: int = info["end_tick"]  # type: ignore
    maxinsts: int = info["maxinsts"]  # type: ignore
    trace_format: str = info["trace_format"]  # type: ignore

    remote_cmd = f"""
mkdir -p {shlex.quote(str(debug_dir))} && cd {shlex.quote(str(debug_dir))} || exit 1
rm -f running completed abort exit_code remote_pid remote_host
touch running
nohup bash -lc '
OUTDIR=$PWD XS_MAX_INSTS={maxinsts} XS_DEBUG_FLAGS={shlex.quote(debug_flags)} \\
XS_DEBUG_START={start_tick} XS_DEBUG_END={end_tick} TRACE_FORMAT={shlex.quote(trace_format)} \\
bash {shlex.quote(str(arch_script))} {shlex.quote(str(trace_path))} > log.txt 2>&1
status=$?
rm -f running
if [ $status -eq 0 ]; then
  touch completed
else
  echo $status > exit_code
  touch abort
fi
' >/dev/null 2>&1 &
pid=$!
echo $pid > remote_pid
echo {host} > remote_host
echo $pid
"""
    rc, out, err = ssh(host, remote_cmd, timeout=20)
    if rc != 0:
        print(f"[WARN] launch failed on {host}: {err}", file=sys.stderr)
        return False, None
    pid: Optional[int] = None
    try:
        pid = int(out.strip().split()[-1])
    except Exception:
        pass
    return True, pid


def poll_status(task_dir: Path) -> str:
    if (task_dir / "completed").exists():
        return "completed"
    if (task_dir / "abort").exists():
        return "abort"
    if (task_dir / "running").exists():
        return "running"
    return "pending"


def load_servers(path: Path) -> List[str]:
    hosts: List[str] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        hosts.append(line.strip())
    return hosts


def terminate_running(running: Dict[str, Tuple[str, Path, Dict[str, object]]]) -> int:
    """Best-effort kill running remote tasks on SIGINT."""
    killed = 0
    for _, (name, _task_dir, info) in running.items():
        host = info.get("host")
        debug_dir = info.get("debug_dir")
        if not host or not debug_dir:
            continue
        remote_cmd = (
            f"cd {shlex.quote(str(debug_dir))} && "
            "if [ -f remote_pid ]; then pid=$(cat remote_pid); "
            "kill -TERM $pid 2>/dev/null || true; fi; "
            "touch abort; rm -f running"
        )
        rc, out, err = ssh(host, remote_cmd, timeout=10)
        killed += 1
        msg = err or out
        if rc != 0:
            print(f"[WARN] failed to terminate on {host}: {msg}", file=sys.stderr)
        else:
            print(f"[CANCEL] sent SIGTERM to task on {host} (dir={debug_dir})")
    return killed


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Distributed rerun aborted trace workloads with debug flags."
    )
    ap.add_argument(
        "--work-root",
        required=True,
        help="Work root containing per-workload dirs with abort/log.txt",
    )
    ap.add_argument(
        "--server-list",
        required=True,
        help="File listing servers (one host per line)",
    )
    ap.add_argument(
        "--arch-script",
        default="util/xs_scripts/trace/run_trace_champsim.sh",
        help="Path to trace arch script",
    )
    ap.add_argument(
        "--debug-flags", default=DEFAULT_DEBUG_FLAGS, help="Comma-separated debug flags"
    )
    ap.add_argument(
        "--trace-format",
        default=None,
        help="Trace format (champsim/cbp2025); defaults to env TRACE_FORMAT or champsim",
    )
    ap.add_argument(
        "--max-global", type=int, default=16, help="Global max concurrent reruns"
    )
    ap.add_argument(
        "--max-threads-per-host",
        type=int,
        default=4,
        help="Per-host max concurrency",
    )
    ap.add_argument(
        "--poll-seconds", type=int, default=20, help="Polling interval seconds"
    )
    ap.add_argument(
        "--clear-old-abort",
        action="store_true",
        help="Remove existing abort markers before rerun to distinguish new aborts",
    )
    args = ap.parse_args()

    work_root = Path(args.work_root).resolve()
    arch_script = Path(args.arch_script).resolve()
    server_list = Path(args.server_list).resolve()

    if not work_root.is_dir():
        print(f"work_root is not a directory: {work_root}", file=sys.stderr)
        return 1
    if not arch_script.exists():
        print(f"arch_script not found: {arch_script}", file=sys.stderr)
        return 1
    if not server_list.exists():
        print(f"server_list not found: {server_list}", file=sys.stderr)
        return 1

    trace_format = args.trace_format or os.environ.get("TRACE_FORMAT", "champsim")

    hosts = load_servers(server_list)
    if not hosts:
        print("no hosts loaded from server_list", file=sys.stderr)
        return 1

    pending: List[Tuple[str, Path, Dict[str, object]]] = []
    for sub in work_root.iterdir():
        if not sub.is_dir():
            continue
        info = parse_abort_info(sub, trace_format, require_abort=not args.clear_old_abort)
        if info is None:
            continue
        # Optionally clear old abort marker so new aborts are distinguishable
        if args.clear_old_abort:
            abort_file = sub / "abort"
            if abort_file.exists():
                prev = sub / "abort.prev"
                try:
                    if prev.exists():
                        abort_file.unlink()
                    else:
                        abort_file.rename(prev)
                except Exception:
                    abort_file.unlink(missing_ok=True)
        pending.append((sub.name, sub, info))
    pending.sort(key=lambda x: x[0])

    if not pending:
        print("no aborted workloads found", file=sys.stderr)
        return 0

    running: Dict[str, Tuple[str, Path, Dict[str, object]]] = {}
    host_running: Dict[str, int] = {h: 0 for h in hosts}

    print(
        f"Loaded {len(pending)} aborted workloads, "
        f"dispatching across {len(hosts)} hosts."
    )

    try:
        while pending or running:
            # poll running tasks
            finished: List[str] = []
            for task_id, (name, task_dir, info) in list(running.items()):
                status = poll_status(info["debug_dir"])  # type: ignore
                if status in ("completed", "abort"):
                    finished.append(task_id)
                    host = info.get("host")  # type: ignore
                    if host:
                        host_running[host] = max(0, host_running.get(host, 1) - 1)
                    print(f"[{name}] status={status}")
            for tid in finished:
                running.pop(tid, None)

            # compute capacities
            host_capacity: Dict[str, int] = {}
            for h in hosts:
                cap, info = get_host_capacity(h, args.max_threads_per_host)
                host_capacity[h] = cap
                # optional debug info
                # print(f"[host {h}] {info}", file=sys.stderr)

            global_running = len(running)
            while pending and global_running < args.max_global:
                # pick first host with capacity
                target_host = None
                for h in hosts:
                    if host_running.get(h, 0) < host_capacity.get(h, 0):
                        target_host = h
                        break
                if target_host is None:
                    break  # no capacity now

                name, task_dir, info = pending.pop(0)
                ok, pid = launch_task(
                    target_host, arch_script, task_dir, info, args.debug_flags
                )
                if not ok:
                    print(
                        f"[{name}] launch failed on {target_host}, skipping",
                        file=sys.stderr,
                    )
                    continue
                info["host"] = target_host
                info["pid"] = pid
                running[str(task_dir)] = (name, task_dir, info)
                host_running[target_host] = host_running.get(target_host, 0) + 1
                global_running += 1
                print(
                    f"[{name}] dispatched to {target_host} (pid={pid}) "
                    f"start={info['start_tick']} end={info['end_tick']}"
                )

            if pending or running:
                time.sleep(args.poll_seconds)
    except KeyboardInterrupt:
        print(
            "SIGINT received, attempting to terminate running reruns...",
            file=sys.stderr,
        )
        terminated = terminate_running(running)
        print(
            f"Termination attempts sent for {terminated} tasks.", file=sys.stderr
        )
        return 130

    print("All tasks completed or aborted.")
    return 0


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main())
