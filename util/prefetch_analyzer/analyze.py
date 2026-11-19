#!/usr/bin/env python3
"""Analyze one GEM5 run, optionally paired with prefetch-off and original controls."""

import argparse
import configparser
import csv
import json
import math
from pathlib import Path

from correlate import compare_pcs, ratio, summarize
from parse_trace import (
    address,
    digest,
    export_csv,
    line_address,
    load_trace,
    require,
)
from report import write_report
from stages import source_analysis
from symbolize import symbolize_pcs


def read_stats(path, cpu):
    """Use the last complete dump, following util/tea's CI analysis convention."""
    values, blocks, complete = {}, 0, False
    with path.open() as stream:
        for line in stream:
            if "Begin Simulation Statistics" in line:
                values, complete = {}, False
            elif "End Simulation Statistics" in line:
                complete, blocks = True, blocks + 1
            else:
                fields = line.split()
                if len(fields) >= 2 and not fields[0].startswith("#"):
                    try:
                        value = float(fields[1])
                    except ValueError:
                        continue
                    if math.isfinite(value):
                        values[fields[0]] = value
    require(complete and blocks, f"{path}: no final complete statistics block")
    cycles = values.get(cpu + ".numCycles")
    retired = values.get(
        cpu + ".committedInsts", values.get(cpu + ".numInsts")
    )
    return {
        "cpu": cpu,
        "blocks": blocks,
        "cycles": cycles,
        "retired_instructions": retired,
        "ipc": ratio(retired, cycles) if retired is not None else None,
        "final_tick": values.get("finalTick"),
        "sim_ticks": values.get("simTicks"),
        "sha256": digest(path),
        "native_counters": {
            k: v
            for k, v in values.items()
            if k.endswith(
                (
                    ".pfOnlyFill",
                    ".pfMergedWithDemand",
                    ".pfDequeued",
                    ".pfIdentified",
                    ".pfRemovedFull",
                    ".pfRemovedDemand",
                    ".hits::total",
                    ".misses::total",
                )
            )
        },
    }


def reconcile_counters(db, summary):
    if not summary["trace_matches_stats_window"]:
        return {"status": "not_comparable_roi", "checks": []}
    if not summary["config"]:
        return {"status": "missing_config", "checks": []}
    config = summary["config"]["entries"]
    l1 = {
        key.removesuffix(".cache_level")
        for key, value in config.items()
        if key.endswith(".cache_level")
        and value == "1"
        and config.get(key.removesuffix(".cache_level") + ".is_read_only")
        == "false"
    }
    mappings = {
        "pfDequeued": ("actor", "PF_DEQUEUE", "1", ()),
        "pfIdentified": ("actor", "PF_IDENTIFIED", "1", ()),
        "pfRemovedFull": (
            "actor",
            "PF_DROP",
            "reason IN (?,?)",
            ("queue_full", "translation_queue_full"),
        ),
        "pfRemovedDemand": (
            "actor",
            "PF_DROP",
            "reason=?",
            ("demand_squash",),
        ),
        "pfOnlyFill": ("cache", "PF_FILL_CLASSIFY", "result=?", ("pure",)),
        "pfMergedWithDemand": (
            "cache",
            "PF_FILL_CLASSIFY",
            "result=?",
            ("merged",),
        ),
    }
    checks = []
    for name, expected in summary["stats"]["native_counters"].items():
        owner, suffix = name.rsplit(".", 1)
        if suffix in mappings:
            column, event, clause, parameters = mappings[suffix]
            actual = db.execute(
                f"SELECT count(*) FROM events WHERE {column}=? AND event=? AND {clause}",
                (owner, event, *parameters),
            ).fetchone()[0]
        elif suffix in ("hits::total", "misses::total"):
            cache, command = owner.rsplit(".", 1)
            if cache not in l1:
                continue
            outcomes = (
                ("hit", "hit")
                if suffix == "hits::total"
                else ("miss", "miss_retry")
            )
            actual = db.execute(
                "SELECT count(*) FROM events WHERE cache=? AND command=? "
                "AND event='DEMAND_ACCESS' AND result IN (?,?)",
                (cache, command, *outcomes),
            ).fetchone()[0]
        else:
            continue
        checks.append(
            {
                "counter": name,
                "trace": actual,
                "native": expected,
                "equal": actual == expected,
            }
        )
    return {
        "status": (
            "equal"
            if checks and all(c["equal"] for c in checks)
            else "mismatch"
            if checks
            else "no_counters"
        ),
        "checks": checks,
    }


def read_config(path):
    config = configparser.ConfigParser(interpolation=None)
    config.optionxform = str
    config.read(path)
    # Prefetcher objects and their wiring are the declared experimental axis.
    ignored = {
        "children",
        "prefetch_trace",
        "prefetcher",
        "prefetch_can_offload",
        "store_prefetch_train",
        "real_pf",
        "hint_downstream",
    }
    entries = {}
    for section in config.sections():
        if ".prefetcher" in section or ".prefetch_trace" in section:
            continue
        for key, value in config[section].items():
            if key not in ignored:
                if (
                    section.startswith("system.redirect_paths")
                    and key == "host_paths"
                ):
                    value = value.replace(
                        str(path.parent / "fs") + "/", "<run-fs>/"
                    )
                entries[section + "." + key] = value
    return entries


def analyze_run(path, output, cpu, weight):
    path = Path(path).resolve()
    if path.is_dir():
        folder = path / "m5out" if (path / "m5out").is_dir() else path
        config = configparser.ConfigParser(interpolation=None)
        config.read(folder / "config.ini")
        section = next(
            (
                s
                for s in config.sections()
                if config[s].get("type") == "PrefetchTrace"
            ),
            None,
        )
        filename = (
            config[section]["filename"] if section else "prefetch-trace.csv"
        )
        trace = folder / filename
    else:
        trace, folder = path, path.parent
    output.mkdir(parents=True, exist_ok=False)
    db, metadata = load_trace(trace, output / "events.sqlite")
    summary, pcs = summarize(db, metadata, weight)
    summary["run"] = str(path)
    summary["weight"] = weight
    stats_path, config_path = folder / "stats.txt", folder / "config.ini"
    summary["stats"] = (
        read_stats(stats_path, cpu) if stats_path.exists() else None
    )
    summary["config"] = (
        {"sha256": digest(config_path), "entries": read_config(config_path)}
        if config_path.exists()
        else None
    )
    begin = max(
        int(metadata["start_tick"]),
        int(
            db.execute(
                "SELECT coalesce(max(tick),0) FROM events WHERE event='ROI_BEGIN'"
            ).fetchone()[0]
        ),
    )
    end = min(int(metadata["end_tick"]), int(metadata["trace_end_tick"]))
    stats = summary["stats"]
    summary["trace_matches_stats_window"] = bool(
        stats
        and stats["final_tick"] == end
        and stats["sim_ticks"] is not None
        and stats["final_tick"] - stats["sim_ticks"] == begin
    )
    summary["observation_ticks"] = [begin, end]
    summary["counter_reconciliation"] = reconcile_counters(db, summary)
    summary["source_analysis"] = source_analysis(db)
    for name, rows, columns in (
        (
            "sources",
            summary["source_analysis"]["sources"],
            [
                "cache",
                "source",
                "candidates",
                "aliases",
                "sent",
                "useful",
                "unused_evicted",
                "censored",
                "accuracy_observed",
            ],
        ),
        (
            "source-drops",
            summary["source_analysis"]["drop_events"],
            ["cache", "source", "reason", "events"],
        ),
        (
            "stages",
            summary["source_analysis"]["phases"],
            [
                "cache",
                "source",
                "stage",
                "samples",
                "mean_ticks",
                "p50_ticks",
                "p90_ticks",
                "p99_ticks",
            ],
        ),
    ):
        with (output / (name + ".csv")).open("w", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=columns)
            writer.writeheader()
            writer.writerows(rows)
    return db, summary, pcs


def compare(off, on, prefetch_off=False):
    result = {
        "net_miss_coverage": None,
        "ipc_speedup": None,
        "mean_latency_change_ticks": None,
        "config_differences": [],
        "paired": False,
    }
    if not (off["stats"] and on["stats"] and off["config"] and on["config"]):
        result[
            "reason"
        ] = "Both runs need stats.txt and config.ini for a performance comparison."
        return result
    a, b = off["config"]["entries"], on["config"]["entries"]
    result["config_differences"] = [
        {"parameter": key, "before": a.get(key), "after": b.get(key)}
        for key in sorted(a.keys() | b.keys())
        if a.get(key) != b.get(key)
    ]
    if result["config_differences"]:
        result[
            "reason"
        ] = "Non-prefetch configuration differs; paired metrics are withheld."
        return result
    require(
        off["trace"]["block_size"] == on["trace"]["block_size"],
        "Different line sizes",
    )
    result["paired"] = True
    off_ipc, on_ipc = off["stats"]["ipc"], on["stats"]["ipc"]
    result["ipc_speedup"] = (
        on_ipc / off_ipc - 1 if off_ipc and on_ipc else None
    )
    lat_a, lat_b = (
        off["demand"]["mean_latency_ticks"],
        on["demand"]["mean_latency_ticks"],
    )
    if lat_a is not None and lat_b is not None:
        result["mean_latency_change_ticks"] = lat_b - lat_a
    retired_a, retired_b = (
        off["stats"]["retired_instructions"],
        on["stats"]["retired_instructions"],
    )
    if (
        prefetch_off
        and off["trace_matches_stats_window"]
        and on["trace_matches_stats_window"]
        and retired_a
        and retired_b
    ):
        before = off["demand"]["misses"] / retired_a
        after = on["demand"]["misses"] / retired_b
        result["net_miss_coverage"] = 1 - after / before if before else None
    elif prefetch_off:
        result[
            "reason"
        ] = "Coverage requires both demand traces to cover their entire paired statistics windows."
    result["useful_without_ipc_gain"] = bool(
        on["prefetch"]["timely_useful_requests"]
        and result["ipc_speedup"] is not None
        and result["ipc_speedup"] <= 0
    )
    return result


def export_timeline(db, output, pc=None, line=None, block_size=64):
    # Filters select seed facts. Then follow identities, including aliases,
    # trigger requests and demand consumers with a different PC.
    where, params = [], []
    if pc:
        where.append("pc=?")
        params.append(address(pc))
    if line:
        where.append("(pa_line=? OR va_line=?)")
        params.extend([line_address(line, block_size)] * 2)
    seed = " AND ".join(where) if where else "1"
    cte = f"""WITH RECURSIVE selected_pf(id) AS (
        SELECT prefetch_id FROM events WHERE {seed} AND prefetch_id>0
        UNION SELECT related_id FROM events WHERE {seed} AND event='DEMAND_MERGE' AND related_id>0
        UNION SELECT matched_pf FROM demands WHERE {seed} AND matched_pf IS NOT NULL
        UNION SELECT p.alias_id FROM prefetches p JOIN selected_pf s ON p.id=s.id WHERE p.alias_id IS NOT NULL
        UNION SELECT p.id FROM prefetches p JOIN selected_pf s ON p.alias_id=s.id
    ), selected_requests(id) AS (
        SELECT request_id FROM events WHERE {seed} AND request_id>0
        UNION SELECT trigger_id FROM prefetches WHERE id IN (SELECT id FROM selected_pf)
        UNION SELECT request_id FROM events WHERE event='PF_FIRST_USE' AND prefetch_id IN (SELECT id FROM selected_pf)
        UNION SELECT request_id FROM events WHERE event='DEMAND_MERGE' AND related_id IN (SELECT id FROM selected_pf)
    ) """
    parameters = params * 4
    selection = """SELECT * FROM events WHERE
        prefetch_id IN (SELECT id FROM selected_pf) OR
        (prefetch_id=0 AND request_id IN (SELECT id FROM selected_requests))"""
    query = cte + selection + ' ORDER BY tick,"order"'
    export_csv(db, query, output / "timeline.csv", parameters)
    preview = [dict(r) for r in db.execute(query + " LIMIT 2000", parameters)]
    total = db.execute(
        cte + "SELECT count(*) FROM (" + selection + ")", parameters
    ).fetchone()[0]
    return {
        "events": preview,
        "total": total,
        "preview_limit": 2000,
        "scope": {"pc": pc, "line": line},
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "run", type=Path, help="Run directory, m5out or trace CSV"
    )
    parser.add_argument(
        "--output", type=Path, required=True, help="New analysis directory"
    )
    parser.add_argument(
        "--baseline",
        type=Path,
        help="Matched prefetch-off run (with demand trace)",
    )
    parser.add_argument(
        "--control", type=Path, help="Matched original prefetch-on run"
    )
    parser.add_argument(
        "--cpu", default="system.cpu", help="CPU statistics prefix"
    )
    parser.add_argument(
        "--weight",
        type=float,
        default=1.0,
        help="Workload weight for PC latency ranking",
    )
    parser.add_argument(
        "--pc", help="Select a trigger/demand PC for the timeline"
    )
    parser.add_argument(
        "--line",
        help="Select a physical or context-qualified virtual line for the timeline",
    )
    parser.add_argument(
        "--elf", type=Path, help="Matching workload ELF for PC symbols"
    )
    parser.add_argument(
        "--addr2line",
        default="riscv64-linux-gnu-addr2line",
        help="Symbol resolver executable",
    )
    args = parser.parse_args()
    require(
        math.isfinite(args.weight) and args.weight > 0,
        "Weight must be finite and positive",
    )
    db, summary, pcs = analyze_run(
        args.run, args.output, args.cpu, args.weight
    )
    if args.elf:
        summary["symbolization"] = symbolize_pcs(pcs, args.elf, args.addr2line)
    runs = {"candidate": summary}
    pc_runs = {"candidate": pcs}
    for label, path in (
        ("baseline", args.baseline),
        ("control", args.control),
    ):
        if path:
            other_db, other, other_pcs = analyze_run(
                path, args.output / label, args.cpu, args.weight
            )
            if args.elf:
                other["symbolization"] = symbolize_pcs(
                    other_pcs, args.elf, args.addr2line
                )
            (args.output / label / "summary.json").write_text(
                json.dumps(other, indent=2) + "\n"
            )
            other_db.close()
            runs[label] = other
            pc_runs[label] = other_pcs
    if "baseline" in runs:
        require(
            runs["baseline"]["prefetch"]["candidates"] == 0
            and runs["baseline"]["prefetch"]["sent_requests"] == 0,
            "--baseline must be a prefetch-off run",
        )
        summary["versus_prefetch_off"] = compare(
            runs["baseline"], summary, prefetch_off=True
        )
    if "control" in runs:
        summary["versus_original"] = compare(runs["control"], summary)
    pc_deltas = []
    reference = (
        "control"
        if "control" in runs
        else "baseline"
        if "baseline" in runs
        else None
    )
    comparison = summary.get(
        "versus_original" if reference == "control" else "versus_prefetch_off",
        {},
    )
    if (
        reference
        and comparison.get("paired")
        and summary["trace_matches_stats_window"]
        and runs[reference]["trace_matches_stats_window"]
        and summary["stats"]["retired_instructions"]
        and runs[reference]["stats"]["retired_instructions"]
    ):
        pc_deltas = compare_pcs(
            pc_runs[reference],
            pcs,
            runs[reference]["stats"]["retired_instructions"],
            summary["stats"]["retired_instructions"],
            args.weight,
        )
    summary["pc_comparison_reference"] = reference
    with (args.output / "pc-comparison.csv").open("w", newline="") as stream:
        columns = (
            list(pc_deltas[0])
            if pc_deltas
            else ["cache", "pc", "weighted_wait_delta_ticks_per_inst"]
        )
        writer = csv.DictWriter(stream, fieldnames=columns)
        writer.writeheader()
        writer.writerows(pc_deltas)
    timeline_pc = args.pc or (pcs[0]["pc"] if pcs and not args.line else None)
    timeline = export_timeline(
        db,
        args.output,
        timeline_pc,
        args.line,
        int(summary["trace"]["block_size"]),
    )
    export_csv(
        db,
        "SELECT * FROM prefetches ORDER BY id",
        args.output / "prefetches.csv",
    )
    export_csv(
        db,
        'SELECT * FROM demands ORDER BY tick,"order"',
        args.output / "demands.csv",
    )
    with (args.output / "pcs.csv").open("w", newline="") as stream:
        columns = (
            list(pcs[0]) if pcs else ["cache", "pc", "accesses", "misses"]
        )
        writer = csv.DictWriter(stream, fieldnames=columns)
        writer.writeheader()
        writer.writerows(
            {
                k: json.dumps(v) if isinstance(v, (dict, list)) else v
                for k, v in pc.items()
            }
            for pc in pcs
        )
    with (args.output / "performance.csv").open("w", newline="") as stream:
        columns = [
            "variant",
            "cycles",
            "retired_instructions",
            "ipc",
            "misses",
            "prefetch_bytes",
            "weight",
        ]
        writer = csv.DictWriter(stream, fieldnames=columns)
        writer.writeheader()
        for label, run in runs.items():
            stats = run["stats"] or {}
            writer.writerow(
                {
                    "variant": label,
                    **{k: stats.get(k) for k in columns[1:4]},
                    "misses": run["demand"]["misses"],
                    "prefetch_bytes": run["prefetch"][
                        "prefetch_request_bytes"
                    ],
                    "weight": args.weight,
                }
            )
    (args.output / "audit.json").write_text(
        json.dumps(
            {
                "trace": summary["trace"],
                "config_sha256": summary["config"]["sha256"]
                if summary["config"]
                else None,
                "stats_sha256": summary["stats"]["sha256"]
                if summary["stats"]
                else None,
                "counter_reconciliation": summary["counter_reconciliation"],
                "demand_metadata": dict(
                    zip(
                        [
                            "lookups",
                            "pc",
                            "vaddr",
                            "paddr",
                            "inst_seq",
                            "address_space",
                            "arrival_tick",
                        ],
                        db.execute(
                            "SELECT count(*),sum(pc IS NOT NULL),sum(vaddr IS NOT NULL),"
                            "sum(paddr IS NOT NULL),sum(inst_seq IS NOT NULL),"
                            "sum(address_space<>''),sum(arrival_tick IS NOT NULL) "
                            "FROM events WHERE event='DEMAND_ACCESS'"
                        ).fetchone(),
                    )
                ),
                "prefetch_events_without_identity": db.execute(
                    "SELECT count(*) FROM events WHERE event LIKE 'PF_%' "
                    "AND event NOT IN ('PF_TRAIN','PF_TRAIN_DROP','PF_OBSERVE') "
                    "AND prefetch_id=0"
                ).fetchone()[0],
                "miss_categories_conserve": sum(
                    summary["demand"]["failures"].values()
                )
                == summary["demand"]["misses"],
                "prefetch_outcomes_conserve": summary["prefetch"][
                    "sent_requests"
                ]
                == sum(
                    summary["prefetch"][key]
                    for key in (
                        "useful_requests",
                        "unused_evicted_requests",
                        "right_censored_sent_requests",
                    )
                ),
            },
            indent=2,
        )
        + "\n"
    )
    (args.output / "summary.json").write_text(
        json.dumps(summary, indent=2) + "\n"
    )
    write_report(args.output, summary, pcs, timeline, runs, pc_deltas)
    db.close()
    print(args.output.resolve() / "report.html")


if __name__ == "__main__":
    main()
