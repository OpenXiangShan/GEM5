import argparse
import math
import sqlite3
import subprocess
from collections import defaultdict
from functools import lru_cache
import textwrap

try:
    import pandas as pd
except ModuleNotFoundError:
    pd = None


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("sqldb")
    parser.add_argument("-p", "--period", action="store", default=1, type=int)
    parser.add_argument("-n", "--top", action="store", default=20, type=int)
    parser.add_argument("--detail", action="store_true", default=False)
    parser.add_argument("--pc", action="append", default=[],
                        help="filter by PC, can be repeated, accepts hex or decimal")
    parser.add_argument("-r", "--rtl_dasm", action="store_true", default=False,
                        help="use spike-dasm when the disassembly column is numeric")
    return parser.parse_args()


args = parse_args()
period = max(1, int(args.period))

replay_reason_desc = {
    "C": "Cache Miss",
    "T": "TLB Miss",
    "B": "Bank Conflict",
    "N": "Nuke",
    "S": "Cache Stall",
    "R": "RAR replay",
    "W": "RAW replay",
    "O": "Other Reason",
}
replay_reason_order = list(replay_reason_desc.keys())


@lru_cache(maxsize=None)
def disassemble(val):
    if type(val) is str:
        return val
    if not args.rtl_dasm:
        return hex(val)

    hex_val = hex(val).lower()
    command = f'echo "DASM({hex_val})" | spike-dasm'
    return subprocess.run(
        command,
        shell=True,
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()


def parse_pc_filters(values):
    pcs = set()
    for raw in values:
        pcs.add(int(raw, 0))
    return pcs


def has_table(cur, name):
    cur.execute(
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=? LIMIT 1",
        (name,),
    )
    return cur.fetchone() is not None


def format_scalar(value):
    if isinstance(value, float):
        if math.isnan(value) or math.isinf(value):
            return str(value)
        return f"{value:.3f}".rstrip("0").rstrip(".")
    if value is None:
        return ""
    return str(value)


def print_rows(rows, headers):
    formatted = [[format_scalar(value) for value in row] for row in rows]
    numeric_col = []
    for col_idx in range(len(headers)):
        is_numeric = True
        for row in rows:
            value = row[col_idx]
            if value is None:
                continue
            if not isinstance(value, (int, float)):
                is_numeric = False
                break
        numeric_col.append(is_numeric)

    widths = []
    for col_idx, header in enumerate(headers):
        width = len(header)
        for row in formatted:
            width = max(width, len(row[col_idx]))
        widths.append(width)

    def format_line(values):
        parts = []
        for col_idx, value in enumerate(values):
            align = ">" if numeric_col[col_idx] else "<"
            parts.append(f"{value:{align}{widths[col_idx]}}")
        return "  ".join(parts)

    print(format_line(headers))
    for row in formatted:
        print(format_line(row))


def print_wrapped_rows(rows, headers, wrap_col=None, wrap_width=80):
    formatted = [[format_scalar(value) for value in row] for row in rows]
    numeric_col = []
    for col_idx in range(len(headers)):
        is_numeric = True
        for row in rows:
            value = row[col_idx]
            if value is None:
                continue
            if not isinstance(value, (int, float)):
                is_numeric = False
                break
        numeric_col.append(is_numeric)

    widths = []
    for col_idx, header in enumerate(headers):
        width = len(header)
        for row in formatted:
            if wrap_col is not None and col_idx == wrap_col:
                width = max(width, min(len(row[col_idx]), wrap_width))
            else:
                width = max(width, len(row[col_idx]))
        widths.append(width)

    def format_line(values):
        parts = []
        for col_idx, value in enumerate(values):
            align = ">" if numeric_col[col_idx] else "<"
            parts.append(f"{value:{align}{widths[col_idx]}}")
        return "  ".join(parts)

    def wrap_value(col_idx, value):
        if wrap_col is not None and col_idx == wrap_col:
            wrapped = textwrap.wrap(
                value,
                width=wrap_width,
                break_long_words=False,
                break_on_hyphens=False,
                replace_whitespace=False,
            )
            return wrapped if wrapped else [""]
        return [value]

    print(format_line(headers))
    for row in formatted:
        wrapped_cols = [wrap_value(col_idx, value) for col_idx, value in enumerate(row)]
        max_lines = max(len(col) for col in wrapped_cols)
        for line_idx in range(max_lines):
            values = []
            for col_idx, col in enumerate(wrapped_cols):
                values.append(col[line_idx] if line_idx < len(col) else "")
            print(format_line(values))


def normalize_stage_ticks(pos):
    normalized = list(pos)
    for idx in range(1, len(normalized)):
        if normalized[idx] == 0 or normalized[idx] < normalized[idx - 1]:
            normalized[idx] = normalized[idx - 1]
    return normalized


def join_reason_str(events):
    if not events:
        return ""
    return "".join(reason for _, reason in events)


def format_detail_timeline(exec_tick, replay_events, block_start_tick, writeback_tick):
    points = []
    if exec_tick > 0:
        points.append((exec_tick, "E"))
    for tick, reason in replay_events:
        if exec_tick <= 0 or tick >= exec_tick:
            points.append((tick, reason))
    if block_start_tick and exec_tick > 0 and exec_tick < block_start_tick < writeback_tick:
        points.append((block_start_tick, "ROBS"))

    if not points:
        return f"W@{writeback_tick // period}" if writeback_tick > 0 else ""

    priority = {"E": 0, "ROBS": 1, "W": 3}

    def sort_key(item):
        tick, label = item
        return (tick, priority.get(label, 2))

    points.sort(key=sort_key)

    pieces = []
    after_block = False
    first = True
    for tick, label in points:
        if not first:
            pieces.append(" >>> " if after_block else " --- ")
        if label == "ROBS":
            pieces.append(f"ROBS@{tick // period}")
            after_block = True
        else:
            token = f"{label}@{tick // period}"
            if after_block and label != "W":
                token = f"!!{token}!!"
            pieces.append(token)
        first = False

    if writeback_tick > 0:
        pieces.append(" >>> " if after_block else " --- ")
        token = f"W@{writeback_tick // period}"
        if after_block:
            token = f"!!{token}!!"
        pieces.append(token)

    nodes = []
    current_sep = ""
    for piece in pieces:
        if piece in (" --- ", " >>> "):
            current_sep = piece
            continue
        if nodes:
            prev_tick = int(nodes[-1].split("@", 1)[1].replace("!!", ""))
            curr_tick = int(piece.replace("!!", "").split("@", 1)[1])
            delta = curr_tick - prev_tick
            nodes.append(f" --({delta})--> {piece}")
        else:
            nodes.append(piece)
    return "".join(nodes)


def compute_reason_block_ticks(events, block_start_tick, block_end_tick):
    reason_ticks = defaultdict(int)
    if not events or block_start_tick <= 0 or block_end_tick <= 0:
        return reason_ticks

    effective_block_start = block_start_tick
    if effective_block_start >= block_end_tick:
        return reason_ticks

    for idx, (tick, reason) in enumerate(events):
        seg_start = tick
        if idx == 0 and effective_block_start < seg_start:
            seg_start = effective_block_start
        seg_end = events[idx + 1][0] if idx + 1 < len(events) else block_end_tick

        overlap_start = max(seg_start, effective_block_start)
        overlap_end = min(seg_end, block_end_tick)
        if overlap_end > overlap_start:
            reason_ticks[reason] += overlap_end - overlap_start

    return reason_ticks


def format_reason_stat(count, avg_block_cycles, show_avg):
    if count == 0:
        return "0"
    if not show_avg:
        return str(count)
    return f"{count}({format_scalar(avg_block_cycles)})"


def normalize_rows(rows, col_name):
    idx = {name: i for i, name in enumerate(col_name)}
    pc_filters = parse_pc_filters(args.pc)
    has_filter = bool(pc_filters)

    loads = {}
    for row in rows:
        rid = row[idx["ID"]]
        pc = row[idx["PC"]]
        if has_filter and pc not in pc_filters:
            continue

        entry = loads.setdefault(rid, {
            "row": row,
            "events": [],
        })

        if "ReplayTick" in idx and row[idx["ReplayTick"]] is not None:
            entry["events"].append(
                (row[idx["ReplayTick"]], row[idx["ReplayReason"]])
            )

    return loads


def summarize_by_pc(loads, col_name, has_detail):
    idx = {name: i for i, name in enumerate(col_name)}
    pc_map = {}

    for entry in loads.values():
        row = entry["row"]
        pc = row[idx["PC"]]
        if pc not in pc_map:
            pc_map[pc] = {
                "count": 0,
                "block_cycles": 0,
                "replay_count": 0,
                "replay_span": 0,
                "pos_sum": [0] * 10,
                "replay_reason": defaultdict(int),
                "replay_block_ticks": defaultdict(int),
                "disasm": disassemble(row[idx["DisAsm"]]),
                "pc": pc,
            }

        item = pc_map[pc]
        item["count"] += 1
        pos = [row[idx[name]] for name in [
            "AtFetch", "AtDecode", "AtRename", "AtDispQue", "AtIssueQue",
            "AtIssueArb", "AtIssueReadReg", "AtFU", "AtBypassVal",
            "AtWriteVal", "AtCommit",
        ]]
        pos = normalize_stage_ticks(pos)
        pos_diff = [j - i for i, j in zip(pos[:-1], pos[1:])]
        item["pos_sum"] = [x + y for x, y in zip(item["pos_sum"], pos_diff)]
        commit_tick = row[idx["AtCommit"]]

        events = entry["events"]
        writeback_tick = row[idx["AtWriteVal"]]
        if events:
            item["replay_count"] += len(events)
            item["replay_span"] += max(0, writeback_tick - events[0][0])
            for _, reason in events:
                item["replay_reason"][reason] += 1
        else:
            replay_str = row[idx["ReplayStr"]] if "ReplayStr" in idx else ""
            item["replay_count"] += len(replay_str)
            if replay_str:
                for reason in replay_str:
                    item["replay_reason"][reason] += 1
                if not has_detail and row[idx["LastReplay"]] > 0:
                    item["replay_span"] += max(0, writeback_tick - row[idx["LastReplay"]])

        block_start_tick = row[idx.get("BlockStartTick", "BlockStartTick")] if "BlockStartTick" in idx else 0
        if block_start_tick:
            item["block_cycles"] += max(0, (commit_tick - block_start_tick) // period)

        for reason, ticks in compute_reason_block_ticks(events, block_start_tick, commit_tick).items():
            item["replay_block_ticks"][reason] += ticks

    out = []
    for pc, item in pc_map.items():
        count = item["count"]
        reason_counts = [item["replay_reason"].get(code, 0) for code in replay_reason_order]
        reason_stats = []
        for code in replay_reason_order:
            reason_count = item["replay_reason"].get(code, 0)
            avg_block = 0
            if reason_count > 0:
                avg_block = item["replay_block_ticks"].get(code, 0) / reason_count / period
            reason_stats.append(format_reason_stat(reason_count, avg_block, has_detail))
        out.append([
            count,
            hex(pc),
            item["disasm"],
            item["block_cycles"] / count,
            item["replay_count"] / count,
            item["replay_span"] / count / period,
        ] + [x / count / period for x in item["pos_sum"]] + reason_stats)

    out.sort(key=lambda x: (x[0], x[3]), reverse=True)
    return out[: args.top]


def print_details(loads, col_name):
    idx = {name: i for i, name in enumerate(col_name)}
    rows = []
    for entry in loads.values():
        row = entry["row"]
        events = entry["events"]
        block_start_tick = row[idx["BlockStartTick"]] if "BlockStartTick" in idx else 0
        replay_str = join_reason_str(events) or row[idx["ReplayStr"]]
        exec_tick = row[idx["AtIssueReadReg"]] if "AtIssueReadReg" in idx else (
            row[idx["AtFU"]] if "AtFU" in idx else 0
        )
        writeback_tick = row[idx["AtWriteVal"]] if "AtWriteVal" in idx else 0
        rows.append([
            row[idx["ID"]],
            hex(row[idx["PC"]]),
            disassemble(row[idx["DisAsm"]]),
            block_start_tick // period if block_start_tick else 0,
            row[idx["AtCommit"]] // period,
            len(events) if events else len(replay_str),
            format_detail_timeline(exec_tick, events, block_start_tick, writeback_tick) or replay_str,
            replay_str,
        ])

    rows.sort(key=lambda x: (x[4], x[0]))
    print_wrapped_rows(
        rows,
        [
            "ID",
            "PC",
            "DisAsm",
            "BlockStart",
            "AtCommit",
            "ReplayCnt",
            "Timeline",
            "ReplayStr",
        ],
        wrap_col=6,
        wrap_width=80,
    )


with sqlite3.connect(args.sqldb) as db:
    cur = db.cursor()
    has_detail = has_table(cur, "LoadReplayTrace")

    if has_detail:
        cur.execute(
            "SELECT LifeTimeCommitTrace.*, LoadLifeTimeCommitTrace.*, "
            "LoadReplayTrace.ReplayIdx, LoadReplayTrace.ReplayReason, "
            "LoadReplayTrace.ReplayTick, LoadReplayTrace.BlockStartTick "
            "FROM LifeTimeCommitTrace "
            "INNER JOIN LoadLifeTimeCommitTrace ON LifeTimeCommitTrace.ID = LoadLifeTimeCommitTrace.ID "
            "LEFT JOIN LoadReplayTrace ON LifeTimeCommitTrace.ID = LoadReplayTrace.ID "
            "WHERE LifeTimeCommitTrace.AtCommit != 0 "
            "ORDER BY LifeTimeCommitTrace.ID, LoadReplayTrace.ReplayIdx;"
        )
    else:
        cur.execute(
            "SELECT LifeTimeCommitTrace.*, LoadLifeTimeCommitTrace.* "
            "FROM LifeTimeCommitTrace "
            "INNER JOIN LoadLifeTimeCommitTrace ON LifeTimeCommitTrace.ID = LoadLifeTimeCommitTrace.ID "
            "WHERE LifeTimeCommitTrace.AtCommit != 0;"
        )

    col_name = [i[0] for i in cur.description]
    rows = cur.fetchall()
    loads = normalize_rows(rows, col_name)

    if args.detail:
        print_details(loads, col_name)
    else:
        summary = summarize_by_pc(loads, col_name, has_detail)
        col_name = [
            "Count",
            "PC",
            "DisAsm",
            "BlockCycles",
            "ReplayCount",
            "ReplaySpan",
            "d",
            "r",
            "D",
            "i",
            "a",
            "g",
            "e",
            "b",
            "w",
            "c",
        ] + list(replay_reason_desc.values())
        print_rows(summary, col_name)
