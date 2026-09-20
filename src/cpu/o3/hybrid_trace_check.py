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
    entry_limit = int(params["commitWidth"])
    capacity = int(params["numROBEntries"])
    length_limit = int(params["CROB_instPerGroup"])
    assert not rates_only, "Physical entry validation requires ROB,CommitRate"

    stats = {}
    for line in Path(stats_path).read_text().splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[0].startswith(cpu + "."):
            stats[fields[0]] = fields[1]

    prefix = re.compile(r"^\s*(\d+): " + re.escape(cpu) +
                        r"\.(rob|commit): (.*)$")
    allocation = re.compile(r"Hybrid allocate id=(\d+) type=(\d+) "
                            r"former=(\d+) latter=(\d+)")
    member = re.compile(r"Hybrid member id=(\d+) sn=(\d+) former=([01])")
    removal = re.compile(r"Hybrid remove id=(\d+) sn=(\d+) former=([01]) "
                         r"reason=(commit|drain|squash) remaining=(\d+)")
    redirect = re.compile(r"Hybrid squash id=(\d+) former=([01]) "
                          r"itself=([01]) boundary=(\d+)")
    full_flush = re.compile(r"Hybrid full squash boundary=(\d+)")
    downgrade = re.compile(r"Hybrid downgrade id=(\d+) former=(\d+)")
    invariant = re.compile(r"Hybrid invariant groups=(\d+) "
                           r"dynInsts=(\d+) sum=(\d+)")
    entries = {}  # Insertion ordered; ids never reused after squash.
    allocated_types = Counter()
    allocated_lengths = Counter()
    retire_per_tick = Counter()
    commit_rates = {}
    visited = {}
    committed_entries = drained_entries = squashed = downgrades = 0
    invariant_checks = 0
    last_id = 0
    target = None
    squash_boundary = None
    pending_downgrade = None

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
            if pending_downgrade is not None:
                assert downgrade.fullmatch(message), (lineno, pending_downgrade)
            if match := allocation.fullmatch(message):
                eid, kind, former, latter = map(int, match.groups())
                length = former + latter
                assert eid > last_id and 0 <= kind < 6, lineno
                assert former >= 1 and 1 <= length <= length_limit, lineno
                assert bool(latter) == (kind >= 3), lineno
                assert kind != 3 or (former, latter) == (1, 1), lineno
                assert kind != 4 or former == 1, lineno
                assert kind != 5 or latter == 1, lineno
                entries[eid] = {"type": kind, "former": former,
                                "latter": latter, "members": deque()}
                allocated_types[kind] += 1
                allocated_lengths[length] += 1
                last_id = eid
                assert len(entries) <= capacity, lineno
            elif match := member.fullmatch(message):
                eid, sn, former = map(int, match.groups())
                entry = entries[eid]
                assert former == (len(entry["members"]) < entry["former"])
                entry["members"].append((sn, former))
            elif match := redirect.fullmatch(message):
                eid, former, itself, boundary = map(int, match.groups())
                assert eid in entries, lineno
                # Independent literal truth table: retain neither, former,
                # former, or both slots in the redirect entry.
                keep = {(1, 1): (), (1, 0): (1,),
                        (0, 1): (1,), (0, 0): (1, 0)}[former, itself]
                for key, entry in entries.items():
                    for sn, slot in entry["members"]:
                        retained = key < eid or (key == eid and slot in keep)
                        assert (sn <= boundary) == retained, (lineno, key, sn)
                target = (eid, keep)
                squash_boundary = boundary
            elif match := full_flush.fullmatch(message):
                squash_boundary = int(match[1])
                target = None
            elif match := removal.fullmatch(message):
                eid, sn, former, reason, remaining = match.groups()
                eid, sn, former, remaining = map(int, (eid, sn, former, remaining))
                entry = entries[eid]
                if reason == "squash":
                    assert squash_boundary is not None, lineno
                    assert sn > squash_boundary, (lineno, sn, squash_boundary)
                    assert eid == next(reversed(entries)), lineno
                    actual = entry["members"].pop()
                    squashed += 1
                else:
                    assert eid == next(iter(entries)), lineno
                    actual = entry["members"].popleft()
                    visited.setdefault(tick, set()).add(eid)
                    if reason == "commit":
                        retire_per_tick[tick] += 1
                        committed_entries += remaining == 0
                    else:
                        squashed += 1
                        drained_entries += remaining == 0
                assert actual == (sn, former), (lineno, actual, sn, former)
                entry["former" if former else "latter"] -= 1
                assert min(entry["former"], entry["latter"]) >= 0, lineno
                assert remaining == len(entry["members"]), lineno
                if (reason == "squash" and not former and
                        entry["latter"] == 0 and entry["former"] > 0 and
                        entry["type"] >= 3 and target == (eid, (1,))):
                    pending_downgrade = eid
                if remaining == 0:
                    del entries[eid]
            elif match := downgrade.fullmatch(message):
                eid, former = map(int, match.groups())
                assert pending_downgrade == eid, lineno
                pending_downgrade = None
                entry = entries[eid]
                assert entry["type"] >= 3 and former > 0, lineno
                assert entry["former"] == former and entry["latter"] == 0
                assert target == (eid, (1,)), lineno
                entry["type"] = 0  # NORMAL, irrespective of former class.
                downgrades += 1
            elif match := invariant.fullmatch(message):
                count, dyninsts, total = map(int, match.groups())
                assert count <= capacity and dyninsts == total, lineno
                assert count == len(entries), lineno
                invariant_checks += 1

    assert pending_downgrade is None
    assert commit_rates and allocated_lengths
    # A trace may end while a simulator exit callback is retiring an instruction,
    # before CommitRate's cycle epilogue. Check every completed cycle exactly.
    for tick, rate in commit_rates.items():
        assert rate == retire_per_tick[tick], (tick, rate, retire_per_tick[tick])
    for tick, accessed in visited.items():
        assert len(accessed) <= entry_limit, (tick, len(accessed), entry_limit)
    groups_total = sum(allocated_lengths.values())
    insts_total = sum(length * count for length, count in allocated_lengths.items())
    for suffix, expected in {
        ".rob.hybridAllocatedGroups": groups_total,
        ".rob.hybridAllocatedInsts": insts_total,
        ".rob.hybridGroupLength::samples": groups_total,
        ".rob.hybridDowngrades": downgrades,
        ".commit.hybridCommittedEntries": committed_entries,
        ".commit.hybridDrainedEntries": drained_entries,
    }.items():
        assert int(stats[cpu + suffix]) == expected, (suffix, expected)
    for kind, name in enumerate(("NORMAL-S", "NORMAL-C", "NORMAL-N", "CC", "CS", "SC")):
        assert int(stats[cpu + ".rob.hybridGroupType::" + name]) == allocated_types[kind]
    for length, count in allocated_lengths.items():
        assert int(stats[cpu + ".rob.hybridGroupLength::" + str(length)]) == count
    assert sum(retire_per_tick.values()) + squashed + sum(
        len(entry["members"]) for entry in entries.values()) == insts_total

    return {
        "commit_entry_width": entry_limit,
        "max_successful_retire": max(retire_per_tick.values(), default=0),
        "max_entries_accessed": max(map(len, visited.values()), default=0),
        "committed_entries": committed_entries,
        "allocated_groups": groups_total,
        "allocated_dyninsts": insts_total,
        "allocation_compression_ratio": insts_total / groups_total,
        "successful_retire": sum(retire_per_tick.values()),
        "squashed_removed": squashed,
        "downgrades": downgrades,
        "invariant_checks": invariant_checks,
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--config", type=Path)
    parser.add_argument("--stats", type=Path)
    parser.add_argument("--cpu", default="system.cpu")
    parser.add_argument("--rates-only", action="store_true",
                        help="Unsupported: physical entry validation requires ROB logs")
    args = parser.parse_args()
    result = check_trace(args.trace,
                         args.config or args.trace.parent / "config.ini",
                         args.stats or args.trace.parent / "stats.txt", args.cpu,
                         args.rates_only)
    print(json.dumps(result, indent=2))
