# Copyright (c) 2026
# All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause

"""Check sampled LRU/ICP selection through real cache packets."""

import argparse
from pathlib import Path

import m5
from m5.objects import (
    AddrRange, Cache, ICPRP, PrefetcherForwarder, Root, SimpleMemory,
    SrcClockDomain, System, SystemXBar, TrafficGen, VoltageDomain,
)

parser = argparse.ArgumentParser()
parser.add_argument("--scenario", choices=["leaders", "switch", "prefetch-only",
                    "saturation", "merged-lru", "merged-icp",
                    "sampling-geometry", "merged-prefetch-lru",
                    "merged-prefetch-icp", "merged-upper-demand-lru",
                    "merged-upper-demand-icp"], required=True)
parser.add_argument("--pc-entries", type=int, choices=[0, 64], default=0)
args = parser.parse_args()
READ, SOFT_PREFETCH, PREFETCH = 2, 12, 14
PF_FLAG = 0x01000000
trace = []
expected = dict(lruLeaderDemandFills=0, icpLeaderDemandFills=0,
                followerLruPrefetchFills=0, followerIcpPrefetchFills=0,
                policySwitches=0, selectorValue=511,
                usefulPrefetches=0, unusedPrefetches=0, epfHits=0,
                ignoredPrefetchHits=0)


def reads(lines):
    trace.extend((READ, line, 0) for line in lines)


def prefetch(line, soft=False):
    trace.append((SOFT_PREFETCH if soft else PREFETCH, line, PF_FLAG))


if args.scenario == "sampling-geometry":
    # Exercise all set-index bits at real slice geometry. Each policy must
    # receive exactly 1/32 of the sets, and balanced fills restore selection.
    reads(range(1024))
    misses = 1024
    expected.pop("policySwitches")
    expected.update(lruLeaderDemandFills=32, icpLeaderDemandFills=32,
                    prefetchFills=0, highPriorityFills=0, lowPriorityFills=0)
elif args.scenario == "leaders":
    # Set 0 always uses LRU; set 3 always uses ICP, regardless of selector.
    reads([0, 4, 8, 12, 3, 7, 11, 15])
    prefetch(16)
    reads([20])
    prefetch(16, soft=True)
    reads([24, 28, 32, 16])
    prefetch(19)
    reads([23, 19])
    misses = 14
    expected.update(lruLeaderDemandFills=8, icpLeaderDemandFills=6,
                    selectorValue=513, policySwitches=3,
                    prefetchFills=2, highPriorityFills=1, lowPriorityFills=1,
                    usefulPrefetches=1, unusedPrefetches=1, epfHits=1)
elif args.scenario == "switch":
    # Follower set 1 first uses LRU, then ICP, then returns to LRU.
    reads([1, 5, 9, 13])
    prefetch(17)
    reads([21, 25])
    prefetch(17, soft=True)
    reads([29, 33, 37, 17, 0])
    prefetch(41)
    reads([45, 41, 3])
    prefetch(49)
    reads([53, 57, 61, 49])
    misses = 16
    expected.update(lruLeaderDemandFills=1, icpLeaderDemandFills=1,
                    followerLruPrefetchFills=2, followerIcpPrefetchFills=1,
                    policySwitches=2, prefetchFills=3,
                    highPriorityFills=2, lowPriorityFills=1,
                    usefulPrefetches=2, unusedPrefetches=1, epfHits=1)
elif args.scenario == "prefetch-only":
    prefetch(0)
    prefetch(3)
    prefetch(1)
    prefetch(0, soft=True)
    prefetch(3, soft=True)
    prefetch(2)
    misses = 0
    expected.update(followerLruPrefetchFills=2, prefetchFills=4,
                    highPriorityFills=3, lowPriorityFills=1,
                    ignoredPrefetchHits=1)
elif args.scenario == "saturation":
    # Both extremes must saturate, rather than wrap and change the choice.
    reads(range(0, 4096, 4))
    prefetch(1)
    reads(range(3, 4099, 4))
    prefetch(5)
    misses = 2048
    expected.update(lruLeaderDemandFills=1024, icpLeaderDemandFills=1024,
                    followerLruPrefetchFills=1, followerIcpPrefetchFills=1,
                    policySwitches=2, selectorValue=0, prefetchFills=2,
                    highPriorityFills=1, lowPriorityFills=1)
else:
    # Model local hardware prefetches and upper-level prefetches, which
    # arrive as ReadReq with PREFETCH set. Only a true demand target may
    # train the selected leader and credit useful prefetches.
    icp = args.scenario.endswith("icp")
    line = 3 if icp else 0
    if args.scenario.startswith("merged-upper-demand"):
        trace.append((READ, line, PF_FLAG))
    else:
        prefetch(line)
    if args.scenario.startswith("merged-prefetch"):
        trace.append((READ, line, PF_FLAG))
    else:
        reads([line])
    if args.scenario.startswith("merged-upper-demand"):
        # A subsequent demand hit must not credit the merged use twice.
        reads([line])
    misses = 2 if args.scenario.startswith("merged-upper-demand") else 1
    expected.update(lruLeaderDemandFills=0 if icp else 1,
                    icpLeaderDemandFills=1 if icp else 0,
                    selectorValue=510 if icp else 512,
                    policySwitches=0 if icp else 1, prefetchFills=1,
                    highPriorityFills=0 if icp else 1,
                    lowPriorityFills=1 if icp else 0, usefulPrefetches=1)

    if args.scenario.startswith("merged-prefetch"):
        expected.update(lruLeaderDemandFills=0, icpLeaderDemandFills=0,
                        selectorValue=511, policySwitches=0, usefulPrefetches=0)


def varint(value):
    result = bytearray()
    while value >= 128:
        result.append((value & 127) | 128)
        value >>= 7
    result.append(value)
    return bytes(result)


def integer(field, value):
    return varint(field << 3) + varint(value)


def message(file, value):
    file.write(varint(len(value)))
    file.write(value)


output = Path(m5.options.outdir)
trace_path = output / "adaptive-trace.pb"
with trace_path.open("wb") as file:
    file.write(b"gem5")
    name = b"ICP adaptive integration test"
    message(file, varint(10) + varint(len(name)) + name + integer(3, 10**12))
    for i, (cmd, line, flags) in enumerate(trace):
        tick = 105000 if args.scenario.startswith("merged") and i == 1 else (i + 1) * 100000
        packet = (integer(1, tick) + integer(2, cmd) + integer(3, line * 64) +
                  integer(4, 64) + integer(5, flags) + integer(7, 0x1000))
        message(file, packet)
duration = (len(trace) + 2) * 100000
state_path = output / "adaptive-traffic.cfg"
state_path.write_text(
    f"STATE 0 {duration} TRACE {trace_path} 0\n"
    "STATE 1 100000 EXIT\nINIT 0\nTRANSITION 0 1 1\nTRANSITION 1 1 1\n"
)
system = System(mem_mode="timing", mem_ranges=[AddrRange("1MiB")])
system.clk_domain = SrcClockDomain(clock="1GHz", voltage_domain=VoltageDomain())
system.cache_line_size = 64
system.cpu = TrafficGen(config_file=str(state_path))
num_sets = 1024 if args.scenario == "sampling-geometry" else 4
system.l2 = Cache(
    size=f"{num_sets * 4 * 64}B", assoc=4, tag_latency=1, data_latency=1,
    response_latency=1, mshrs=8, tgts_per_mshr=8, cache_level=2,
    prefetcher=PrefetcherForwarder(),
    replacement_policy=ICPRP(
        num_sets=num_sets, num_ways=4, block_size=64, pc_entries=args.pc_entries,
        initial_accuracy=0, accuracy_threshold=8, epf_entries=8, epf_assoc=4,
        adaptive=True, sample_stride=32 if num_sets == 1024 else 4,
    ),
)
system.bus = SystemXBar()
system.memory = SimpleMemory(range=system.mem_ranges[0], latency="5ns")
system.cpu.port = system.l2.cpu_side
system.l2.mem_side = system.bus.cpu_side_ports
system.memory.port = system.bus.mem_side_ports
system.system_port = system.bus.cpu_side_ports
root = Root(full_system=False, system=system)
m5.instantiate()
event = m5.simulate(duration + 100000)
assert "has encountered the exit state" in event.getCause(), event.getCause()
m5.stats.dump()
values = {}
for line in (output / "stats.txt").read_text().splitlines():
    fields = line.split()
    if len(fields) > 1:
        values[fields[0]] = fields[1]
for name, value in expected.items():
    actual = float(values["system.l2.replacement_policy." + name])
    assert actual == value, (args.scenario, name, actual, value)
# The cache omits an all-zero requestor vector when no demand was issued.
key = "system.l2.demandMisses::total"
actual = float(values.get(key, 0)) if misses == 0 else float(values[key])
assert actual == misses, (args.scenario, "demandMisses", actual, misses)
print("PASS ICP adaptive", args.scenario, args.pc_entries)
