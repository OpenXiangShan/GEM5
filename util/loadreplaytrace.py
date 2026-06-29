import argparse
import os
import math
import sqlite3
import subprocess
import sys
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
    parser.add_argument(
        "--sort-by",
        choices=("count", "total-block-cycles"),
        default="count",
        help=(
            "sort summary rows by Count or by Count * BlockCycles "
            "(default: count)"
        ),
    )
    mode_group = parser.add_mutually_exclusive_group()
    mode_group.add_argument("--detail", action="store_true", default=False)
    mode_group.add_argument(
        "--stage-summary", action="store_true", default=False,
        help="write per-PC cache-stage averages to a separate file",
    )
    parser.add_argument(
        "--rob-stall-only", "--robstall-only",
        dest="rob_stall_only", action="store_true", default=False,
        help="with --detail, only show loads with BlockStartTick > 0",
    )
    parser.add_argument(
        "--stage-summary-file", default=None,
        help="optional output file for --stage-summary",
    )
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


def build_pc_filter_clause(pc_filters):
    if not pc_filters:
        return "", []
    ordered = sorted(pc_filters)
    placeholders = ", ".join("?" for _ in ordered)
    return f" AND LifeTimeCommitTrace.PC IN ({placeholders})", ordered


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
    print_rows_to_stream(rows, headers, sys.stdout)


def print_rows_to_stream(rows, headers, stream):
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

    print(format_line(headers), file=stream)
    for row in formatted:
        print(format_line(row), file=stream)


def print_wrapped_rows(rows, headers, wrap_col=None, wrap_width=80):
    print_wrapped_rows_to_stream(rows, headers, sys.stdout, wrap_col, wrap_width)


def print_wrapped_rows_to_stream(rows, headers, stream, wrap_col=None, wrap_width=80):
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

    print(format_line(headers), file=stream)
    for row in formatted:
        wrapped_cols = [wrap_value(col_idx, value) for col_idx, value in enumerate(row)]
        max_lines = max(len(col) for col in wrapped_cols)
        for line_idx in range(max_lines):
            values = []
            for col_idx, col in enumerate(wrapped_cols):
                values.append(col[line_idx] if line_idx < len(col) else "")
            print(format_line(values), file=stream)


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


def format_detail_timeline(exec_tick, replay_events, block_start_tick,
                           l1_return_tick, l2_return_tick, l3_return_tick,
                           writeback_tick):
    points = []
    if exec_tick > 0:
        points.append((exec_tick, "E"))
    for tick, reason in replay_events:
        if exec_tick <= 0 or tick >= exec_tick:
            points.append((tick, reason))
    if l1_return_tick > 0:
        points.append((l1_return_tick, "L1R"))
    if l2_return_tick > 0:
        points.append((l2_return_tick, "L2R"))
    if l3_return_tick > 0:
        points.append((l3_return_tick, "L3R"))
    if block_start_tick and exec_tick > 0 and exec_tick < block_start_tick < writeback_tick:
        points.append((block_start_tick, "ROBS"))

    if not points:
        return f"W@{writeback_tick // period}" if writeback_tick > 0 else ""

    priority = {"E": 0, "L1R": 1, "L2R": 2, "L3R": 3, "ROBS": 4, "W": 5}

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


def classify_cache_miss_bucket(row, idx, l2_miss_col, l3_miss_col):
    """
    Bucket cache-miss stall by the deepest effective miss level reached.

    Using the effective L2/L3 miss flags keeps the three buckets mutually
    exclusive and additive: pure L1-miss stalls stay in the L1 bucket, L2-miss
    stalls cover L1+L2 miss paths that stop at L3 hit, and L3-miss stalls cover
    the full L1+L2+L3 miss path.
    """
    if get_optional_bool(row, idx, l3_miss_col):
        return "l3"
    if get_optional_bool(row, idx, l2_miss_col):
        return "l2"
    return "l1"


def format_reason_stat(count, avg_block_cycles, show_avg):
    if count == 0:
        return "0"
    if not show_avg:
        return str(count)
    return f"{count}({format_scalar(avg_block_cycles)})"


def get_optional_value(row, idx, name, default=0):
    if name not in idx:
        return default
    value = row[idx[name]]
    if value is None:
        return default
    return value


def get_optional_bool(row, idx, name):
    return int(bool(get_optional_value(row, idx, name, 0)))


def prefer_column(idx, preferred, fallback):
    return preferred if preferred in idx else fallback


def scaled_tick(value):
    if value <= 0:
        return None
    return value / period


def scaled_delta(start_tick, end_tick):
    if start_tick <= 0 or end_tick <= 0 or end_tick < start_tick:
        return None
    return (end_tick - start_tick) / period


def safe_stage_delta(start_tick, end_tick):
    if start_tick <= 0 or end_tick <= 0 or end_tick < start_tick:
        return None
    return (end_tick - start_tick) / period


def avg_or_none(sum_value, count_value):
    if count_value == 0:
        return None
    return sum_value / count_value


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
    has_level_info = "L1Miss" in idx
    l2_miss_col = prefer_column(idx, "EffL2Miss", "L2Miss")
    l3_miss_col = prefer_column(idx, "EffL3Miss", "L3Miss")
    l2_return_col = prefer_column(idx, "EffL2ReturnTick", "L2ReturnTick")
    l3_return_col = prefer_column(idx, "EffL3ReturnTick", "L3ReturnTick")
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
                "cache_miss_block_ticks": 0,
                "cache_miss_block_ticks_l1": 0,
                "cache_miss_block_ticks_l2": 0,
                "cache_miss_block_ticks_l3": 0,
                "disasm": disassemble(row[idx["DisAsm"]]),
                "pc": pc,
            }
            if has_level_info:
                pc_map[pc].update({
                    "l1_miss_count": 0,
                    "l2_miss_count": 0,
                    "l3_miss_count": 0,
                    "l1_ret_lat_sum": 0,
                    "l1_ret_lat_count": 0,
                    "l2_ret_lat_sum": 0,
                    "l2_ret_lat_count": 0,
                    "l3_ret_lat_sum": 0,
                    "l3_ret_lat_count": 0,
                    "ready_lat_sum": 0,
                    "ready_lat_count": 0,
                })

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

        block_start_tick = get_optional_value(row, idx, "BlockStartTick", 0)
        if block_start_tick:
            item["block_cycles"] += max(0, (commit_tick - block_start_tick) // period)

        for reason, ticks in compute_reason_block_ticks(events, block_start_tick, commit_tick).items():
            item["replay_block_ticks"][reason] += ticks
            if reason == "C":
                bucket = classify_cache_miss_bucket(row, idx, l2_miss_col, l3_miss_col)
                item["cache_miss_block_ticks"] += ticks
                item[f"cache_miss_block_ticks_{bucket}"] += ticks

        if has_level_info:
            item["l1_miss_count"] += get_optional_bool(row, idx, "L1Miss")
            item["l2_miss_count"] += get_optional_bool(row, idx, l2_miss_col)
            item["l3_miss_count"] += get_optional_bool(row, idx, l3_miss_col)

            level_fields = [
                ("L1ReturnTick", "l1_ret_lat_sum", "l1_ret_lat_count"),
                (l2_return_col, "l2_ret_lat_sum", "l2_ret_lat_count"),
                (l3_return_col, "l3_ret_lat_sum", "l3_ret_lat_count"),
                ("DataReadyTick", "ready_lat_sum", "ready_lat_count"),
            ]
            for field, sum_key, count_key in level_fields:
                latency = scaled_delta(
                    block_start_tick,
                    get_optional_value(row, idx, field, 0),
                )
                if latency is not None:
                    item[sum_key] += latency
                    item[count_key] += 1

    out = []
    for pc, item in pc_map.items():
        count = item["count"]
        reason_stats = []
        for code in replay_reason_order:
            reason_count = item["replay_reason"].get(code, 0)
            avg_block = 0
            if reason_count > 0:
                avg_block = item["replay_block_ticks"].get(code, 0) / reason_count / period
            reason_stats.append(format_reason_stat(reason_count, avg_block, has_detail))
        row = [
            count,
            hex(pc),
            item["disasm"],
            item["block_cycles"] / count,
            item["block_cycles"],
            item["replay_count"] / count,
            item["replay_span"] / count / period,
        ]
        if has_level_info:
            row.extend([
                item["l1_miss_count"] * 100.0 / count,
                item["l2_miss_count"] * 100.0 / count,
                item["l3_miss_count"] * 100.0 / count,
                item["l1_ret_lat_sum"] / item["l1_ret_lat_count"]
                if item["l1_ret_lat_count"] else None,
                item["l2_ret_lat_sum"] / item["l2_ret_lat_count"]
                if item["l2_ret_lat_count"] else None,
                item["l3_ret_lat_sum"] / item["l3_ret_lat_count"]
                if item["l3_ret_lat_count"] else None,
                item["ready_lat_sum"] / item["ready_lat_count"]
                if item["ready_lat_count"] else None,
                item["cache_miss_block_ticks"] / count / period,
                item["cache_miss_block_ticks_l1"] / count / period,
                item["cache_miss_block_ticks_l2"] / count / period,
                item["cache_miss_block_ticks_l3"] / count / period,
            ])
        row.extend([x / count / period for x in item["pos_sum"]])
        row.extend(reason_stats)
        out.append(row)

    if args.sort_by == "total-block-cycles":
        out.sort(key=lambda x: (x[4], x[0], x[3]), reverse=True)
    else:
        out.sort(key=lambda x: (x[0], x[3]), reverse=True)
    return out[: args.top], has_level_info


def summarize_stage_by_pc(loads, col_name):
    idx = {name: i for i, name in enumerate(col_name)}
    required = [
        "PC", "DisAsm",
        "L1Miss", "EffL2Miss", "EffL3Miss",
        "ReqCreateTick", "L1MissTick", "L1SendTick", "L1RespRecvTick",
        "EffL2MissTick", "EffL2SendTick", "EffL2RespRecvTick",
        "EffL3MissTick", "EffL3SendTick", "EffL3RespRecvTick",
    ]
    missing = [name for name in required if name not in idx]
    if missing:
        raise RuntimeError(
            "stage-summary requires these columns: " + ", ".join(missing)
        )

    pc_map = {}

    def ensure_pc(pc, row):
        if pc not in pc_map:
            pc_map[pc] = {
                "count": 0,
                "l1_all_cnt": 0,
                "l1_all_svc_sum": 0,
                "l1_all_svc_count": 0,
                "l1_only_cnt": 0,
                "l1_only_svc_sum": 0,
                "l1_only_svc_count": 0,
                "l2_all_cnt": 0,
                "l2_all_svc_sum": 0,
                "l2_all_svc_count": 0,
                "l2_only_cnt": 0,
                "l2_only_svc_sum": 0,
                "l2_only_svc_count": 0,
                "l3_all_cnt": 0,
                "l3_all_svc_sum": 0,
                "l3_all_svc_count": 0,
                "req_to_l1miss_sum": 0,
                "req_to_l1miss_count": 0,
                "l1miss_to_l1send_sum": 0,
                "l1miss_to_l1send_count": 0,
                "l1send_to_l2miss_sum": 0,
                "l1send_to_l2miss_count": 0,
                "l2miss_to_l2send_sum": 0,
                "l2miss_to_l2send_count": 0,
                "l2send_to_l3miss_sum": 0,
                "l2send_to_l3miss_count": 0,
                "l3miss_to_l3send_sum": 0,
                "l3miss_to_l3send_count": 0,
                "l3resp_to_l2resp_sum": 0,
                "l3resp_to_l2resp_count": 0,
                "l2resp_to_l1resp_sum": 0,
                "l2resp_to_l1resp_count": 0,
                "disasm": disassemble(row[idx["DisAsm"]]),
                "pc": pc,
            }
        return pc_map[pc]

    for entry in loads.values():
        row = entry["row"]
        pc = row[idx["PC"]]
        item = ensure_pc(pc, row)
        item["count"] += 1

        l1_miss = get_optional_bool(row, idx, "L1Miss")
        l2_miss = get_optional_bool(row, idx, "EffL2Miss")
        l3_miss = get_optional_bool(row, idx, "EffL3Miss")

        req_create_tick = get_optional_value(row, idx, "ReqCreateTick", 0)
        l1_miss_tick = get_optional_value(row, idx, "L1MissTick", 0)
        l1_send_tick = get_optional_value(row, idx, "L1SendTick", 0)
        l1_resp_tick = get_optional_value(row, idx, "L1RespRecvTick", 0)
        l2_miss_tick = get_optional_value(row, idx, "EffL2MissTick", 0)
        l2_send_tick = get_optional_value(row, idx, "EffL2SendTick", 0)
        l2_resp_tick = get_optional_value(row, idx, "EffL2RespRecvTick", 0)
        l3_miss_tick = get_optional_value(row, idx, "EffL3MissTick", 0)
        l3_send_tick = get_optional_value(row, idx, "EffL3SendTick", 0)
        l3_resp_tick = get_optional_value(row, idx, "EffL3RespRecvTick", 0)

        if l1_miss:
            item["l1_all_cnt"] += 1
            svc = safe_stage_delta(l1_send_tick, l1_resp_tick)
            if svc is not None:
                item["l1_all_svc_sum"] += svc
                item["l1_all_svc_count"] += 1
            if not l2_miss:
                item["l1_only_cnt"] += 1
                svc = safe_stage_delta(l1_send_tick, l1_resp_tick)
                if svc is not None:
                    item["l1_only_svc_sum"] += svc
                    item["l1_only_svc_count"] += 1

            delta = safe_stage_delta(req_create_tick, l1_miss_tick)
            if delta is not None:
                item["req_to_l1miss_sum"] += delta
                item["req_to_l1miss_count"] += 1
            delta = safe_stage_delta(l1_miss_tick, l1_send_tick)
            if delta is not None:
                item["l1miss_to_l1send_sum"] += delta
                item["l1miss_to_l1send_count"] += 1

        if l2_miss:
            item["l2_all_cnt"] += 1
            svc = safe_stage_delta(l2_send_tick, l2_resp_tick)
            if svc is not None:
                item["l2_all_svc_sum"] += svc
                item["l2_all_svc_count"] += 1
            if not l3_miss:
                item["l2_only_cnt"] += 1
                svc = safe_stage_delta(l2_send_tick, l2_resp_tick)
                if svc is not None:
                    item["l2_only_svc_sum"] += svc
                    item["l2_only_svc_count"] += 1

            delta = safe_stage_delta(l1_send_tick, l2_miss_tick)
            if delta is not None:
                item["l1send_to_l2miss_sum"] += delta
                item["l1send_to_l2miss_count"] += 1
            delta = safe_stage_delta(l2_miss_tick, l2_send_tick)
            if delta is not None:
                item["l2miss_to_l2send_sum"] += delta
                item["l2miss_to_l2send_count"] += 1
            delta = safe_stage_delta(l2_resp_tick, l1_resp_tick)
            if delta is not None:
                item["l2resp_to_l1resp_sum"] += delta
                item["l2resp_to_l1resp_count"] += 1

        if l3_miss:
            item["l3_all_cnt"] += 1
            svc = safe_stage_delta(l3_send_tick, l3_resp_tick)
            if svc is not None:
                item["l3_all_svc_sum"] += svc
                item["l3_all_svc_count"] += 1

            delta = safe_stage_delta(l2_send_tick, l3_miss_tick)
            if delta is not None:
                item["l2send_to_l3miss_sum"] += delta
                item["l2send_to_l3miss_count"] += 1
            delta = safe_stage_delta(l3_miss_tick, l3_send_tick)
            if delta is not None:
                item["l3miss_to_l3send_sum"] += delta
                item["l3miss_to_l3send_count"] += 1
            delta = safe_stage_delta(l3_resp_tick, l2_resp_tick)
            if delta is not None:
                item["l3resp_to_l2resp_sum"] += delta
                item["l3resp_to_l2resp_count"] += 1

    out = []
    for pc, item in pc_map.items():
        row = [
            item["count"],
            hex(pc),
            item["disasm"],
            item["l1_all_cnt"],
            avg_or_none(item["l1_all_svc_sum"], item["l1_all_svc_count"]),
            item["l1_only_cnt"],
            avg_or_none(item["l1_only_svc_sum"], item["l1_only_svc_count"]),
            item["l2_all_cnt"],
            avg_or_none(item["l2_all_svc_sum"], item["l2_all_svc_count"]),
            item["l2_only_cnt"],
            avg_or_none(item["l2_only_svc_sum"], item["l2_only_svc_count"]),
            item["l3_all_cnt"],
            avg_or_none(item["l3_all_svc_sum"], item["l3_all_svc_count"]),
            avg_or_none(item["req_to_l1miss_sum"], item["req_to_l1miss_count"]),
            avg_or_none(item["l1miss_to_l1send_sum"], item["l1miss_to_l1send_count"]),
            avg_or_none(item["l1send_to_l2miss_sum"], item["l1send_to_l2miss_count"]),
            avg_or_none(item["l2miss_to_l2send_sum"], item["l2miss_to_l2send_count"]),
            avg_or_none(item["l2send_to_l3miss_sum"], item["l2send_to_l3miss_count"]),
            avg_or_none(item["l3miss_to_l3send_sum"], item["l3miss_to_l3send_count"]),
            avg_or_none(item["l3resp_to_l2resp_sum"], item["l3resp_to_l2resp_count"]),
            avg_or_none(item["l2resp_to_l1resp_sum"], item["l2resp_to_l1resp_count"]),
        ]
        out.append(row)

    out.sort(key=lambda x: (x[0], x[3]), reverse=True)
    return out[: args.top]


def print_details(loads, col_name):
    idx = {name: i for i, name in enumerate(col_name)}
    has_level_info = "L1Miss" in idx
    l2_miss_col = prefer_column(idx, "EffL2Miss", "L2Miss")
    l3_miss_col = prefer_column(idx, "EffL3Miss", "L3Miss")
    l2_return_col = prefer_column(idx, "EffL2ReturnTick", "L2ReturnTick")
    l3_return_col = prefer_column(idx, "EffL3ReturnTick", "L3ReturnTick")
    rows = []
    for entry in loads.values():
        row = entry["row"]
        events = entry["events"]
        block_start_tick = get_optional_value(row, idx, "BlockStartTick", 0)
        if args.rob_stall_only and block_start_tick <= 0:
            continue
        replay_str = join_reason_str(events) or row[idx["ReplayStr"]]
        exec_tick = row[idx["AtIssueReadReg"]] if "AtIssueReadReg" in idx else (
            row[idx["AtFU"]] if "AtFU" in idx else 0
        )
        writeback_tick = row[idx["AtWriteVal"]] if "AtWriteVal" in idx else 0
        detail_row = [
            row[idx["ID"]],
            hex(row[idx["PC"]]),
            disassemble(row[idx["DisAsm"]]),
            scaled_tick(block_start_tick),
        ]
        if has_level_info:
            l1_return_tick = get_optional_value(row, idx, "L1ReturnTick", 0)
            l2_return_tick = get_optional_value(row, idx, l2_return_col, 0)
            l3_return_tick = get_optional_value(row, idx, l3_return_col, 0)
            ready_tick = get_optional_value(row, idx, "DataReadyTick", 0)
            detail_row.extend([
                get_optional_bool(row, idx, "L1Miss"),
                get_optional_bool(row, idx, l2_miss_col),
                get_optional_bool(row, idx, l3_miss_col),
                scaled_tick(l1_return_tick),
                scaled_tick(l2_return_tick),
                scaled_tick(l3_return_tick),
                scaled_tick(ready_tick),
                scaled_delta(block_start_tick, l1_return_tick),
                scaled_delta(block_start_tick, l2_return_tick),
                scaled_delta(block_start_tick, l3_return_tick),
                scaled_delta(block_start_tick, ready_tick),
            ])
        detail_row.extend([
            scaled_tick(row[idx["AtCommit"]]),
            len(events) if events else len(replay_str),
            format_detail_timeline(
                exec_tick,
                events,
                block_start_tick,
                l1_return_tick if has_level_info else 0,
                l2_return_tick if has_level_info else 0,
                l3_return_tick if has_level_info else 0,
                writeback_tick,
            ) or replay_str,
            replay_str,
        ])
        rows.append(detail_row)

    commit_idx = 15 if has_level_info else 4
    rows.sort(key=lambda x: (x[commit_idx], x[0]))
    headers = [
        "ID",
        "PC",
        "DisAsm",
        "BlockStart",
    ]
    if has_level_info:
        headers.extend([
            "L1Miss",
            "L2Miss",
            "L3Miss",
            "L1Return",
            "L2Return",
            "L3Return",
            "Ready",
            "L1RetLat",
            "L2RetLat",
            "L3RetLat",
            "ReadyLat",
        ])
    headers.extend([
        "AtCommit",
        "ReplayCnt",
        "Timeline",
        "ReplayStr",
    ])
    print_wrapped_rows(rows, headers, wrap_col=len(headers) - 2, wrap_width=80)


with sqlite3.connect(args.sqldb) as db:
    cur = db.cursor()
    has_detail = has_table(cur, "LoadReplayTrace")
    pc_filter_clause, pc_filter_params = build_pc_filter_clause(parse_pc_filters(args.pc))

    if has_detail:
        cur.execute(
            "SELECT LifeTimeCommitTrace.*, LoadLifeTimeCommitTrace.*, "
            "LoadReplayTrace.ReplayIdx, LoadReplayTrace.ReplayReason, "
            "LoadReplayTrace.ReplayTick, LoadReplayTrace.BlockStartTick "
            "FROM LifeTimeCommitTrace "
            "INNER JOIN LoadLifeTimeCommitTrace ON LifeTimeCommitTrace.ID = LoadLifeTimeCommitTrace.ID "
            "LEFT JOIN LoadReplayTrace ON LifeTimeCommitTrace.ID = LoadReplayTrace.ID "
            "WHERE LifeTimeCommitTrace.AtCommit != 0 "
            f"{pc_filter_clause} "
            "ORDER BY LifeTimeCommitTrace.ID, LoadReplayTrace.ReplayIdx;",
            pc_filter_params,
        )
    else:
        cur.execute(
            "SELECT LifeTimeCommitTrace.*, LoadLifeTimeCommitTrace.* "
            "FROM LifeTimeCommitTrace "
            "INNER JOIN LoadLifeTimeCommitTrace ON LifeTimeCommitTrace.ID = LoadLifeTimeCommitTrace.ID "
            "WHERE LifeTimeCommitTrace.AtCommit != 0 "
            f"{pc_filter_clause};",
            pc_filter_params,
        )

    col_name = [i[0] for i in cur.description]
    rows = cur.fetchall()
    loads = normalize_rows(rows, col_name)

    if args.stage_summary:
        summary = summarize_stage_by_pc(loads, col_name)
        out_dir = os.path.dirname(os.path.abspath(args.sqldb))
        if args.stage_summary_file:
            out_path = args.stage_summary_file
        else:
            stem = os.path.splitext(os.path.basename(args.sqldb))[0]
            out_path = os.path.join(out_dir, f"{stem}_stage_summary.txt")
        out_parent = os.path.dirname(os.path.abspath(out_path))
        if out_parent:
            os.makedirs(out_parent, exist_ok=True)
        headers = [
            "Count",
            "PC",
            "DisAsm",
            "L1AllCnt",
            "AvgL1AllSvc",
            "L1OnlyCnt",
            "AvgL1OnlySvc",
            "L2AllCnt",
            "AvgL2AllSvc",
            "L2OnlyCnt",
            "AvgL2OnlySvc",
            "L3AllCnt",
            "AvgL3AllSvc",
            "AvgReqToL1Miss",
            "AvgL1MissToL1Send",
            "AvgL1SendToL2Miss",
            "AvgL2MissToL2Send",
            "AvgL2SendToL3Miss",
            "AvgL3MissToL3Send",
            "AvgL3RespToL2Resp",
            "AvgL2RespToL1Resp",
        ]
        with open(out_path, "w", encoding="utf-8") as out_file:
            print_rows_to_stream(summary, headers, out_file)
        print(f"stage summary written to {out_path}")
    elif args.detail:
        print_details(loads, col_name)
    else:
        summary, has_level_info = summarize_by_pc(loads, col_name, has_detail)
        col_name = [
            "Count",
            "PC",
            "DisAsm",
            "BlockCycles",
            "TotalBlockCycles",
            "ReplayCount",
            "ReplaySpan",
        ]
        if has_level_info:
            col_name.extend([
                "L1Miss%",
                "L2Miss%",
                "L3Miss%",
                "AvgL1RetLat",
                "AvgL2RetLat",
                "AvgL3RetLat",
                "AvgReadyLat",
                "AvgCacheMissStall",
                "AvgL1MissStall",
                "AvgL2MissStall",
                "AvgL3MissStall",
            ])
        col_name.extend([
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
        ] + list(replay_reason_desc.values()))
        print_rows(summary, col_name)
