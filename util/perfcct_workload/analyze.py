#!/usr/bin/env python3
"""Emit bounded, read-only PerfCCT evidence for labeled microbenchmarks."""

import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import sqlite3
import subprocess


WAKE = {
    "stlf": {"stlf"},
    "translation": {"translation_observed_complete"},
    "cache_admission": {"cache_retry"},
    "cache_refill": {"cache_hint", "cache_response"},
}


def symbols(elf):
    output = subprocess.check_output(
        ["riscv64-linux-gnu-nm", "-n", str(elf)], text=True
    )
    return {
        fields[2]: int(fields[0], 16)
        for line in output.splitlines()
        if len(fields := line.split()) == 3
        and (
            fields[2].startswith("probe_")
            or fields[2] in ("roi_begin", "roi_end")
        )
    }


def union_ticks(intervals):
    total, end = 0, None
    for left, right in sorted(intervals):
        total += max(0, right - max(left, end if end is not None else left))
        end = max(right, end if end is not None else right)
    return total


def analyze(db_path, elf_path):
    pcs = symbols(elf_path)
    with sqlite3.connect(
        Path(db_path).resolve().as_uri() + "?mode=ro", uri=True
    ) as db:
        db.row_factory = sqlite3.Row
        meta = dict(db.execute("SELECT Key, Value FROM PerfCCTMeta"))
        markers = []
        for label in ("roi_begin", "roi_end"):
            if label not in pcs:
                raise ValueError(f"ELF has no {label} symbol")
            found = list(
                db.execute(
                    "SELECT * FROM PerfCCTInst WHERE PC=? AND EndKind='commit'",
                    (pcs[label],),
                )
            )
            if len(found) != 1:
                raise ValueError(
                    f"{label}: expected one committed marker, got {len(found)}"
                )
            markers.append(found[0])
        begin, finish = markers
        cpu, tid = begin["Cpu"], begin["TID"]
        start, end = begin["EndTick"], finish["EndTick"]
        lo, hi = begin["SeqNum"], finish["SeqNum"]
        if (
            (cpu, tid) != (finish["Cpu"], finish["TID"])
            or start >= end
            or lo >= hi
        ):
            raise ValueError(
                "ROI markers must have ordered ticks/sequences on one CPU/thread"
            )
        period = int(meta[f"cpu.{cpu}.clock_period"])
        if period <= 0:
            raise ValueError("CPU clock period must be positive")
        insts = {
            row["SeqNum"]: dict(row)
            for row in db.execute(
                "SELECT * FROM PerfCCTInst WHERE Cpu=? AND TID=?", (cpu, tid)
            )
        }
        events = [
            dict(row)
            for row in db.execute(
                "SELECT * FROM PerfCCTEvent WHERE Cpu=? AND TID=? "
                "AND SeqNum>=? AND SeqNum<? AND Tick>=? AND Tick<? ORDER BY Tick,ID",
                (cpu, tid, lo, hi, start, end),
            )
            if row["SeqNum"] in insts
        ]
        spans = [
            dict(row)
            for row in db.execute(
                "SELECT * FROM PerfCCTCommitSpan WHERE Cpu=? AND TID=? "
                "AND StartTick<? AND (EndTick>? OR EndTick IS NULL)",
                (cpu, tid, end, start),
            )
        ]
    totals, top = defaultdict(Counter), defaultdict(Counter)
    blocker_intervals = defaultdict(list)
    nonhead = Counter()
    incomplete = 0
    for span in spans:
        if span["EndKind"] != "closed":
            incomplete += 1
        if span["EndTick"] is None:
            continue
        left, right = max(start, span["StartTick"]), min(end, span["EndTick"])
        ticks = max(0, right - left)
        kind = "zero_commit" if span["Committed"] == 0 else "positive_commit"
        totals[span["Reason"]][kind + "_cycles"] += ticks / period
        totals[span["Reason"]]["spans"] += 1
        if span["Reason"] not in ("group_not_ready", "commit_head_blocked"):
            continue
        seq = span["BlockerSeq"]
        blocker_intervals[seq].append((left, right))
        inst = insts.get(seq, {})
        key = (span["Reason"], inst.get("PC"), inst.get("Disasm"))
        top[key][kind + "_cycles"] += ticks / period
        top[key]["spans"] += 1
        if span["Reason"] == "group_not_ready" and seq != span["HeadSeq"]:
            nonhead["spans"] += 1
            nonhead[kind + "_cycles"] += ticks / period
    probe_events = defaultdict(Counter)
    reverse_pcs = {
        pc: label for label, pc in pcs.items() if label.startswith("probe_")
    }
    by_attempt = defaultdict(list)
    for event in events:
        by_attempt[(event["SeqNum"], event["Attempt"])].append(event)
        label = reverse_pcs.get(insts[event["SeqNum"]]["PC"])
        if label:
            probe_events[label][event["Event"] + ":" + event["Detail"]] += 1
    waits, summaries = [], defaultdict(Counter)
    for (seq, attempt), group in by_attempt.items():
        pending = {}
        for event in group:
            detail = event["Detail"]
            if event["Event"] == "wait_begin":
                wait = {
                    "seq": seq,
                    "attempt": attempt,
                    "reason": detail,
                    "related_seq": event["RelatedSeq"],
                    "start_tick": event["Tick"],
                    "end_tick": None,
                    "observed_cycles": None,
                    "own_blocker_overlap_cycles": None,
                    "status": "unknown",
                }
                waits.append(wait)
                pending[detail] = wait
            elif event["Event"] == "wake":
                for reason in list(pending):
                    if detail not in WAKE.get(reason, set()):
                        continue
                    wait = pending[reason]
                    if (
                        reason == "stlf"
                        and wait["related_seq"]
                        and event["RelatedSeq"] != wait["related_seq"]
                    ):
                        continue
                    pending.pop(reason)
                    left, right = wait["start_tick"], event["Tick"]
                    wait.update(
                        end_tick=right,
                        observed_cycles=(right - left) / period,
                        wake_detail=detail,
                        status="observed",
                        own_blocker_overlap_cycles=union_ticks(
                            [
                                (max(left, a), min(right, b))
                                for a, b in blocker_intervals[seq]
                                if a < right and b > left
                            ]
                        )
                        / period,
                    )
    observed_intervals = defaultdict(list)
    for wait in waits:
        stats = summaries[wait["reason"]]
        stats[wait["status"] + "_intervals"] += 1
        if wait["status"] == "observed":
            observed_intervals[wait["reason"]].append(
                (wait["start_tick"], wait["end_tick"])
            )
            stats["observed_cycles"] += wait["observed_cycles"]
            stats["own_blocker_overlap_cycles"] += wait[
                "own_blocker_overlap_cycles"
            ]
    for reason, stats in summaries.items():
        stats["observed_union_cycles"] = (
            union_ticks(observed_intervals[reason]) / period
        )
    ranked = [
        dict(reason=key[0], pc=key[1], disasm=key[2], **value)
        for key, value in top.items()
    ]
    ranked.sort(key=lambda row: -row.get("zero_commit_cycles", 0))
    return {
        "roi": {
            "cpu": cpu,
            "tid": tid,
            "start_tick": start,
            "end_tick": end,
            "clock_period": period,
            "marker_commit_to_commit_cycles": (end - start) / period,
            "seq_begin_inclusive": lo,
            "seq_end_exclusive": hi,
            "committed_operations": sum(
                inst["EndKind"] == "commit" and lo <= seq < hi
                for seq, inst in insts.items()
            ),
        },
        "symbols": pcs,
        "commit_by_reason": dict(totals),
        "group_nonhead": dict(nonhead),
        "top_blockers": ranked[:20],
        "incomplete_spans": incomplete,
        "probe_events": dict(probe_events),
        "wait_summary": dict(summaries),
        "stlf_examples": [wait for wait in waits if wait["reason"] == "stlf"][
            :8
        ],
        "wait_examples": waits[:8],
        "notes": [
            "ROI is marker commit-to-commit [start,end); events also require dynamic seq in [begin,end).",
            "Committed operations use the same dynamic seq range, including begin and excluding end.",
            "Events outside the tick window cannot close waits; missing or mismatched wakes stay unknown.",
            "Cache hint/response pairing records an observation, not necessarily data completion.",
            "observed_cycles sums instruction-cycles; observed_union_cycles counts the union per wait reason.",
            "Zero/positive commit observations and summed waits are not recoverable cycles or CPI components.",
            "Own-blocker overlap covers group_not_ready/commit_head_blocked spans, including partial commits.",
            "Incomplete spans have observed ends only; spans with unknown ends contribute no duration.",
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("db")
    parser.add_argument("elf")
    args = parser.parse_args()
    print(json.dumps(analyze(args.db, args.elf), indent=2))


if __name__ == "__main__":
    main()
