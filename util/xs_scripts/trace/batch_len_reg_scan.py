#!/usr/bin/env python3
"""
Batch length-vs-reg analyzer over ChampSim/CBP traces.

Scans two trace roots (champsim / cbp) and runs
`util/trace/dump_champsim_trace.py --analyze-len-reg`
for each trace, writing per-trace reports to an output dir.

Usage:
  python3 util/xs_scripts/trace/batch_len_reg_scan.py \
      --champsim-root /nfs/home/share/glr/champsim_traces \
      --cbp-root /nfs/home/share/glr/cbp_traces \
      --out-dir ./len_reg_reports \
      --max-procs 4
"""

from __future__ import annotations

import argparse
import os
import subprocess
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from typing import Iterable, List, Tuple


def list_traces(root: Path) -> List[Path]:
    pats = ("*.xz", "*.gz", "*")
    found: set[Path] = set()
    for pat in pats:
        found.update(root.rglob(pat))
    # filter out directories and remove duplicates caused by overlapping globs
    return [p for p in found if p.is_file()]


def run_analyzer(trace: Path, fmt: str, out_dir: Path) -> Tuple[Path, int, str]:
    out_file = out_dir / f"{trace.name}.len_reg.txt"
    cmd = [
        "python3",
        str(Path(__file__).resolve().parent.parent.parent / "trace" / "dump_champsim_trace.py"),
        "--trace",
        str(trace),
        "--format",
        fmt,
        "--analyze-len-reg",
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    out_file.parent.mkdir(parents=True, exist_ok=True)
    out_file.write_text(proc.stdout + ("\n[stderr]\n" + proc.stderr if proc.stderr else ""))
    return trace, proc.returncode, out_file.name


def main() -> int:
    ap = argparse.ArgumentParser(description="Batch scan traces for len-reg anomalies")
    ap.add_argument("--champsim-root", type=Path, required=True, help="Root dir for ChampSim traces")
    ap.add_argument("--cbp-root", type=Path, required=True, help="Root dir for CBP2025 traces")
    ap.add_argument("--out-dir", type=Path, required=True, help="Output directory for reports")
    ap.add_argument("--max-procs", type=int, default=2, help="Max concurrent analyzer processes")
    args = ap.parse_args()

    champsim_traces = list_traces(args.champsim_root)
    cbp_traces = list_traces(args.cbp_root)

    tasks: List[Tuple[Path, str]] = []
    tasks += [(p, "champsim") for p in champsim_traces]
    tasks += [(p, "cbp2025") for p in cbp_traces]

    if not tasks:
        print("No traces found")
        return 0

    args.out_dir.mkdir(parents=True, exist_ok=True)

    with ThreadPoolExecutor(max_workers=args.max_procs) as ex:
        futs = {ex.submit(run_analyzer, t, fmt, args.out_dir): (t, fmt) for t, fmt in tasks}
        for fut in as_completed(futs):
            trace, fmt = futs[fut]
            try:
                _, rc, out_name = fut.result()
                status = "OK" if rc == 0 else f"RC={rc}"
                print(f"[{status}] {fmt} {trace} -> {out_name}")
            except Exception as e:
                print(f"[ERR] {fmt} {trace}: {e}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
