#!/usr/bin/env python3
import argparse
import bisect
import sqlite3
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple


def parse_pc(text: str) -> int:
    text = text.strip().lower()
    if text.startswith("0x"):
        return int(text, 16)
    return int(text, 16)


def fmt_pc(value: int) -> str:
    return f"0x{value:x}"


@dataclass(frozen=True)
class Event:
    idx: int
    tick: int
    branch_pc: int
    start_pc: int
    actual_taken: int
    alloc_success: int
    alloc_table: int
    alloc_index: int
    alloc_tag: int
    alloc_way: int
    alloc_fh: int
    main_found: int
    main_table: int
    main_index: int
    main_tag: int
    main_entry_pc: int
    alt_found: int
    alt_table: int
    alt_index: int
    alt_tag: int
    alt_entry_pc: int
    index_fh: int


def load_events(bpdb: Path, branch_pcs: Iterable[int]) -> List[Event]:
    conn = sqlite3.connect(bpdb)
    conn.row_factory = sqlite3.Row
    pcs = list(branch_pcs)
    placeholders = ",".join("?" for _ in pcs)
    rows = conn.execute(
        f"""
        select
          ID,
          TICK,
          branchPC,
          startPC,
          actualTaken,
          allocSuccess,
          allocTable,
          allocIndex,
          allocTag,
          allocWay,
          allocIndexFoldedHist,
          mainFound,
          mainTable,
          mainIndex,
          mainTag,
          mainEntryPC,
          altFound,
          altTable,
          altIndex,
          altTag,
          altEntryPC,
          indexFoldedHist
        from TAGEMISSTRACE
        where branchPC in ({placeholders})
        order by TICK, ID
        """,
        pcs,
    ).fetchall()
    return [
        Event(
            idx=i,
            tick=row["TICK"],
            branch_pc=row["branchPC"],
            start_pc=row["startPC"],
            actual_taken=row["actualTaken"],
            alloc_success=row["allocSuccess"],
            alloc_table=row["allocTable"],
            alloc_index=row["allocIndex"],
            alloc_tag=row["allocTag"],
            alloc_way=row["allocWay"],
            alloc_fh=row["allocIndexFoldedHist"],
            main_found=row["mainFound"],
            main_table=row["mainTable"],
            main_index=row["mainIndex"],
            main_tag=row["mainTag"],
            main_entry_pc=row["mainEntryPC"],
            alt_found=row["altFound"],
            alt_table=row["altTable"],
            alt_index=row["altIndex"],
            alt_tag=row["altTag"],
            alt_entry_pc=row["altEntryPC"],
            index_fh=row["indexFoldedHist"],
        )
        for i, row in enumerate(rows)
    ]


def find_next_slot_replace(
    event: Event,
    events: List[Event],
    slot_map: Dict[Tuple[int, int, int], List[int]],
) -> Optional[Event]:
    slot = (event.alloc_table, event.alloc_index, event.alloc_way)
    indices = slot_map.get(slot, [])
    pos = bisect.bisect_right(indices, event.idx)
    while pos < len(indices):
        later = events[indices[pos]]
        pos += 1
        if not later.alloc_success:
            continue
        if later.alloc_tag != event.alloc_tag:
            return later
    return None


def classify_next_context_event(alloc: Event, later: Event) -> str:
    main_exact = (
        later.main_found
        and later.main_table == alloc.alloc_table
        and later.main_index == alloc.alloc_index
        and later.main_tag == alloc.alloc_tag
    )
    alt_exact = (
        later.alt_found
        and later.alt_table == alloc.alloc_table
        and later.alt_index == alloc.alloc_index
        and later.alt_tag == alloc.alloc_tag
    )
    if main_exact:
        return "next_main_exact"
    if alt_exact:
        return "next_alt_exact"
    if later.main_found and later.main_table == alloc.alloc_table:
        if later.main_index == alloc.alloc_index:
            return "next_main_same_index_diff_tag"
        if later.index_fh == alloc.alloc_fh:
            return "next_main_diff_index_same_fh"
        return "next_main_diff_index_diff_fh"
    if later.alt_found and later.alt_table == alloc.alloc_table:
        if later.alt_index == alloc.alloc_index:
            return "next_alt_same_index_diff_tag"
        if later.index_fh == alloc.alloc_fh:
            return "next_alt_diff_index_same_fh"
        return "next_alt_diff_index_diff_fh"
    return "next_other"


def analyze_context(
    events: List[Event],
    context_indices: List[int],
    slot_map: Dict[Tuple[int, int, int], List[int]],
    alloc_table_min: int,
) -> Dict[str, object]:
    counters: Counter[str] = Counter()
    sample_examples: Dict[str, List[str]] = defaultdict(list)
    top_next_main_keys: Counter[Tuple[int, int, int]] = Counter()
    top_next_alt_keys: Counter[Tuple[int, int, int]] = Counter()

    for pos, idx in enumerate(context_indices):
        event = events[idx]
        if not event.alloc_success or event.alloc_table < alloc_table_min:
            continue

        counters["high_alloc"] += 1
        next_ctx = events[context_indices[pos + 1]] if pos + 1 < len(context_indices) else None
        if next_ctx is None:
            counters["no_next_context"] += 1
        else:
            next_cls = classify_next_context_event(event, next_ctx)
            counters[next_cls] += 1
            if next_cls == "next_main_diff_index_diff_fh":
                top_next_main_keys[
                    (next_ctx.main_table, next_ctx.main_index, next_ctx.main_tag)
                ] += 1
                if next_ctx.main_entry_pc == event.branch_pc:
                    counters["next_main_diff_index_diff_fh_self_entry"] += 1
                else:
                    counters["next_main_diff_index_diff_fh_foreign_entry"] += 1
            elif next_cls == "next_alt_diff_index_diff_fh":
                top_next_alt_keys[
                    (next_ctx.alt_table, next_ctx.alt_index, next_ctx.alt_tag)
                ] += 1
                if next_ctx.alt_entry_pc == event.branch_pc:
                    counters["next_alt_diff_index_diff_fh_self_entry"] += 1
                else:
                    counters["next_alt_diff_index_diff_fh_foreign_entry"] += 1
            if len(sample_examples[next_cls]) < 3:
                sample_examples[next_cls].append(
                    f"alloc@{event.tick} a=({event.alloc_table},{event.alloc_index},{event.alloc_tag}) "
                    f"next@{next_ctx.tick} main=({next_ctx.main_table},{next_ctx.main_index},{next_ctx.main_tag},pc={fmt_pc(next_ctx.main_entry_pc)}) "
                    f"alt=({next_ctx.alt_table},{next_ctx.alt_index},{next_ctx.alt_tag},pc={fmt_pc(next_ctx.alt_entry_pc)}) fh={next_ctx.index_fh}"
                )

        replace = find_next_slot_replace(event, events, slot_map)

        provider_hit = None
        alt_hit = None
        first_same_table_main = None
        first_same_table_alt = None

        for later_idx in context_indices[pos + 1 :]:
            later = events[later_idx]
            if provider_hit is None and (
                later.main_found
                and later.main_table == event.alloc_table
                and later.main_index == event.alloc_index
                and later.main_tag == event.alloc_tag
            ):
                provider_hit = later
            if alt_hit is None and (
                later.alt_found
                and later.alt_table == event.alloc_table
                and later.alt_index == event.alloc_index
                and later.alt_tag == event.alloc_tag
            ):
                alt_hit = later
            if first_same_table_main is None and later.main_found and later.main_table == event.alloc_table:
                first_same_table_main = later
            if first_same_table_alt is None and later.alt_found and later.alt_table == event.alloc_table:
                first_same_table_alt = later
            if provider_hit and alt_hit and first_same_table_main and first_same_table_alt:
                break

        first_exact = min(
            [cand for cand in (provider_hit, alt_hit) if cand is not None],
            key=lambda item: item.idx,
            default=None,
        )
        if provider_hit is not None:
            counters["provider_exact_any"] += 1
        if alt_hit is not None:
            counters["alt_exact_any"] += 1

        if replace is None and first_exact is None:
            counters["neither_replace_nor_exact"] += 1
        elif replace is None and first_exact is not None:
            if first_exact is provider_hit:
                counters["provider_before_replace"] += 1
            else:
                counters["alt_before_replace"] += 1
        elif replace is not None and first_exact is None:
            counters["replace_before_any_exact"] += 1
        elif first_exact.idx < replace.idx:
            if first_exact is provider_hit:
                counters["provider_before_replace"] += 1
            else:
                counters["alt_before_replace"] += 1
        else:
            counters["replace_before_any_exact"] += 1

        if first_same_table_main is None:
            counters["no_same_table_main"] += 1
        else:
            if first_same_table_main.main_index == event.alloc_index:
                if first_same_table_main.main_tag == event.alloc_tag:
                    counters["same_table_main_exact"] += 1
                else:
                    counters["same_table_main_same_index_diff_tag"] += 1
            else:
                if first_same_table_main.index_fh == event.alloc_fh:
                    counters["same_table_main_diff_index_same_fh"] += 1
                else:
                    counters["same_table_main_diff_index_diff_fh"] += 1

        if first_same_table_alt is None:
            counters["no_same_table_alt"] += 1
        else:
            if first_same_table_alt.alt_index == event.alloc_index:
                if first_same_table_alt.alt_tag == event.alloc_tag:
                    counters["same_table_alt_exact"] += 1
                else:
                    counters["same_table_alt_same_index_diff_tag"] += 1
            else:
                if first_same_table_alt.index_fh == event.alloc_fh:
                    counters["same_table_alt_diff_index_same_fh"] += 1
                else:
                    counters["same_table_alt_diff_index_diff_fh"] += 1

    return {
        "counts": counters,
        "examples": sample_examples,
        "top_next_main_keys": top_next_main_keys,
        "top_next_alt_keys": top_next_alt_keys,
    }


def pct(num: int, den: int) -> float:
    if den == 0:
        return 0.0
    return 100.0 * num / den


def print_summary(branch_pc: int, start_pc: int, taken: int, result: Dict[str, object]) -> None:
    counts: Counter[str] = result["counts"]  # type: ignore[assignment]
    examples: Dict[str, List[str]] = result["examples"]  # type: ignore[assignment]
    top_next_main_keys: Counter[Tuple[int, int, int]] = result["top_next_main_keys"]  # type: ignore[assignment]
    top_next_alt_keys: Counter[Tuple[int, int, int]] = result["top_next_alt_keys"]  # type: ignore[assignment]
    total = counts["high_alloc"]
    print(
        f"[context] branch={fmt_pc(branch_pc)} start={fmt_pc(start_pc)} taken={taken} high_alloc={total}"
    )
    if total == 0:
        print()
        return
    for key in (
        "provider_before_replace",
        "alt_before_replace",
        "replace_before_any_exact",
        "neither_replace_nor_exact",
        "provider_exact_any",
        "alt_exact_any",
        "same_table_main_exact",
        "same_table_main_same_index_diff_tag",
        "same_table_main_diff_index_same_fh",
        "same_table_main_diff_index_diff_fh",
        "same_table_alt_exact",
        "same_table_alt_same_index_diff_tag",
        "same_table_alt_diff_index_same_fh",
        "same_table_alt_diff_index_diff_fh",
        "next_main_exact",
        "next_alt_exact",
        "next_main_same_index_diff_tag",
        "next_main_diff_index_same_fh",
        "next_main_diff_index_diff_fh",
        "next_main_diff_index_diff_fh_self_entry",
        "next_main_diff_index_diff_fh_foreign_entry",
        "next_alt_same_index_diff_tag",
        "next_alt_diff_index_same_fh",
        "next_alt_diff_index_diff_fh",
        "next_alt_diff_index_diff_fh_self_entry",
        "next_alt_diff_index_diff_fh_foreign_entry",
        "next_other",
    ):
        value = counts[key]
        if value:
            print(f"  {key}: {value} ({pct(value, total):.2f}%)")
    for key in (
        "next_alt_exact",
        "next_main_diff_index_diff_fh",
        "next_alt_diff_index_diff_fh",
        "next_other",
    ):
        if examples.get(key):
            print(f"  examples[{key}]")
            for item in examples[key]:
                print(f"    {item}")
    if top_next_main_keys:
        print("  top_next_main_diff_index_diff_fh_keys")
        for (table, index, tag), value in top_next_main_keys.most_common(5):
            print(f"    ({table},{index},{tag}) -> {value}")
    if top_next_alt_keys:
        print("  top_next_alt_diff_index_diff_fh_keys")
        for (table, index, tag), value in top_next_alt_keys.most_common(5):
            print(f"    ({table},{index},{tag}) -> {value}")
    print()


def main() -> None:
    parser = argparse.ArgumentParser(description="Analyze TAGE high-table allocation lifecycle.")
    parser.add_argument("--bpdb", required=True)
    parser.add_argument("--branch-pc", action="append", required=True, help="Hex branch PC")
    parser.add_argument("--alloc-table-min", type=int, default=2)
    args = parser.parse_args()

    branch_pcs = [parse_pc(text) for text in args.branch_pc]
    events = load_events(Path(args.bpdb), branch_pcs)

    context_map: Dict[Tuple[int, int, int], List[int]] = defaultdict(list)
    slot_map: Dict[Tuple[int, int, int], List[int]] = defaultdict(list)
    for event in events:
        context_map[(event.branch_pc, event.start_pc, event.actual_taken)].append(event.idx)
        if event.alloc_success:
            slot_map[(event.alloc_table, event.alloc_index, event.alloc_way)].append(event.idx)

    for branch_pc in branch_pcs:
        branch_contexts = sorted(
            (ctx for ctx in context_map if ctx[0] == branch_pc),
            key=lambda item: (item[1], item[2]),
        )
        for _, start_pc, taken in branch_contexts:
            result = analyze_context(
                events,
                context_map[(branch_pc, start_pc, taken)],
                slot_map,
                args.alloc_table_min,
            )
            print_summary(branch_pc, start_pc, taken, result)


if __name__ == "__main__":
    main()
