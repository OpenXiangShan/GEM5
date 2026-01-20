#!/usr/bin/env python3
"""
Single-workload rerun helper for aborted trace-driven runs with a debug window.

Given a workload directory that contains log.txt and an abort marker, extract
the suggested debug window and trace path, then rerun locally under the same
debug conventions as the distributed helper:
  - Outputs go to <work-dir>/debug/
  - Marks running/completed/abort/exit_code files
  - Sets XS_DEBUG_START/END, XS_DEBUG_FLAGS, XS_MAX_INSTS, TRACE_FORMAT

Example:
  python3 util/xs_scripts/trace/rerun_aborted_with_debug.py \\
    --work-dir /path/to/workload_dir \\
    --arch-script util/xs_scripts/trace/run_trace_champsim.sh \\
    --debug-flags IEW,Fetch,Commit,CommitTrace,DecoupleBP,TraceReader,Decode
"""

from __future__ import annotations

import argparse
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path
from typing import Dict, Optional


DEFAULT_DEBUG_FLAGS = "IEW,Fetch,Commit,CommitTrace,DecoupleBP,TraceReader,Decode"


def parse_abort_info(
    work_dir: Path, trace_format: str, require_abort: bool = True
) -> Optional[Dict[str, object]]:
    """Parse log/abort info to derive debug window and trace metadata."""
    log_file = work_dir / "log.txt"
    abort_file = work_dir / "abort"
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

    debug_dir = work_dir / "debug"

    return {
        "abort_tick": abort_tick,
        "start_tick": start_tick,
        "end_tick": end_tick,
        "trace_path": trace_path,
        "maxinsts": maxinsts,
        "debug_dir": debug_dir,
        "trace_format": trace_format,
    }


def touch(path: Path) -> None:
    path.touch(exist_ok=True)


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Rerun a single aborted trace workload locally with debug flags."
    )
    ap.add_argument("--work-dir", required=True, help="Workload directory containing log.txt and abort")
    ap.add_argument(
        "--arch-script",
        default="util/xs_scripts/trace/run_trace_champsim.sh",
        help="Path to trace arch script",
    )
    ap.add_argument(
        "--debug-flags",
        default=DEFAULT_DEBUG_FLAGS,
        help="Comma-separated debug flags",
    )
    ap.add_argument(
        "--trace-format",
        default=None,
        help="Trace format (champsim/cbp2025); defaults to env TRACE_FORMAT or champsim",
    )
    ap.add_argument(
        "--clear-old",
        action="store_true",
        help="Clear existing debug markers before rerun",
    )
    ap.add_argument(
        "--allow-missing-abort",
        action="store_true",
        help="Proceed even if abort marker is missing (useful after manual cleanup)",
    )
    args = ap.parse_args()

    work_dir = Path(args.work_dir).resolve()
    arch_script = Path(args.arch_script).resolve()

    if not work_dir.is_dir():
        print(f"work_dir is not a directory: {work_dir}", file=sys.stderr)
        return 1
    if not arch_script.exists():
        print(f"arch_script not found: {arch_script}", file=sys.stderr)
        return 1

    trace_format = args.trace_format or os.environ.get("TRACE_FORMAT", "champsim")

    info = parse_abort_info(
        work_dir, trace_format, require_abort=not args.allow_missing_abort
    )
    if info is None:
        print("failed to parse abort/log info; ensure log.txt and abort exist", file=sys.stderr)
        return 1

    debug_dir: Path = info["debug_dir"]  # type: ignore
    debug_dir.mkdir(parents=True, exist_ok=True)

    # Clean markers if requested
    if args.clear_old:
        for marker in ("running", "completed", "abort", "exit_code"):
            (debug_dir / marker).unlink(missing_ok=True)

    # Prepare status files
    touch(debug_dir / "running")

    env = os.environ.copy()
    env["XS_MAX_INSTS"] = str(info["maxinsts"])  # type: ignore
    env["XS_DEBUG_FLAGS"] = args.debug_flags
    env["XS_DEBUG_START"] = str(info["start_tick"])  # type: ignore
    env["XS_DEBUG_END"] = str(info["end_tick"])  # type: ignore
    env["TRACE_FORMAT"] = str(info["trace_format"])  # type: ignore

    cmd = [
        "bash",
        "-lc",
        f"OUTDIR=$PWD bash {shlex.quote(str(arch_script))} {shlex.quote(str(info['trace_path']))}",
    ]

    log_file = debug_dir / "log.txt"
    try:
        with open(log_file, "w", encoding="utf-8") as lf:
            proc = subprocess.run(
                cmd,
                cwd=debug_dir,
                env=env,
                stdout=lf,
                stderr=subprocess.STDOUT,
                text=True,
            )
            (debug_dir / "running").unlink(missing_ok=True)
            if proc.returncode == 0:
                touch(debug_dir / "completed")
            else:
                touch(debug_dir / "abort")
                (debug_dir / "exit_code").write_text(str(proc.returncode), encoding="utf-8")
                return proc.returncode or 1
    except KeyboardInterrupt:
        print("Interrupted, marking abort.", file=sys.stderr)
        touch(debug_dir / "abort")
        (debug_dir / "running").unlink(missing_ok=True)
        return 130
    except Exception as exc:  # pragma: no cover - best-effort guard
        print(f"Failed to rerun: {exc}", file=sys.stderr)
        touch(debug_dir / "abort")
        (debug_dir / "running").unlink(missing_ok=True)
        return 1

    return 0


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main())
