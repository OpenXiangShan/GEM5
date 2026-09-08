#!/usr/bin/env python3
import argparse
import sqlite3
from collections import Counter, defaultdict
from pathlib import Path


def parse_pc(text: str) -> int:
    text = text.strip().lower()
    if text.startswith("0x"):
        return int(text, 16)
    return int(text, 16)


def fmt_pc(value: int) -> str:
    return f"0x{value:x}"


def classify(pred_taken: int, pred_pos: int, actual_pos: int) -> str:
    if not pred_taken:
        return "none"
    if pred_pos == actual_pos:
        return "self"
    return "same_block_other"


def scan_cond(conn: sqlite3.Connection, branch_pc: int, start_pc: int, taken: int):
    cond = {}
    for i in range(8):
        sql = f"""
            select
                STAMP,
                STARTPC_ADDR,
                CFIPC,
                ACTUALTAKEN,
                MISPREDICT,
                PROVIDERTABLEIDX,
                PROVIDERTAKENCTR_VALUE,
                PROVIDERUSEFULCTR_VALUE,
                HASALT,
                USEALT,
                NEEDALLOCATE,
                ALLOCATESUCCESS,
                ALLOCATETABLEIDX
            from CondTrace_{i}
            where STARTPC_ADDR=?
              and CFIPC=?
              and ACTUALTAKEN=?
        """
        for row in conn.execute(sql, (start_pc, branch_pc, taken)):
            key = (row[0], row[1], row[2], row[3])
            cond[key] = {
                "mispredict": int(row[4]),
                "provider_table": int(row[5]),
                "provider_ctr": int(row[6]),
                "provider_useful": int(row[7]),
                "has_alt": int(row[8]),
                "use_alt": int(row[9]),
                "need_alloc": int(row[10]),
                "alloc_success": int(row[11]),
                "alloc_table": int(row[12]),
            }
    return cond


def scan_train(conn: sqlite3.Connection, branch_pc: int, start_pc: int, taken: int):
    cols = [
        "ID",
        "STAMP",
        "TRAIN_STARTPC_ADDR",
        "TRAIN_PERFMETA_S3PREDICTION_TAKEN",
        "TRAIN_PERFMETA_S3PREDICTION_CFIPOSITION",
        "TRAIN_PERFMETA_S3PREDICTION_TARGET_ADDR",
    ]
    for i in range(8):
        cols += [
            f"TRAIN_BRANCHES_{i}_VALID",
            f"TRAIN_BRANCHES_{i}_BITS_DEBUG_REALCFIPC",
            f"TRAIN_BRANCHES_{i}_BITS_CFIPOSITION",
            f"TRAIN_BRANCHES_{i}_BITS_TAKEN",
            f"TRAIN_BRANCHES_{i}_BITS_MISPREDICT",
        ]
    sql = "select " + ",".join(cols) + " from BpuTrainTrace"

    rows = []
    for row in conn.execute(sql):
        train_id, stamp, row_start_pc, pred_taken, pred_pos, pred_target = row[:6]
        if row_start_pc != start_pc:
            continue
        base = 6
        for _ in range(8):
            valid, row_branch_pc, actual_pos, actual_taken, mispredict = row[base : base + 5]
            base += 5
            if not valid:
                continue
            if row_branch_pc == branch_pc and actual_taken == taken:
                rows.append(
                    {
                        "train_id": int(train_id),
                        "stamp": int(stamp),
                        "start_pc": int(row_start_pc),
                        "pred_taken": int(pred_taken),
                        "pred_pos": int(pred_pos),
                        "pred_target": int(pred_target),
                        "branch_pc": int(row_branch_pc),
                        "actual_pos": int(actual_pos),
                        "actual_taken": int(actual_taken),
                        "mispredict": int(mispredict),
                    }
                )
    return rows


def main() -> None:
    ap = argparse.ArgumentParser(description="Analyze RTL block-level contributor regime from BpuTrainTrace.")
    ap.add_argument("--rtl-db", required=True)
    ap.add_argument("--branch-pc", required=True)
    ap.add_argument("--start-pc", required=True)
    ap.add_argument("--taken", type=int, required=True)
    ap.add_argument("--top", type=int, default=12)
    args = ap.parse_args()

    rtl_db = Path(args.rtl_db)
    branch_pc = parse_pc(args.branch_pc)
    start_pc = parse_pc(args.start_pc)

    conn = sqlite3.connect(rtl_db)
    train_rows = scan_train(conn, branch_pc, start_pc, args.taken)
    if not train_rows:
        print("no rows matched")
        return

    cond_rows = scan_cond(conn, branch_pc, start_pc, args.taken)

    total = len(train_rows)
    pred = Counter()
    cat = Counter()
    top = Counter()
    stats = defaultdict(lambda: {
        "rows": 0,
        "mispredict": 0,
        "joined": 0,
        "provider_table": 0,
        "provider_ctr": 0,
        "provider_useful": 0,
        "has_alt": 0,
        "use_alt": 0,
        "need_alloc": 0,
        "alloc_success": 0,
        "alloc_table": 0,
    })

    for row in train_rows:
        key = classify(row["pred_taken"], row["pred_pos"], row["actual_pos"])
        pred["predTaken=1" if row["pred_taken"] else "predTaken=0"] += 1
        cat[key] += 1
        top[(key, row["pred_taken"], row["pred_pos"], row["pred_target"])] += 1

        bucket = stats[key]
        bucket["rows"] += 1
        bucket["mispredict"] += row["mispredict"]

        cond_key = (row["stamp"], row["start_pc"], row["branch_pc"], row["actual_taken"])
        if cond_key in cond_rows:
            c = cond_rows[cond_key]
            bucket["joined"] += 1
            bucket["provider_table"] += c["provider_table"]
            bucket["provider_ctr"] += c["provider_ctr"]
            bucket["provider_useful"] += c["provider_useful"]
            bucket["has_alt"] += c["has_alt"]
            bucket["use_alt"] += c["use_alt"]
            bucket["need_alloc"] += c["need_alloc"]
            bucket["alloc_success"] += c["alloc_success"]
            bucket["alloc_table"] += c["alloc_table"]

    print(
        f"[context] branch={fmt_pc(branch_pc)} start={fmt_pc(start_pc)} "
        f"actualTaken={args.taken} rows={total}"
    )
    print("[pred_taken_share]")
    for k, v in pred.items():
        print(f"{k}\trows={v}\tshare={v * 100 / total:.2f}%")

    print("[contributor_category]")
    for k, v in cat.items():
        print(f"{k}\trows={v}\tshare={v * 100 / total:.2f}%")

    print("[category_stats]")
    for k in ["self", "same_block_other", "none"]:
        s = stats.get(k)
        if not s or not s["rows"]:
            continue
        line = (
            f"{k}\trows={s['rows']}\tshare={s['rows'] * 100 / total:.2f}%\t"
            f"mispredPct={s['mispredict'] * 100 / s['rows']:.2f}%\tjoined={s['joined']}"
        )
        if s["joined"]:
            line += (
                f"\tavgProviderTable={s['provider_table'] / s['joined']:.2f}"
                f"\tavgProviderCtr={s['provider_ctr'] / s['joined']:.2f}"
                f"\tavgProviderUseful={s['provider_useful'] / s['joined']:.2f}"
                f"\thasAltPct={s['has_alt'] * 100 / s['joined']:.2f}%"
                f"\tuseAltPct={s['use_alt'] * 100 / s['joined']:.2f}%"
                f"\tneedAllocPct={s['need_alloc'] * 100 / s['joined']:.2f}%"
                f"\tallocSuccessPct={s['alloc_success'] * 100 / s['joined']:.2f}%"
                f"\tavgAllocTable={s['alloc_table'] / s['joined']:.2f}"
            )
        print(line)

    print("[top_predicted_contributors]")
    for (k, pred_taken, pred_pos, pred_target), cnt in top.most_common(args.top):
        print(
            f"category={k}\tpredTaken={pred_taken}\tpredPos={pred_pos}\t"
            f"predTarget={fmt_pc(pred_target)}\trows={cnt}\tshare={cnt * 100 / total:.2f}%"
        )


if __name__ == "__main__":
    main()
