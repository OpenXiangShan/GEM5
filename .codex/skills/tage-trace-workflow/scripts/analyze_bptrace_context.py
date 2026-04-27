#!/usr/bin/env python3
import argparse
import bisect
import math
import sqlite3
from collections import Counter, defaultdict
from pathlib import Path


def parse_pc(text: str) -> int:
    text = text.strip().lower()
    if text.startswith("0x"):
        return int(text, 16)
    return int(text, 16)


def suffix_bucket(bitstr: str, width: int) -> str:
    if not bitstr:
        return ""
    if width <= 0 or len(bitstr) <= width:
        return bitstr
    return bitstr[-width:]


def entropy(counter: Counter) -> float:
    total = sum(counter.values())
    if total == 0:
        return 0.0
    return -sum((c / total) * math.log2(c / total) for c in counter.values())


def top_share(counter: Counter, topn: int) -> float:
    total = sum(counter.values())
    if total == 0:
        return 0.0
    return sum(c for _, c in counter.most_common(topn)) / total


def mispred_bin(distance: int | None) -> str:
    if distance is None:
        return "none"
    if distance <= 1:
        return "0-1"
    if distance <= 4:
        return "2-4"
    if distance <= 8:
        return "5-8"
    if distance <= 16:
        return "9-16"
    return "17+"


def recent_signature(rows, row_idx: int, k: int, include_target: bool, include_type: bool) -> str:
    start = max(0, row_idx - k)
    parts = []
    for row in rows[start:row_idx]:
        token = ""
        if include_type:
            token += f"{row['controlType']}@"
        token += f"{row['controlPC']:x}:{row['taken']}{'m' if row['mispred'] else ''}"
        if include_target and row["taken"]:
            token += f"->{row['target']:x}"
        parts.append(token)
    return "|".join(parts)


def build_phr_contributor_rows(bp_rows):
    fsq_rows = defaultdict(list)
    fsq_order = []
    seen = set()
    for row in bp_rows:
        fsq_id = row["fsqId"]
        fsq_rows[fsq_id].append(row)
        if fsq_id not in seen:
            seen.add(fsq_id)
            fsq_order.append(fsq_id)

    contributors = []
    for fsq_id in fsq_order:
        chosen = None
        for row in fsq_rows[fsq_id]:
            is_cond = row["controlType"] == 0
            if is_cond:
                if row["taken"]:
                    chosen = row
                    break
            else:
                chosen = row
                break
        if chosen is not None:
            contributors.append(chosen)
    return contributors


def main() -> None:
    ap = argparse.ArgumentParser(description="Analyze BPTRACE context around TAGEMISSTRACE rows.")
    ap.add_argument("--bpdb", required=True)
    ap.add_argument("--branch-pc", required=True)
    ap.add_argument("--start-pc", required=True)
    ap.add_argument("--taken", type=int, required=True)
    ap.add_argument("--width", type=int, default=24)
    ap.add_argument("--sig-k", type=int, default=4)
    ap.add_argument("--min-sig-samples", type=int, default=12)
    ap.add_argument("--alloc-only", action="store_true")
    ap.add_argument("--include-target", action="store_true")
    ap.add_argument("--include-type", action="store_true")
    ap.add_argument(
        "--event-stream",
        choices=("control", "phr"),
        default="control",
        help="control: recent committed controls; phr: one PHR-contributor event per FSQ",
    )
    args = ap.parse_args()

    branch_pc = parse_pc(args.branch_pc)
    start_pc = parse_pc(args.start_pc)

    conn = sqlite3.connect(Path(args.bpdb))
    conn.row_factory = sqlite3.Row

    bp_rows = list(
        conn.execute(
            "select ID, TICK, fsqId, controlPC, controlType, startPC, taken, mispred, target "
            "from BPTRACE order by TICK, ID"
        )
    )
    bp_ticks = [row["TICK"] for row in bp_rows]
    phr_rows = build_phr_contributor_rows(bp_rows)
    phr_id_to_pos = {row["ID"]: idx for idx, row in enumerate(phr_rows)}
    phr_fsq_to_pos = {row["fsqId"]: idx for idx, row in enumerate(phr_rows)}

    where = "branchPC=? and startPC=? and actualTaken=? and mainFound!=0"
    if args.alloc_only:
        where += " and allocSuccess!=0 and allocTable>=2"
    tage_rows = list(
        conn.execute(
            f"""select ID, TICK, phistory, allocSuccess, allocTable, mainTable,
                       mainCounter, mainUseful, useAlt, predTaken, actualTaken
                from TAGEMISSTRACE
                where {where}
                order by TICK, ID""",
            (branch_pc, start_pc, args.taken),
        )
    )

    ctx_bp_indices = [
        idx
        for idx, row in enumerate(bp_rows)
        if row["controlPC"] == branch_pc and row["startPC"] == start_pc and row["taken"] == args.taken
    ]
    ctx_bp_ticks = [bp_rows[idx]["TICK"] for idx in ctx_bp_indices]

    matched = []
    used_ptr = 0
    for row in tage_rows:
        pos = bisect.bisect_right(ctx_bp_ticks, row["TICK"], lo=used_ptr) - 1
        if pos < used_ptr:
            continue
        bp_idx = ctx_bp_indices[pos]
        used_ptr = pos + 1
        matched.append((row, bp_idx))

    last_mispred_idx = []
    last = None
    stream_rows = phr_rows if args.event_stream == "phr" else bp_rows
    for row in stream_rows:
        last_mispred_idx.append(last)
        if row["mispred"]:
            last = row["ID"]

    id_to_pos = {row["ID"]: idx for idx, row in enumerate(stream_rows)}

    phr_ctr = Counter()
    bin_rows = defaultdict(list)
    sig_rows = defaultdict(list)

    for tage_row, bp_idx in matched:
        bp_row = bp_rows[bp_idx]
        bucket = suffix_bucket(tage_row["phistory"], args.width)
        phr_ctr[bucket] += 1

        if args.event_stream == "phr":
            stream_pos = phr_fsq_to_pos.get(bp_row["fsqId"])
            if stream_pos is None:
                continue
        else:
            stream_pos = bp_idx

        prev_mispred_id = last_mispred_idx[stream_pos]
        if prev_mispred_id is None:
            dist = None
        else:
            prev_idx = id_to_pos[prev_mispred_id]
            dist = stream_pos - prev_idx - 1
        bin_rows[mispred_bin(dist)].append(bucket)
        sig_rows_source = phr_rows if args.event_stream == "phr" else bp_rows
        sig = recent_signature(
            sig_rows_source,
            stream_pos,
            args.sig_k,
            args.include_target,
            args.include_type,
        )
        sig_rows[sig].append(
            {
                "bucket": bucket,
                "mainTable": int(tage_row["mainTable"]),
                "mainCounter": int(tage_row["mainCounter"]),
                "mainUseful": int(tage_row["mainUseful"]),
                "allocSuccess": int(tage_row["allocSuccess"]),
                "allocTable": int(tage_row["allocTable"]),
                "useAlt": int(tage_row["useAlt"]),
                "mispred": int(tage_row["predTaken"]) != int(tage_row["actualTaken"]),
            }
        )

    print(
        f"[match] tage_rows={len(tage_rows)} matched={len(matched)} bp_ctx_rows={len(ctx_bp_indices)} "
        f"alloc_only={args.alloc_only} width={args.width} sig_k={args.sig_k} "
        f"include_target={args.include_target} include_type={args.include_type} "
        f"event_stream={args.event_stream}"
    )
    print(
        f"[overall] uniq_phr={len(phr_ctr)} top1={top_share(phr_ctr,1)*100:.2f}% "
        f"top3={top_share(phr_ctr,3)*100:.2f}% entropy={entropy(phr_ctr):.2f}"
    )

    print("[distance_to_prev_mispred]")
    for label in ["0-1", "2-4", "5-8", "9-16", "17+", "none"]:
        vals = bin_rows.get(label, [])
        if not vals:
            continue
        ctr = Counter(vals)
        total = len(vals)
        print(
            f"{label}\trows={total}\tshare={total*100/len(matched):.2f}%\t"
            f"uniq={len(ctr)}\ttop1={top_share(ctr,1)*100:.2f}%\t"
            f"top3={top_share(ctr,3)*100:.2f}%\tentropy={entropy(ctr):.2f}"
        )

    print("[top_recent_signatures]")
    shown = 0
    for sig, vals in sorted(sig_rows.items(), key=lambda kv: len(kv[1]), reverse=True):
        if len(vals) < args.min_sig_samples:
            continue
        ctr = Counter(v["bucket"] for v in vals)
        avg_main_table = sum(v["mainTable"] for v in vals) / len(vals)
        avg_main_counter = sum(v["mainCounter"] for v in vals) / len(vals)
        avg_main_useful = sum(v["mainUseful"] for v in vals) / len(vals)
        alloc_rate = sum(1 for v in vals if v["allocSuccess"] and v["allocTable"] >= 2) / len(vals)
        use_alt_rate = sum(v["useAlt"] for v in vals) / len(vals)
        mispred_rate = sum(1 for v in vals if v["mispred"]) / len(vals)
        disp_sig = sig if sig else "<empty>"
        print(
            f"{disp_sig}\trows={len(vals)}\tuniq={len(ctr)}\t"
            f"top1={top_share(ctr,1)*100:.2f}%\ttop3={top_share(ctr,3)*100:.2f}%\t"
            f"entropy={entropy(ctr):.2f}\tavgMainTable={avg_main_table:.2f}\t"
            f"avgMainCounter={avg_main_counter:.2f}\tavgMainUseful={avg_main_useful:.2f}\t"
            f"allocRate={alloc_rate*100:.2f}%\tuseAltRate={use_alt_rate*100:.2f}%\t"
            f"mispredRate={mispred_rate*100:.2f}%"
        )
        shown += 1
        if shown >= 12:
            break


if __name__ == "__main__":
    main()
