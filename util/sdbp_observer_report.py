#!/usr/bin/env python3
# Copyright (c) 2026 Institute of Computing Technology, CAS
# SPDX-License-Identifier: BSD-3-Clause

"""Summarize full-cache SDBP observations from the last gem5 stats dump."""

import argparse
import json
import math
from pathlib import Path


SOURCES = ("l1Demand", "l1Prefetch", "l2Prefetch")


def read_stats(path):
    stats = {}
    for line in path.read_text().splitlines():
        if "Begin Simulation Statistics" in line:
            stats = {}
        fields = line.split()
        if len(fields) >= 2:
            try:
                stats[fields[0]] = float(fields[1])
            except ValueError:
                pass
    return stats


def ratio(numerator, denominator):
    return numerator / denominator if denominator else None


def summarize(stats, prefixes, sources):
    def count(name):
        return sum(
            stats.get(f"{prefix}.{name}::{source}", 0)
            for prefix in prefixes
            for source in sources
        )

    tp = count("fullCacheDeadTruePositiveBySource")
    fp = count("fullCacheDeadFalsePositiveBySource")
    tn = count("fullCacheLiveTrueNegativeBySource")
    fn = count("fullCacheLiveFalseNegativeBySource")
    resolved = count("fullCacheResolvedBySource")
    predictions = count("fullCachePredictionsBySource")
    carried = count("fullCacheCarriedInBySource")
    censored = count("fullCacheCensoredBySource")
    pending = count("fullCachePendingBySource")
    if tp + fp + tn + fn != resolved:
        raise ValueError(
            "Confusion matrix does not sum to resolved predictions"
        )
    if predictions + carried != resolved + censored + pending:
        raise ValueError("Prediction lifecycle accounting does not balance")

    fills = count("fullCachePrefetchFillsBySource")
    pf_carried = count("fullCachePrefetchCarriedInBySource")
    used = count("fullCachePrefetchDemandUsesBySource")
    unused = count("fullCachePrefetchUnusedEvictionsBySource")
    pf_censored = count("fullCachePrefetchUnusedInvalidationsBySource")
    pf_pending = count("fullCachePrefetchPendingBySource")
    if fills + pf_carried != used + unused + pf_censored + pf_pending:
        raise ValueError("Prefetch demand-use accounting does not balance")
    dead_recall = ratio(tp, tp + fn)
    live_recall = ratio(tn, tn + fp)
    return {
        "predictions": predictions,
        "carried_in": carried,
        "resolved": resolved,
        "censored": censored,
        "pending": pending,
        "tp_dead": tp,
        "fp_dead": fp,
        "tn_live": tn,
        "fn_live": fn,
        "accuracy": ratio(tp + tn, resolved),
        "dead_precision": ratio(tp, tp + fp),
        "dead_recall": dead_recall,
        "live_precision": ratio(tn, tn + fn),
        "live_recall": live_recall,
        "balanced_accuracy": (
            (dead_recall + live_recall) / 2
            if dead_recall is not None and live_recall is not None
            else None
        ),
        "always_live_accuracy": ratio(tn + fp, resolved),
        "actual_dead_fraction": ratio(tp + fn, resolved),
        "predicted_dead_fraction": ratio(tp + fp, resolved),
        "resolved_fraction": ratio(resolved, predictions + carried),
        "censored_fraction": ratio(censored, predictions + carried),
        "pending_fraction": ratio(pending, predictions + carried),
        "demand_reuses": count("fullCacheDemandReuseBySource"),
        "prefetch_reuses": count("fullCachePrefetchReuseBySource"),
        "predicted_dead_demand_reuses": count(
            "fullCachePredDeadDemandReuseBySource"
        ),
        "demand_reuse_false_dead_rate": ratio(
            count("fullCachePredDeadDemandReuseBySource"),
            count("fullCacheDemandReuseBySource"),
        ),
        "prefetch_fills": fills,
        "prefetch_carried_in": pf_carried,
        "prefetch_demand_used": used,
        "prefetch_unused_evictions": unused,
        "prefetch_unused_invalidations": pf_censored,
        "prefetch_pending_first_demand": pf_pending,
        "prefetch_demand_use_rate_resolved": ratio(used, used + unused),
    }


def report(stats):
    suffixes = (
        ".fullCachePredictionsBySource::",
        ".fullCacheCarriedInBySource::",
        ".fullCacheResolvedBySource::",
    )
    prefixes = sorted(
        {
            key.split(suffix)[0]
            for key in stats
            for suffix in suffixes
            if suffix in key
        }
    )
    if not prefixes:
        raise ValueError("No full-cache SDBP observer statistics found")

    def groups(selected):
        return {
            **{
                source: summarize(stats, selected, (source,))
                for source in SOURCES
            },
            "prefetch": summarize(stats, selected, SOURCES[1:]),
            "all": summarize(stats, selected, SOURCES),
        }

    return {
        "label": "next eligible demand/prefetch reuse before LRU replacement",
        "positive_class": "dead",
        "grouping": "source of the stored prediction, not the reusing request",
        "aggregate": groups(prefixes),
        "policies": {prefix: groups((prefix,)) for prefix in prefixes},
    }


def compare_baseline(stats, baseline):
    differences = {}
    compared = 0
    for key, value in baseline.items():
        if ".replacement_policy." in key:
            continue
        if key not in ("simTicks", "simInsts") and not key.startswith(
            "system."
        ):
            continue
        other = stats.get(key, 0)
        # Undefined formula values (e.g. empty denominators) compare equal.
        if value != value and other != other:
            continue
        compared += 1
        if value != other:
            differences[key] = {
                "lru": value if math.isfinite(value) else str(value),
                "observer": other if math.isfinite(other) else str(other),
            }
    return {
        "compared": compared,
        "equal": not differences,
        "differences": differences,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stats", type=Path)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    stats = read_stats(args.stats)
    result = report(stats)
    if args.baseline:
        result["lru_comparison"] = compare_baseline(
            stats, read_stats(args.baseline)
        )
    text = json.dumps(result, indent=2, allow_nan=False) + "\n"
    if args.output:
        args.output.write_text(text)
    else:
        print(text, end="")
    if args.baseline and not result["lru_comparison"]["equal"]:
        raise SystemExit("Observer changed baseline simulation statistics")


if __name__ == "__main__":
    main()
