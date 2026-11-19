#!/usr/bin/env python3

import argparse
import json
import pathlib
import re
import sqlite3
import sys
from collections import Counter
from dataclasses import asdict, replace
from typing import Dict, Iterable, List, Mapping, Optional, Sequence, Tuple

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from util.ts_bop_degree.analyze import (
    BopReplay,
    PBOP_OFFSETS,
    ResourceConfig,
    SchemeTracker,
    StudentEntry,
    VBOP_OFFSETS,
    iter_accesses,
    merged_config,
    read_clock_period,
    read_marker,
)


VBOP_SET = set(VBOP_OFFSETS)
PBOP_SET = set(PBOP_OFFSETS)
BASELINE_SCHEME = "merged_d2_greedy_marginal_th2_top1"
RESOURCE_NAME = "f256_p8e4_3to1"
RESOURCE_CONFIG = ResourceConfig(
    name=RESOURCE_NAME,
    primary_fifo_size=8,
    extra_fifo_size=4,
    primary_slots=3,
    extra_slots=1,
    recent_filter_entries=256,
)


def load_json(path: pathlib.Path) -> Dict:
    return json.loads(path.read_text())


def find_scheme(document: Mapping, name: str) -> Mapping:
    return next(item for item in document["schemes"] if item["name"] == name)


def find_resource(scheme: Mapping, name: str) -> Mapping:
    return next(item for item in scheme.get("resources", []) if item["name"] == name)


def offset_family(offset: int) -> str:
    if offset in VBOP_SET:
        return "vbop"
    if offset in PBOP_SET:
        return "pbop"
    raise ValueError(f"offset {offset} is not in a historical BOP family")


def parse_split(value: str) -> Tuple[int, int]:
    try:
        vbop_slots, pbop_slots = (int(item) for item in value.split(":"))
    except (ValueError, TypeError) as error:
        raise argparse.ArgumentTypeError("split must be VBOP_SLOTS:PBOP_SLOTS") from error
    if vbop_slots <= 0 or pbop_slots <= 0:
        raise argparse.ArgumentTypeError("partition sizes must be positive")
    return vbop_slots, pbop_slots


def parse_reservation(value: str) -> Tuple[int, int, int]:
    try:
        vbop_min, pbop_min, total_slots = (
            int(item) for item in value.split(":")
        )
    except (ValueError, TypeError) as error:
        raise argparse.ArgumentTypeError(
            "reservation must be VBOP_MIN:PBOP_MIN:TOTAL_SLOTS"
        ) from error
    if vbop_min <= 0 or pbop_min <= 0:
        raise argparse.ArgumentTypeError("family reservations must be positive")
    if total_slots < vbop_min + pbop_min:
        raise argparse.ArgumentTypeError(
            "total slots must cover both family reservations"
        )
    return vbop_min, pbop_min, total_slots


class PartitionedBopReplay(BopReplay):
    def __init__(
        self,
        clock_period_ticks: int,
        vbop_slots: int,
        pbop_slots: int,
        selection_policy: str,
        shared_slots: int = 0,
    ):
        if selection_policy not in ("independent", "max_union"):
            raise ValueError(f"unsupported selection policy: {selection_policy}")
        if shared_slots < 0:
            raise ValueError("shared slots must be non-negative")
        total_slots = vbop_slots + pbop_slots + shared_slots
        pool_name = (
            f"PartitionedV{vbop_slots}P{pbop_slots}"
            if shared_slots == 0
            else f"ReservedV{vbop_slots}P{pbop_slots}T{total_slots}"
        )
        config = replace(
            merged_config(2, "greedy_marginal", 0.02, 2),
            name=f"{pool_name}_{selection_policy}",
            student_pool_size=total_slots,
            teacher_top_n=2,
        )
        super().__init__(config, clock_period_ticks)
        self.partition_slots = {
            "vbop": vbop_slots,
            "pbop": pbop_slots,
        }
        self.shared_slots = shared_slots
        self.total_slots = total_slots
        self.selection_policy = selection_policy

    def _entry_rank_key(self, index: int) -> Tuple[float, float, int, int]:
        entry = self.student_pool[index]
        return (
            entry.cur_cov,
            entry.conf,
            -abs(entry.offset),
            -entry.offset,
        )

    def _family_indices(self, family: str) -> List[int]:
        return [
            index
            for index, entry in enumerate(self.student_pool)
            if offset_family(entry.offset) == family
        ]

    def _rank_teacher_winners(self) -> List[int]:
        primary_winner = self.phase_best_offset
        if primary_winner == 0:
            return []
        primary_family = offset_family(primary_winner)
        other_family = "pbop" if primary_family == "vbop" else "vbop"
        ranked_other = sorted(
            (
                index
                for index, score in enumerate(self.scores)
                if score > 0
                and offset_family(self.cfg.offsets[index]) == other_family
            ),
            key=lambda index: (-self.scores[index], index),
        )
        winners = [primary_winner]
        if ranked_other:
            winners.append(self.cfg.offsets[ranked_other[0]])
        self.stats[f"teacher_family_winner_count::{len(winners)}"] += 1
        return winners

    def _insert_teacher_winners(self, offsets: Sequence[int]) -> None:
        for offset in offsets:
            if offset == 0 or any(
                entry.offset == offset for entry in self.student_pool
            ):
                continue
            family = offset_family(offset)
            family_indices = self._family_indices(family)
            if self.shared_slots == 0 and (
                len(family_indices) >= self.partition_slots[family]
            ):
                evict_index = min(
                    family_indices,
                    key=self._eviction_rank_key,
                )
                del self.student_pool[evict_index]
                self.stats[f"partition_evict::{family}"] += 1
            elif len(self.student_pool) >= self.total_slots:
                family_counts = {
                    name: len(self._family_indices(name))
                    for name in ("vbop", "pbop")
                }
                evictable = []
                for index, entry in enumerate(self.student_pool):
                    entry_family = offset_family(entry.offset)
                    if entry_family == family or (
                        family_counts[entry_family]
                        > self.partition_slots[entry_family]
                    ):
                        evictable.append(index)
                if not evictable:
                    raise RuntimeError(
                        "reserved pool has no legal eviction candidate"
                    )
                evict_index = min(evictable, key=self._eviction_rank_key)
                evicted_family = offset_family(
                    self.student_pool[evict_index].offset
                )
                del self.student_pool[evict_index]
                self.stats[f"partition_evict::{evicted_family}"] += 1
            self.student_pool.append(StudentEntry(offset=offset))
            self.stats["teacher_injected"] += 1
            self.stats[f"teacher_injected_offset::{offset}"] += 1
            self.stats[f"partition_injected::{family}"] += 1

    def _eviction_rank_key(self, index: int) -> Tuple[float, int, int, int]:
        entry = self.student_pool[index]
        return (
            entry.conf,
            entry.last_cov,
            -abs(entry.offset),
            entry.offset,
        )

    def _best_family_index(self, family: str) -> Optional[int]:
        indices = self._family_indices(family)
        return max(indices, key=self._entry_rank_key) if indices else None

    def _pair_union(self, lhs: int, rhs: int) -> int:
        selected_mask = (1 << lhs) | (1 << rhs)
        return sum(
            count
            for mask, count in enumerate(self.student_mask_hist)
            if count and (mask & selected_mask)
        )

    def _independent_pair(self) -> Optional[Tuple[int, int]]:
        vbop_index = self._best_family_index("vbop")
        pbop_index = self._best_family_index("pbop")
        if vbop_index is None or pbop_index is None:
            return None
        return vbop_index, pbop_index

    def _max_union_pair(self) -> Optional[Tuple[int, int]]:
        vbop_indices = self._family_indices("vbop")
        pbop_indices = self._family_indices("pbop")
        if not vbop_indices or not pbop_indices:
            return None
        pairs = []
        for vbop_index in vbop_indices:
            for pbop_index in pbop_indices:
                vbop_entry = self.student_pool[vbop_index]
                pbop_entry = self.student_pool[pbop_index]
                pairs.append((
                    self._pair_union(vbop_index, pbop_index),
                    max(vbop_entry.cur_cov, pbop_entry.cur_cov),
                    vbop_entry.cur_cov + pbop_entry.cur_cov,
                    vbop_entry.conf + pbop_entry.conf,
                    -abs(vbop_entry.offset) - abs(pbop_entry.offset),
                    -vbop_entry.offset - pbop_entry.offset,
                    vbop_index,
                    pbop_index,
                ))
        *_, vbop_index, pbop_index = max(pairs)
        return vbop_index, pbop_index

    def _select_student_offsets(self) -> Tuple[List[int], List[int]]:
        if not self.student_pool:
            return [], []
        pair = (
            self._independent_pair()
            if self.selection_policy == "independent"
            else self._max_union_pair()
        )
        if pair is None:
            primary_index = max(
                range(len(self.student_pool)),
                key=self._entry_rank_key,
            )
            return (
                [self.student_pool[primary_index].offset],
                [self.student_pool[primary_index].cur_cov],
            )

        primary_index, extra_index = sorted(
            pair,
            key=self._entry_rank_key,
            reverse=True,
        )
        primary_coverage = self.student_pool[primary_index].cur_cov
        marginal = self._marginal_coverage(
            extra_index,
            1 << primary_index,
        )
        ratio = (
            marginal / self.student_phase_train_count
            if self.student_phase_train_count
            else 0.0
        )
        offsets = [self.student_pool[primary_index].offset]
        contributions = [primary_coverage]
        if ratio >= self.cfg.extra_threshold:
            offsets.append(self.student_pool[extra_index].offset)
            contributions.append(marginal)
        return offsets, contributions

    def _student_phase_end(self, teacher_winners: Sequence[int]) -> None:
        super()._student_phase_end(teacher_winners)
        record = self.phase_records[-1]
        pool_families = {
            offset_family(offset) for offset in record["pool"]
        }
        next_pool_families = {
            offset_family(offset) for offset in record["next_pool"]
        }
        if pool_families == {"vbop", "pbop"}:
            self.stats["partition_phase_both"] += 1
        if next_pool_families == {"vbop", "pbop"}:
            self.stats["partition_next_pool_both"] += 1
        selected = record["selected_offsets"]
        if len(selected) == 2:
            self.stats["partition_selected_pair"] += 1
            if offset_family(selected[0]) != offset_family(selected[1]):
                self.stats["partition_selected_cross_family"] += 1


def scheme_name(
    vbop_slots: int,
    pbop_slots: int,
    policy: str,
    shared_slots: int = 0,
) -> str:
    if shared_slots:
        total_slots = vbop_slots + pbop_slots + shared_slots
        return (
            f"reserve_v{vbop_slots}_p{pbop_slots}_t{total_slots}_{policy}"
        )
    return f"partition_v{vbop_slots}_p{pbop_slots}_{policy}"


def reference_results(result_dir: pathlib.Path) -> Dict:
    shortlist = load_json(result_dir / "shortlist_corrected.json")
    resources = load_json(result_dir / "resources_filter.json")
    dual_resources = load_json(result_dir / "resources_dual.json")
    dual = find_scheme(shortlist, "dual_current")
    baseline = find_scheme(resources, BASELINE_SCHEME)
    dual_local = find_resource(
        find_scheme(dual_resources, "dual_current"),
        "dual_p16e16_strict",
    )
    baseline_local = find_resource(baseline, RESOURCE_NAME)
    return {
        "dual": {
            "metrics": dual["metrics"],
            "local_metrics": dual_local["metrics"],
        },
        "merged_baseline": {
            "metrics": baseline["metrics"],
            "local_metrics": baseline_local["metrics"],
        },
    }


def run_checkpoint(
    entry: Mapping,
    splits: Sequence[Tuple[int, int]],
    reservations: Sequence[Tuple[int, int, int]],
    policies: Sequence[str],
) -> Dict:
    result_dir = pathlib.Path(entry["result_dir"])
    db_path = result_dir / "trace.db"
    clock_period_ticks = read_clock_period(db_path, None)
    connection = sqlite3.connect(str(db_path))
    marker_tick, marker_seq = read_marker(connection)
    trackers: Dict[str, SchemeTracker] = {}
    for vbop_slots, pbop_slots in splits:
        for policy in policies:
            name = scheme_name(vbop_slots, pbop_slots, policy)
            replay = PartitionedBopReplay(
                clock_period_ticks,
                vbop_slots,
                pbop_slots,
                policy,
            )
            tracker = SchemeTracker(name, [replay])
            tracker.attach_resources(
                [RESOURCE_CONFIG],
                clock_period_ticks,
                marker_tick,
                marker_seq,
            )
            trackers[name] = tracker
    for vbop_min, pbop_min, total_slots in reservations:
        shared_slots = total_slots - vbop_min - pbop_min
        for policy in policies:
            name = scheme_name(
                vbop_min,
                pbop_min,
                policy,
                shared_slots,
            )
            replay = PartitionedBopReplay(
                clock_period_ticks,
                vbop_min,
                pbop_min,
                policy,
                shared_slots,
            )
            tracker = SchemeTracker(name, [replay])
            tracker.attach_resources(
                [RESOURCE_CONFIG],
                clock_period_ticks,
                marker_tick,
                marker_seq,
            )
            trackers[name] = tracker

    access_count = 0
    for access in iter_accesses(connection):
        access_count += 1
        measuring = access.seq > marker_seq
        for tracker in trackers.values():
            tracker.process(access, measuring)
        if access.seq == marker_seq:
            for tracker in trackers.values():
                tracker.mark_boundary()
    connection.close()

    schemes = {}
    for name, tracker in trackers.items():
        replay = tracker.replays[0]
        schemes[name] = {
            "config": {
                "vbop_slots": replay.partition_slots["vbop"],
                "pbop_slots": replay.partition_slots["pbop"],
                "shared_slots": replay.shared_slots,
                "total_slots": replay.total_slots,
                "selection_policy": replay.selection_policy,
                "extra_threshold": replay.cfg.extra_threshold,
            },
            "metrics": tracker.metrics.to_dict(),
            "local_metrics": tracker.resource_results()[0]["metrics"],
            "measurement_stats": tracker.replay_stats()[
                replay.cfg.name
            ]["measurement"],
        }
    references = reference_results(result_dir)
    return {
        "name": entry["name"],
        "workload": entry["workload"],
        "weight": float(entry["weight"]),
        "result_dir": str(result_dir),
        "access_count": access_count,
        "marker": {"tick": marker_tick, "access_seq": marker_seq},
        "references": references,
        "schemes": schemes,
    }


def aggregate_metric(
    checkpoints: Sequence[Mapping],
    selector,
    numerator: str,
    denominator: str,
) -> Dict[str, float]:
    weight_sum = sum(float(item["weight"]) for item in checkpoints)
    checkpoint_weighted = sum(
        float(item["weight"])
        * selector(item)[numerator]
        / selector(item)[denominator]
        for item in checkpoints
    ) / weight_sum
    total_numerator = sum(selector(item)[numerator] for item in checkpoints)
    total_denominator = sum(selector(item)[denominator] for item in checkpoints)
    return {
        "checkpoint_weighted": checkpoint_weighted,
        "access_weighted": (
            total_numerator / total_denominator
            if total_denominator
            else 0.0
        ),
    }


def aggregate(checkpoints: Sequence[Mapping]) -> Dict:
    if not checkpoints:
        return {}
    result: Dict = {"references": {}, "schemes": {}}
    for reference in ("dual", "merged_baseline"):
        selector = lambda item, name=reference: item["references"][name]["metrics"]
        local_selector = lambda item, name=reference: item["references"][name]["local_metrics"]
        result["references"][reference] = {
            "ideal_coverage": aggregate_metric(
                checkpoints, selector, "covered_count", "demand_count"
            ),
            "generated_per_demand": aggregate_metric(
                checkpoints, selector, "generated_count", "demand_count"
            ),
            "local_coverage": aggregate_metric(
                checkpoints, local_selector, "covered_count", "demand_count"
            ),
            "local_sent_per_demand": aggregate_metric(
                checkpoints,
                lambda item, select=local_selector: {
                    "sent_count": sum(
                        select(item)["sent_by_role"].values()
                    ),
                    "demand_count": select(item)["demand_count"],
                },
                "sent_count",
                "demand_count",
            ),
        }
    scheme_names = sorted(checkpoints[0]["schemes"])
    for name in scheme_names:
        selector = lambda item, scheme=name: item["schemes"][scheme]["metrics"]
        local_selector = lambda item, scheme=name: item["schemes"][scheme]["local_metrics"]
        result["schemes"][name] = {
            "config": checkpoints[0]["schemes"][name]["config"],
            "ideal_coverage": aggregate_metric(
                checkpoints, selector, "covered_count", "demand_count"
            ),
            "generated_per_demand": aggregate_metric(
                checkpoints, selector, "generated_count", "demand_count"
            ),
            "local_coverage": aggregate_metric(
                checkpoints, local_selector, "covered_count", "demand_count"
            ),
            "local_sent_per_demand": aggregate_metric(
                checkpoints,
                lambda item, select=local_selector: {
                    "sent_count": sum(
                        select(item)["sent_by_role"].values()
                    ),
                    "demand_count": select(item)["demand_count"],
                },
                "sent_count",
                "demand_count",
            ),
        }
    return result


def write_result(
    path: pathlib.Path,
    manifest: pathlib.Path,
    checkpoints: Sequence[Mapping],
    splits: Sequence[Tuple[int, int]],
    reservations: Sequence[Tuple[int, int, int]],
    policies: Sequence[str],
) -> None:
    result = {
        "manifest": str(manifest),
        "definition": (
            "Static trace replay with one merged Teacher, per-family Teacher "
            "winner output, strict or minimum-reserved Student partitions, "
            "and one selected offset from each historical family"
        ),
        "splits": [list(item) for item in splits],
        "reservations": [list(item) for item in reservations],
        "policies": list(policies),
        "checkpoints": list(checkpoints),
        "aggregate": aggregate(checkpoints),
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Replay partitioned TS-BOP Student pools from ArchDB traces"
    )
    parser.add_argument(
        "--manifest",
        type=pathlib.Path,
        default=pathlib.Path("util/ts_bop_degree/checkpoints.json"),
    )
    parser.add_argument("--out-json", required=True, type=pathlib.Path)
    parser.add_argument(
        "--split",
        action="append",
        type=parse_split,
        default=[],
        help="VBOP_SLOTS:PBOP_SLOTS; may be repeated",
    )
    parser.add_argument(
        "--policy",
        action="append",
        choices=("independent", "max_union"),
        default=[],
    )
    parser.add_argument(
        "--reserve",
        action="append",
        type=parse_reservation,
        default=[],
        help=(
            "VBOP_MIN:PBOP_MIN:TOTAL_SLOTS; remaining slots are shared; "
            "may be repeated"
        ),
    )
    parser.add_argument("--checkpoint-regex")
    parser.add_argument("--resume", action="store_true")
    args = parser.parse_args()

    splits = args.split
    reservations = args.reserve
    if not splits and not reservations:
        splits = [(2, 2)]
    policies = args.policy or ["independent", "max_union"]
    manifest = load_json(args.manifest)
    pattern = re.compile(args.checkpoint_regex) if args.checkpoint_regex else None
    selected = [
        entry for entry in manifest
        if pattern is None or pattern.search(entry["name"])
    ]
    if not selected:
        raise ValueError("checkpoint filter selected no entries")

    completed: Dict[str, Mapping] = {}
    if args.resume and args.out_json.exists():
        previous = load_json(args.out_json)
        completed = {
            item["name"]: item for item in previous.get("checkpoints", [])
        }

    results: List[Mapping] = []
    for entry in selected:
        if entry["name"] in completed:
            result = completed[entry["name"]]
        else:
            result = run_checkpoint(entry, splits, reservations, policies)
        results.append(result)
        write_result(
            args.out_json,
            args.manifest,
            results,
            splits,
            reservations,
            policies,
        )
        summaries = {
            name: {
                "ideal_coverage": item["metrics"]["coverage"],
                "local_coverage": item["local_metrics"]["coverage"],
            }
            for name, item in result["schemes"].items()
        }
        print(json.dumps({
            "checkpoint": result["name"],
            "schemes": summaries,
        }, sort_keys=True), flush=True)


if __name__ == "__main__":
    main()
