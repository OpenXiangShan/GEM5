#!/usr/bin/env python3
"""Check a complete Hybrid ROB,CommitRate trace against config and stats.

Example:
    python3 src/cpu/o3/hybrid_trace_check.py /tmp/hybrid-run/rob.log

The sibling config.ini and stats.txt are used by default. Enable ROB and
CommitRate debug flags from tick zero and do not reset statistics during the
run. Retire counts refer to successful DynInst removals, independently of
gem5's architectural committedInsts convention.
"""

import argparse
from collections import Counter, deque
import configparser
from itertools import islice
import json
from pathlib import Path
import re


def check_trace(trace, config_path, stats_path, cpu="system.cpu",
                rates_only=False):
    config = configparser.ConfigParser(interpolation=None)
    config.optionxform = str
    config.read(config_path)
    params = config[cpu]
    assert params["RobCompressPolicy"] == "hybrid"
    inst_limit = int(params["commitInstWidth"])
    group_limit = int(params["commitWidth"])
    capacity = int(params["numROBEntries"])
    length_limit = int(params["CROB_instPerGroup"])

    stats = {}
    for line in Path(stats_path).read_text().splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[0].startswith(cpu + "."):
            stats[fields[0]] = fields[1]

    prefix = re.compile(r"^\s*(\d+): " + re.escape(cpu) +
                        r"\.(rob|commit): (.*)$")
    allocation = re.compile(r"Hybrid allocated group type=(\d+) length=(\d+)")
    invariant = re.compile(r"Hybrid invariant groups=(\d+) "
                           r"dynInsts=(\d+) sum=(\d+)")
    groups = deque()
    allocated_types = Counter()
    allocated_lengths = Counter()
    retire_per_tick = Counter()
    commit_rates = {}
    visited = {}
    window_budget = {}
    drained_per_tick = Counter()
    remaining_after_last_retire = {}
    next_group = 0
    invariant_checks = 0
    squashed = 0

    with Path(trace).open() as source:
        for lineno, line in enumerate(source, 1):
            match = prefix.match(line)
            if not match:
                continue
            tick, component, message = match.groups()
            tick = int(tick)
            if component == "commit":
                if message.isdigit():
                    assert tick not in commit_rates, (lineno, tick)
                    commit_rates[tick] = int(message)
                continue
            if match := allocation.fullmatch(message):
                kind, length = map(int, match.groups())
                assert 0 <= kind < 6 and 1 <= length <= length_limit
                allocated_types[kind] += 1
                allocated_lengths[length] += 1
                groups.append([next_group, length])
                next_group += 1
                assert len(groups) <= capacity, (lineno, len(groups))
            elif match := invariant.fullmatch(message):
                count, dyninsts, total = map(int, match.groups())
                assert count <= capacity and dyninsts == total, lineno
                invariant_checks += 1
            elif "Retiring head instruction," in message or \
                    "Draining squashed head instruction," in message:
                assert groups, lineno
                if tick not in window_budget:
                    window_budget[tick] = sum(
                        item[1] for item in islice(groups, group_limit))
                group, length = groups[0]
                visited.setdefault(tick, set()).add(group)
                if "Retiring head instruction," in message:
                    retire_per_tick[tick] += 1
                    remaining_after_last_retire[tick] = length - 1
                else:
                    squashed += 1
                    drained_per_tick[tick] += 1
                groups[0][1] -= 1
                if not groups[0][1]:
                    groups.popleft()
            elif "Squashing instruction PC " in message:
                assert groups, lineno
                groups[-1][1] -= 1
                squashed += 1
                if not groups[-1][1]:
                    groups.pop()

    assert commit_rates
    if rates_only:
        max_retire = max(commit_rates.values())
        full_cycles = (sum(count == inst_limit
                           for count in commit_rates.values())
                       if inst_limit else 0)
        assert not inst_limit or max_retire <= inst_limit
        assert int(stats[cpu + ".commit.commitInstWidthFullCycles"]) == \
            full_cycles
        return {"commitInstWidth": inst_limit,
                "max_successful_retire": max_retire,
                "quota_full_cycles": full_cycles,
                "successful_retire": sum(commit_rates.values())}
    assert next_group and retire_per_tick
    max_retire = max(retire_per_tick.values())
    if inst_limit:
        assert max_retire <= inst_limit, (max_retire, inst_limit)
    for tick, rate in commit_rates.items():
        assert rate == retire_per_tick[tick], (tick, rate, retire_per_tick[tick])
    for tick, accessed in visited.items():
        # Match countInstsOfGroups(): successful retires use the expanded
        # window. Legacy squashed-head draining does not charge that budget
        # and can visit extra groups; it is not a strict group-access model.
        assert retire_per_tick[tick] <= window_budget[tick], tick
        if not drained_per_tick[tick]:
            assert len(accessed) <= group_limit, (tick, len(accessed))
    groups_total = sum(allocated_lengths.values())
    insts_total = sum(length * count
                      for length, count in allocated_lengths.items())
    full_cycles = (sum(count == inst_limit for count in retire_per_tick.values())
                   if inst_limit else 0)
    for suffix, expected in {
        ".rob.hybridAllocatedGroups": groups_total,
        ".rob.hybridAllocatedInsts": insts_total,
        ".rob.hybridGroupLength::samples": groups_total,
        ".commit.commitInstWidthFullCycles": full_cycles,
    }.items():
        assert int(stats[cpu + suffix]) == expected, (suffix, expected)
    for kind, name in enumerate(("NORMAL-S", "NORMAL-C", "NORMAL-N",
                                 "CC", "CS", "SC")):
        assert int(stats[cpu + ".rob.hybridGroupType::" + name]) == \
            allocated_types[kind], name
    for length, count in allocated_lengths.items():
        assert int(stats[cpu + ".rob.hybridGroupLength::" + str(length)]) == count
    assert sum(retire_per_tick.values()) + squashed + \
        sum(group[1] for group in groups) == insts_total

    return {
        "commitInstWidth": inst_limit,
        "max_successful_retire": max_retire,
        "quota_full_cycles": full_cycles,
        "quota_full_with_partial_group": sum(
            count == inst_limit and remaining_after_last_retire[tick] > 0
            for tick, count in retire_per_tick.items()) if inst_limit else 0,
        "max_groups_accessed": max(map(len, visited.values())),
        "allocated_groups": groups_total,
        "allocated_dyninsts": insts_total,
        "allocation_compression_ratio": insts_total / groups_total,
        "successful_retire": sum(retire_per_tick.values()),
        "squashed_removed": squashed,
        "invariant_checks": invariant_checks,
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--config", type=Path)
    parser.add_argument("--stats", type=Path)
    parser.add_argument("--cpu", default="system.cpu")
    parser.add_argument("--rates-only", action="store_true",
                        help="Validate a trace containing only CommitRate")
    args = parser.parse_args()
    result = check_trace(args.trace,
                         args.config or args.trace.parent / "config.ini",
                         args.stats or args.trace.parent / "stats.txt", args.cpu,
                         args.rates_only)
    print(json.dumps(result, indent=2))
