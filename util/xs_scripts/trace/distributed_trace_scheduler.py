#!/usr/bin/env python3
"""
Distributed trace scheduler for ChampSim / CBP2025 traces.

Features:
- Reads workload list (name path skip fw dw sample) and server list (one host per line).
- Respects per-host capacity: available <= min(max_threads_per_host, cores/2 - load1).
- Respects global max concurrency.
- Periodically polls load and task completion; fills new tasks when capacity is free.
- Runs tasks on remote hosts via SSH; work_dir/status files live on shared NFS.

Usage:
  TRACE_FORMAT=cbp2025 \
  python3 util/xs_scripts/trace/distributed_trace_scheduler.py \
    --arch-script util/xs_scripts/trace/run_trace_champsim.sh \
    --workload-list cbp_traces.lst \
    --trace-root /nfs/home/share/glr/cbp_traces \
    --server-list servers.txt \
    --tag cbp_run \
    --max-global 64 \
    --max-threads-per-host 8 \
    --poll-seconds 30

Outputs:
- Work root: $XSGEM5_WORK_ROOT/<tag> (default XSGEM5_WORK_ROOT or cwd/<tag>)
- Dispatch log: <work_root>/dispatch.tsv (task, host, trace_path, warmup, sample, status, start_ts, end_ts)
- Per-task status files in work_root/<task>/: running/completed/abort/log.txt (touched by remote command)
"""

from __future__ import annotations

import argparse
import math
import os
import shlex
import subprocess
import sys
import time
from pathlib import Path
from typing import Dict, List, Optional, Tuple


def run(
    cmd: List[str],
    check: bool = False,
    capture: bool = True,
    timeout: Optional[int] = None,
) -> subprocess.CompletedProcess:
    return subprocess.run(
        cmd, check=check, capture_output=capture, text=True, timeout=timeout
    )


def ssh(host: str, remote_cmd: str, timeout: int = 10) -> Tuple[int, str, str]:
    full = ["ssh", "-o", "BatchMode=yes", host, remote_cmd]
    proc = run(full, capture=True, timeout=timeout)
    return proc.returncode, proc.stdout.strip(), proc.stderr.strip()


def get_host_capacity(host: str, max_threads: int) -> Tuple[int, str]:
    """Return (capacity, info) where capacity is max runnable tasks on host."""
    # logical cores via nproc --all
    rc, out_core, err = ssh(
        host, "nproc --all || getconf _NPROCESSORS_ONLN", timeout=5
    )
    if rc != 0:
        return 0, f"nproc failed: {err or out_core}"
    try:
        logical = int(out_core.strip().split()[0])
    except (ValueError, IndexError):
        return 0, f"parse cores failed: {out_core}"
    # load1 from /proc/loadavg
    rc, out_load, err = ssh(host, "awk '{print $1}' /proc/loadavg", timeout=5)
    if rc != 0:
        return 0, f"loadavg failed: {err or out_load}"
    try:
        load1 = float(out_load.strip().split()[0])
    except (ValueError, IndexError):
        return 0, f"parse load failed: {out_load}"
    half = logical / 2.0
    avail = math.floor(half - load1)
    capacity = max(0, min(max_threads, avail))
    info = f"cores={logical} half={half:.1f} load1={load1:.2f} cap={capacity}"
    return capacity, info


def find_trace(trace_root: Path, rel_prefix: str) -> Optional[Path]:
    candidates = [
        trace_root / f"{rel_prefix}.xz",
        trace_root / f"{rel_prefix}.gz",
        trace_root / rel_prefix,
    ]
    for c in candidates:
        if c.exists():
            return c
    base = Path(rel_prefix).name
    for suffix in ("*.xz", "*.gz"):
        found = list(trace_root.rglob(f"{base}{suffix}"))
        if found:
            return found[0]
    return None


def load_workloads(path: Path) -> List[Tuple[str, str, int, int]]:
    """Return list of (task, trace_rel, warmup, sample). skip ignored."""
    items: List[Tuple[str, str, int, int]] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        parts = line.split()
        if len(parts) < 6:
            continue
        name, rel, _skip, fw, dw, sample = parts[:6]
        warmup = int(fw) + int(dw)
        sample_i = int(sample)
        items.append((name, rel, warmup, sample_i))
    return items


def load_servers(path: Path) -> List[str]:
    hosts: List[str] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        hosts.append(line.strip())
    return hosts


def launch_task(
    host: str,
    arch_script: Path,
    work_dir: Path,
    trace_path: Path,
    warmup: int,
    sample: int,
    trace_format: str,
) -> Tuple[bool, Optional[int]]:
    total = warmup + sample
    # Run in background on the remote host; mark status files on completion and emit pid.
    remote_cmd = f"""
mkdir -p {shlex.quote(str(work_dir))} && cd {shlex.quote(str(work_dir))} || exit 1
rm -f running completed abort exit_code remote_pid remote_host
touch running
nohup bash -lc '
OUTDIR=$PWD XS_MAX_INSTS={total} XS_WARMUP_INSTS_NO_SWITCH={warmup} TRACE_FORMAT={shlex.quote(trace_format)} \\
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
    except (ValueError, IndexError):
        print(
            f"[WARN] could not parse PID from output: {out!r}",
            file=sys.stderr,
        )
    return True, pid


def poll_status(task_dir: Path) -> str:
    if (task_dir / "completed").exists():
        return "completed"
    if (task_dir / "abort").exists():
        return "abort"
    if (task_dir / "running").exists():
        return "running"
    return "pending"


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Distributed trace scheduler over SSH (ChampSim/CBP)"
    )
    ap.add_argument(
        "--arch-script",
        required=True,
        help="Path to arch script (e.g., util/xs_scripts/trace/run_trace_champsim.sh)",
    )
    ap.add_argument(
        "--workload-list",
        required=True,
        help="Workload list (name path skip fw dw sample)",
    )
    ap.add_argument("--trace-root", required=True, help="Trace root directory")
    ap.add_argument(
        "--server-list",
        required=True,
        help="File listing target servers (one host per line)",
    )
    ap.add_argument(
        "--tag", required=True, help="Tag name for this run (used as work_dir name)"
    )
    ap.add_argument(
        "--max-global", type=int, required=True, help="Global max concurrent tasks"
    )
    ap.add_argument(
        "--max-threads-per-host",
        type=int,
        required=True,
        help="Max concurrent tasks per host",
    )
    ap.add_argument(
        "--poll-seconds",
        type=int,
        default=30,
        help="Polling interval for load and status",
    )
    ap.add_argument(
        "--trace-format",
        default=None,
        help="Trace format (champsim/cbp2025); defaults to env TRACE_FORMAT or champsim",
    )
    args = ap.parse_args()

    trace_format = args.trace_format or os.environ.get("TRACE_FORMAT", "champsim")

    arch_script = Path(args.arch_script).resolve()
    workload_list = Path(args.workload_list).resolve()
    trace_root = Path(args.trace_root).resolve()
    server_list = Path(args.server_list).resolve()

    if not arch_script.exists():
        print(f"arch_script not found: {arch_script}", file=sys.stderr)
        return 1
    if not workload_list.exists():
        print(f"workload_list not found: {workload_list}", file=sys.stderr)
        return 1
    if not trace_root.is_dir():
        print(f"trace_root is not dir: {trace_root}", file=sys.stderr)
        return 1
    if not server_list.exists():
        print(f"server_list not found: {server_list}", file=sys.stderr)
        return 1

    tasks = load_workloads(workload_list)
    if not tasks:
        print("no tasks parsed from workload list", file=sys.stderr)
        return 1
    total_tasks = len(tasks)
    print(f"[INFO] total tasks={total_tasks}")
    hosts = load_servers(server_list)
    if not hosts:
        print("no hosts in server list", file=sys.stderr)
        return 1

    work_root = (
        Path(os.environ.get("XSGEM5_WORK_ROOT", os.getcwd())).resolve() / args.tag
    )
    work_root.mkdir(parents=True, exist_ok=True)

    dispatch_log = work_root / "dispatch.tsv"
    if not dispatch_log.exists() or dispatch_log.stat().st_size == 0:
        dispatch_log.write_text(
            "task\thost\ttrace_path\twarmup\tsample\tstatus\tstart_ts\tend_ts\n",
            encoding="utf-8",
        )

    pending_idx = 0
    running: Dict[str, int] = {h: 0 for h in hosts}
    task_status: Dict[str, str] = {}
    task_host: Dict[str, str] = {}
    task_paths: Dict[str, Path] = {}
    start_ts: Dict[str, float] = {}
    warmup_map: Dict[str, int] = {}
    sample_map: Dict[str, int] = {}
    running_pids: Dict[str, int] = {}

    # Skip tasks already completed on disk
    for name, _rel, warmup, sample in tasks:
        tdir = work_root / name
        if (tdir / "completed").exists():
            task_status[name] = "completed"
            with dispatch_log.open("a", encoding="utf-8") as f:
                f.write(f"{name}\t-\t-\t{warmup}\t{sample}\tcompleted\t0\t0\n")
            print(f"[SKIP] {name} already completed; skipping.")

    try:
        while True:
            # Update finished tasks
            for name, _rel, warmup, sample in tasks:
                status = task_status.get(name, "pending")
                if status != "running":
                    continue
                tdir = work_root / name
                new_status = poll_status(tdir)
                if new_status in ("completed", "abort"):
                    task_status[name] = new_status
                    host = task_host.get(name, "?")
                    running[host] = max(0, running.get(host, 1) - 1)
                    running_pids.pop(name, None)
                    end_time = time.time()
                    elapsed = end_time - start_ts.get(name, end_time)
                    print(
                        f"[DONE] {name} on {host} status={new_status} "
                        f"elapsed={elapsed:.0f}s warmup={warmup} sample={sample}"
                    )
                    with dispatch_log.open("a", encoding="utf-8") as f:
                        f.write(
                            f"{name}\t{host}\t{task_paths[name]}\t{warmup}\t{sample}"
                            f"\t{new_status}\t{start_ts.get(name,0):.0f}"
                            f"\t{end_time:.0f}\n"
                        )

            total_done = sum(
                1
                for s in task_status.values()
                if s in ("completed", "abort", "launch_failed", "missing_trace")
            )
            if total_done == len(tasks):
                break

            # Global available slots
            total_running = sum(running.values())
            global_avail = max(0, args.max_global - total_running)
            if global_avail == 0 and pending_idx >= len(tasks):
                time.sleep(args.poll_seconds)
                continue

            # Check host capacities and schedule
            host_caps: Dict[str, int] = {}
            for host in hosts:
                cap, info = get_host_capacity(host, args.max_threads_per_host)
                host_caps[host] = max(0, cap - running.get(host, 0))
                print(
                    f"[CAP] {host} {info} running={running.get(host,0)} "
                    f"avail={host_caps[host]}"
                )

            made_progress = False
            while global_avail > 0:
                while pending_idx < len(tasks) and task_status.get(
                    tasks[pending_idx][0]
                ):
                    pending_idx += 1
                if pending_idx >= len(tasks):
                    break

                candidates = [h for h, avail in host_caps.items() if avail > 0]
                if not candidates:
                    break
                # Bias toward spreading tasks: choose host with fewest running tasks,
                # then with more remaining capacity.
                host = min(
                    candidates,
                    key=lambda h: (running.get(h, 0), -host_caps[h], h),
                )

                name, rel, warmup, sample = tasks[pending_idx]
                trace_path = find_trace(trace_root, rel)
                if not trace_path:
                    print(
                        f"[WARN] trace not found for {name}: {rel}",
                        file=sys.stderr,
                    )
                    task_status[name] = "missing_trace"
                    pending_idx += 1
                    continue
                tdir = work_root / name
                tdir.mkdir(parents=True, exist_ok=True)
                ok, pid = launch_task(
                    host,
                    arch_script,
                    tdir,
                    trace_path,
                    warmup,
                    sample,
                    trace_format,
                )
                if not ok:
                    task_status[name] = "launch_failed"
                    pending_idx += 1
                    continue
                task_status[name] = "running"
                task_host[name] = host
                task_paths[name] = trace_path
                start_ts[name] = time.time()
                warmup_map[name] = warmup
                sample_map[name] = sample
                if pid is not None:
                    running_pids[name] = pid
                running[host] = running.get(host, 0) + 1
                host_caps[host] = max(0, host_caps[host] - 1)
                global_avail -= 1
                pending_idx += 1
                made_progress = True
                done_cnt = sum(
                    1
                    for s in task_status.values()
                    if s
                    in ("completed", "abort", "launch_failed", "missing_trace")
                )
                print(
                    f"[DISPATCH] {name} -> {host} warmup={warmup} "
                    f"sample={sample} total={warmup+sample} "
                    f"running={running[host]} "
                    f"global_running={sum(running.values())} "
                    f"progress={done_cnt}/{total_tasks}"
                )
            if not made_progress:
                time.sleep(args.poll_seconds)
    except KeyboardInterrupt:
        print("[INFO] Caught Ctrl-C, aborting running tasks...")
        for name, pid in list(running_pids.items()):
            host = task_host.get(name)
            if not host:
                continue
            ssh(host, f"kill -TERM {pid} || true", timeout=5)
            tdir = work_root / name
            (tdir / "running").unlink(missing_ok=True)
            (tdir / "abort").touch()
            task_status[name] = "abort"
            running[host] = max(0, running.get(host, 1) - 1)
            print(f"[ABORT] killed {name} on {host} (pid={pid})")
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
