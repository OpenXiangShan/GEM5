#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
Extract key events from gem5 debug log after a given tick (trace-focused helper).

Usage examples:
  - All events after a tick:
      python3 util/trace/extract_trace_events.py m5out_insts/debug.log --from-tick 480000
  - Only buildInst + commit:
      python3 util/trace/extract_trace_events.py m5out_insts/debug.log --from-tick 480000 --no-squash
  - Custom regex:
      python3 util/trace/extract_trace_events.py m5out_insts/debug.log --from-tick 480000 \\
          --build-pattern 'Instruction PC .* created'
"""

import argparse
import re
import sys
from typing import Dict, List, Tuple


def compile_pat(pat: str):
    return re.compile(pat)


def parse_args():
    p = argparse.ArgumentParser(
        description="Extract key events from gem5 debug log after a given tick."
    )
    p.add_argument("log", help="Path to gem5 debug log (e.g. m5out_insts/debug.log)")
    p.add_argument("--from-tick", type=int, required=True, help="Start tick (inclusive)")
    # Toggles
    p.add_argument("--build", action="store_true", default=True, help="Include buildInst events (default on)")
    p.add_argument("--no-build", action="store_false", dest="build")
    p.add_argument("--squash", action="store_true", default=True, help="Include squash events (default on)")
    p.add_argument("--no-squash", action="store_false", dest="squash")
    p.add_argument("--commit", action="store_true", default=True, help="Include commit events (default on)")
    p.add_argument("--no-commit", action="store_false", dest="commit")
    # Trace metadata bind events (for ChampSim trace debug)
    p.add_argument(
        "--bind", action="store_true", default=True,
        help="Include trace metadata bind events (default on)",
    )
    p.add_argument("--no-bind", action="store_false", dest="bind")
    # Patterns (overridable)
    p.add_argument(
        "--build-pattern", default=r"Instruction PC.*created",
        help="Regex for buildInst",
    )
    p.add_argument(
        "--squash-pattern", default=r"Squashing, setting PC to",
        help="Regex for squash",
    )
    p.add_argument(
        "--commit-pattern", default=r"Committing instruction with PC",
        help="Regex for commit",
    )
    p.add_argument(
        "--bind-pattern", default=r"Bind trace metadata",
        help="Regex for trace metadata bind events",
    )
    # Wrong-path enter/exit events
    p.add_argument(
        "--wp-enter", action="store_true", default=True,
        help="Include wrong-path enter events (default on)",
    )
    p.add_argument("--no-wp-enter", action="store_false", dest="wp_enter")
    p.add_argument(
        "--wp-exit", action="store_true", default=True,
        help="Include wrong-path exit events (default on)",
    )
    p.add_argument("--no-wp-exit", action="store_false", dest="wp_exit")
    p.add_argument(
        "--wp-enter-pattern", default=r"Enter wrong-path mode",
        help="Regex for wrong-path enter",
    )
    p.add_argument(
        "--wp-exit-pattern", default=r"In wrong-path, detected squash from",
        help="Regex for wrong-path exit",
    )
    p.add_argument(
        "--limit", type=int, default=0,
        help="Max lines to print (0 = no limit)",
    )
    # Debug
    p.add_argument(
        "--debug", action="store_true",
        help="Print debug info to stderr (summary, counts)",
    )
    return p.parse_args()


def main():
    args = parse_args()
    pat_tick = re.compile(r"^\s*(\d+):\s*(.*)$")

    pats: List[Tuple[str, re.Pattern]] = []
    if args.build:
        pats.append(("BUILD", compile_pat(args.build_pattern)))
    if args.squash:
        pats.append(("SQUASH", compile_pat(args.squash_pattern)))
    if args.commit:
        pats.append(("COMMIT", compile_pat(args.commit_pattern)))
    if args.bind:
        pats.append(("BIND", compile_pat(args.bind_pattern)))
    if args.wp_enter:
        pats.append(("WP_ENTER", compile_pat(args.wp_enter_pattern)))
    if args.wp_exit:
        pats.append(("WP_EXIT", compile_pat(args.wp_exit_pattern)))

    results: List[Tuple[int, str, str]] = []

    # Debug counters
    total_lines = 0
    tick_lines = 0
    after_tick_lines = 0
    first_tick = None
    last_tick = None
    event_counts: Dict[str, int] = {lbl: 0 for lbl, _ in pats}

    with open(args.log, "r", errors="ignore") as f:
        for line in f:
            total_lines += 1
            m = pat_tick.match(line)
            if not m:
                continue
            tick_lines += 1
            tick = int(m.group(1))
            if first_tick is None:
                first_tick = tick
            last_tick = tick
            if tick < args.from_tick:
                continue
            after_tick_lines += 1
            rest = m.group(2)
            for label, pat in pats:
                if pat.search(rest):
                    results.append((tick, label, line.rstrip("\n")))
                    event_counts[label] = event_counts.get(label, 0) + 1
                    break  # one label per line

    results.sort(key=lambda x: x[0])
    count = 0
    for tick, label, raw in results:
        print(f"{tick}\t[{label}] {raw}")
        count += 1
        if args.limit and count >= args.limit:
            break

    if args.debug:
        def eprint(*a, **k):
            print(*a, file=sys.stderr, **k)

        eprint("[extract_trace_events] Debug summary")
        eprint(f"  file          : {args.log}")
        eprint(f"  from_tick     : {args.from_tick}")
        eprint("  toggles       : "
                f"build={args.build}, squash={args.squash}, "
                f"commit={args.commit}, bind={args.bind}, "
                f"wp_enter={args.wp_enter}, wp_exit={args.wp_exit}")
        eprint("  patterns      :")
        eprint(f"    BUILD   : {args.build_pattern}")
        eprint(f"    SQUASH  : {args.squash_pattern}")
        eprint(f"    COMMIT  : {args.commit_pattern}")
        eprint(f"    BIND    : {args.bind_pattern}")
        eprint(f"    WP_ENTER: {args.wp_enter_pattern}")
        eprint(f"    WP_EXIT : {args.wp_exit_pattern}")
        eprint("  counts        :")
        eprint(f"    total_lines       = {total_lines}")
        eprint(f"    tick_lines        = {tick_lines}")
        eprint(f"    after_tick_lines  = {after_tick_lines}")
        eprint(f"    matches           = {len(results)}")
        eprint(f"    first_tick_seen   = {first_tick}")
        eprint(f"    last_tick_seen    = {last_tick}")
        if event_counts:
            eprint("    event_counts      :")
            for k in sorted(event_counts.keys()):
                eprint(f"      {k:8s} = {event_counts[k]}")
        if len(results) == 0:
            hint = []
            if tick_lines == 0:
                hint.append("no lines start with '<tick>:'")
            if last_tick is not None and last_tick < args.from_tick:
                hint.append("from_tick too large for this log window")
            if hint:
                eprint("  hints         : " + "; ".join(hint))


if __name__ == "__main__":
    main()
