#!/usr/bin/env python3
"""Split StrideTrainTrace into training vs prediction CSVs, HTML tables, and a quick plot.

Usage:
    python util/parse_stride_trace.py \\
        --db $HOME/Trace.db \\
        --out-train train_stride.csv \\
        --out-predict predict_stride.csv \\
        --html-train train_stride.html \\
        --html-predict predict_stride.html \\
        --plot stride_trace.png --plot-limit 8000

The script streams results to avoid loading the whole table into memory.
"""

import argparse
import csv
import html
import os
import sqlite3
from typing import Iterable, List, Sequence

try:
    from matplotlib import pyplot as plt
    from matplotlib import ticker
    _HAS_MPL = True
except ImportError:
    _HAS_MPL = False
    plt = None
    ticker = None

DEFAULT_DB = (
    "/nfs/home/changqingyu/gem5_run_res/Correlation/"
    "xsidealfetch_newPFalign_260108_kmhv3_removeL2Filter_counter/"
    "GemsFDTD_30385_0.268180/Trace.db"
)


def get_columns(conn: sqlite3.Connection) -> List[str]:
    """Return column names from StrideTrainTrace in declared order."""
    cur = conn.execute("PRAGMA table_info('StrideTrainTrace')")
    cols = [row[1] for row in cur.fetchall()]
    if not cols:
        raise RuntimeError("StrideTrainTrace table not found in DB")
    return cols


def _is_hex_column(col: str) -> bool:
    return col.lower() in {"addr", "pc", "hashpc"}


def _format_cell(col: str, val) -> str:
    if val is None:
        return ""
    if _is_hex_column(col):
        try:
            return hex(int(val))
        except (ValueError, TypeError):
            return str(val)
    return str(val)


def write_subset(conn: sqlite3.Connection, where_clause: str, out_path: str, columns: List[str]) -> int:
    """Write subset matching where_clause to CSV, return written row count."""
    os.makedirs(os.path.dirname(os.path.abspath(out_path)) or '.', exist_ok=True)
    query = f"SELECT {', '.join(columns)} FROM StrideTrainTrace WHERE {where_clause} ORDER BY Tick"
    count = 0
    with open(out_path, "w", newline="") as csvfile:
        writer = csv.writer(csvfile)
        writer.writerow(columns)
        for row in conn.execute(query):
            writer.writerow(row)
            count += 1
    return count


def write_subset_html(
    conn: sqlite3.Connection,
    where_clause: str,
    out_path: str,
    columns: List[str],
    title: str,
) -> int:
    """Write subset as an HTML table, return written row count."""
    os.makedirs(os.path.dirname(os.path.abspath(out_path)) or '.', exist_ok=True)
    query = f"SELECT {', '.join(columns)} FROM StrideTrainTrace WHERE {where_clause} ORDER BY Tick"
    count = 0
    with open(out_path, "w") as f:
        f.write("<!DOCTYPE html>\n<html><head><meta charset='utf-8'>\n")
        f.write("<title>StrideTrainTrace - " + html.escape(title) + "</title>\n")
        f.write(
            "<style>body{font-family:Arial,Helvetica,sans-serif;margin:1rem;}"
            "table{border-collapse:collapse;width:100%;}" \
            "th,td{border:1px solid #ccc;padding:4px 6px;font-size:12px;}" \
            "th{background:#f3f3f3;position:sticky;top:0;}" \
            "tr:nth-child(even){background:#fafafa;}" \
            "</style></head><body>\n"
        )
        f.write("<h2>" + html.escape(title) + "</h2>\n")
        f.write("<div style='margin-bottom:8px;'>" + "".join(
            f"<label style='margin-right:8px;font-size:12px;'>{html.escape(col)} "
            f"<input class='col-filter' data-col='{idx}' style='width:120px;font-size:12px;'></label>"
            for idx, col in enumerate(columns)
        ) + "</div>\n")
        f.write("<table><thead><tr>" + "".join(f"<th>{html.escape(col)}</th>" for col in columns) + "</tr></thead><tbody>\n")
        for row in conn.execute(query):
            f.write("<tr>")
            for col, cell in zip(columns, row):
                f.write(f"<td>{html.escape(_format_cell(col, cell))}</td>")
            f.write("</tr>\n")
            count += 1
        f.write("</tbody></table>\n")
        f.write(f"<p>Total rows: {count}</p>\n")
        f.write(
            "<script>\n"
            "const filters=[...document.querySelectorAll('.col-filter')];\n"
            "const rows=[...document.querySelectorAll('tbody tr')];\n"
            "filters.forEach(f=>f.addEventListener('input',()=>{\n"
            "  const terms=filters.map(x=>x.value.toLowerCase());\n"
            "  rows.forEach(r=>{\n"
            "    const cells=[...r.children].map(td=>td.textContent.toLowerCase());\n"
            "    const show=terms.every((t,i)=>!t||cells[i].includes(t));\n"
            "    r.style.display=show?'':'none';\n"
            "  });\n"
            "}));\n"
            "</script>\n"
        )
        f.write("</body></html>")
    return count


def _fetch_for_plot(conn: sqlite3.Connection, limit: int) -> Sequence[Sequence[int]]:
    query = "SELECT Tick, Addr, IsTrain FROM StrideTrainTrace ORDER BY Tick"
    if limit > 0:
        query += f" LIMIT {int(limit)}"
    data = conn.execute(query).fetchall()
    ticks_train, addrs_train = [], []
    ticks_pred, addrs_pred = [], []
    for tick, addr, is_train in data:
        if is_train:
            ticks_train.append(int(tick))
            addrs_train.append(int(addr))
        else:
            ticks_pred.append(int(tick))
            addrs_pred.append(int(addr))
    return ticks_train, addrs_train, ticks_pred, addrs_pred


def plot_trace(conn: sqlite3.Connection, out_path: str, limit: int) -> None:
    if not _HAS_MPL:
        raise RuntimeError("matplotlib is required for plotting; install it or omit --plot")
    ticks_train, addrs_train, ticks_pred, addrs_pred = _fetch_for_plot(conn, limit)
    if not ticks_train and not ticks_pred:
        print("No rows to plot; skipping plot generation")
        return

    fig, ax = plt.subplots(figsize=(10, 5))
    if ticks_train:
        ax.scatter(ticks_train, addrs_train, s=6, c="#1f77b4", label="IsTrain=1")
    if ticks_pred:
        ax.scatter(ticks_pred, addrs_pred, s=6, c="#d62728", label="IsTrain=0")
    ax.set_xlabel("Tick")
    ax.set_ylabel("Addr (hex)")
    ax.yaxis.set_major_formatter(ticker.FuncFormatter(lambda x, _: hex(int(x))))
    ax.legend(loc="best")
    ax.set_title("StrideTrainTrace Addr vs Tick")
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    plt.close(fig)


def main() -> None:
    parser = argparse.ArgumentParser(description="Split StrideTrainTrace into train/predict CSV files, HTML tables, and plot")
    parser.add_argument("--db", default=DEFAULT_DB, help="Path to Trace.db containing StrideTrainTrace")
    parser.add_argument("--out-train", default="stride_train.csv", help="Output CSV for IsTrain=1 rows")
    parser.add_argument(
        "--out-predict",
        default="stride_predict.csv",
        help="Output CSV for IsTrain=0 rows (predicted addresses)",
    )
    parser.add_argument("--html-train", default="stride_train.html", help="Output HTML table for IsTrain=1 rows")
    parser.add_argument(
        "--html-predict",
        default="stride_predict.html",
        help="Output HTML table for IsTrain=0 rows",
    )
    parser.add_argument("--plot", default=None, help="Optional PNG output for Addr vs Tick scatter")
    parser.add_argument("--plot-limit", type=int, default=8000, help="Max rows to load for plotting (0 = all)")
    args = parser.parse_args()

    if not os.path.exists(args.db):
        raise FileNotFoundError(f"DB not found: {args.db}")

    with sqlite3.connect(args.db) as conn:
        cols = get_columns(conn)
        train_rows = write_subset(conn, "IsTrain = 1", args.out_train, cols)
        predict_rows = write_subset(conn, "IsTrain = 0", args.out_predict, cols)
        train_rows_html = write_subset_html(conn, "IsTrain = 1", args.html_train, cols, "Stride Training Trace")
        predict_rows_html = write_subset_html(conn, "IsTrain = 0", args.html_predict, cols, "Stride Prediction Trace")
        if args.plot:
            plot_trace(conn, args.plot, args.plot_limit)

    print(f"Wrote {train_rows} training rows to {args.out_train}")
    print(f"Wrote {predict_rows} prediction rows to {args.out_predict}")
    print(f"Wrote {train_rows_html} training rows to {args.html_train}")
    print(f"Wrote {predict_rows_html} prediction rows to {args.html_predict}")
    if args.plot:
        print(f"Wrote plot to {args.plot} (limit={args.plot_limit})")


if __name__ == "__main__":
    main()
