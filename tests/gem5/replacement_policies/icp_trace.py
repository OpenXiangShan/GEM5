# Copyright (c) 2026 XiangShan
# SPDX-License-Identifier: BSD-3-Clause

"""Exercise ICP through actual cache packets, fills and evictions.

Run with gem5.opt; use --help for scenarios and predictor/insertion options.
The trace is serialized locally without requiring protoc or Python protobuf.
"""

import argparse
from pathlib import Path

import m5
from m5.objects import (
    AddrRange, Cache, ICPRP, PrefetcherForwarder, Root, SimpleMemory, SrcClockDomain,
    System, SystemXBar, TrafficGen, VoltageDomain,
)

parser = argparse.ArgumentParser()
parser.add_argument("--scenario", default="feedback",
                    choices=["feedback", "no-epf", "secure", "demotion",
                             "merged", "superseded", "pc-isolation",
                             "low-retention", "prefetch-recency"])
parser.add_argument("--pc-entries", type=int, default=0, choices=[0, 64])
parser.add_argument("--low-priority", type=int, default=-1, choices=[-1, 2])
parser.add_argument("--high-priority", type=int, default=0, choices=[0, 1])
args = parser.parse_args()


def varint(value):
    data = bytearray()
    while value >= 128:
        data.append((value & 127) | 128)
        value >>= 7
    data.append(value)
    return bytes(data)


def integer(field, value):
    return varint(field << 3) + varint(value)


def message(file, value):
    file.write(varint(len(value)) + value)


# MemCmd indices are from src/mem/packet.hh. Request flags from request.hh.
# HardPFReq is admitted through the cache hardware prefetch queue.
READ, SOFT_PREFETCH, PREFETCH = 2, 12, 14
PF_FLAG, SECURE_FLAG = 0x01000000, 0x10000000
trace = [(READ, line, 0) for line in range(4)]
if args.scenario == "prefetch-recency":
    # A configurable insertion rank must not demote the most recent line
    # when an accurate prefetch hits it.
    trace += [(READ, 0, 0), (SOFT_PREFETCH, 0, PF_FLAG)]
    trace += [(READ, line, 0) for line in range(4, 7)]
    trace += [(READ, 0, 0)]
    expected = dict(prefetchFills=0, highPriorityFills=0, lowPriorityFills=0,
                    usefulPrefetches=0, unusedPrefetches=0, epfHits=0,
                    ignoredPrefetchHits=0)
elif args.scenario == "low-retention":
    trace += [(PREFETCH, 4, PF_FLAG), (READ, 5, 0), (READ, 4, 0)]
    retained = args.low_priority == 2
    expected = dict(prefetchFills=1, highPriorityFills=0, lowPriorityFills=1,
                    usefulPrefetches=1 if retained else 0,
                    unusedPrefetches=0 if retained else 1,
                    epfHits=0 if retained else 1)
elif args.scenario == "pc-isolation":
    # One successful stream must not raise an unrelated stream's confidence.
    trace += [(PREFETCH, 4, PF_FLAG), (READ, 4, 0),
              (PREFETCH, 5, PF_FLAG), (PREFETCH, 6, PF_FLAG)]
    split = bool(args.pc_entries)
    expected = dict(prefetchFills=3, highPriorityFills=1 if split else 2,
                    lowPriorityFills=2 if split else 1, usefulPrefetches=1,
                    unusedPrefetches=1 if split else 0, epfHits=0,
                    pcPrefetchFills=3 if split else 0,
                    fallbackPrefetchFills=0 if split else 3)
elif args.scenario == "superseded":
    # Re-prefetch an evicted low-priority line, then consume it. Merely
    # superseding the old ghost must not penalize the accuracy predictor.
    trace += [(PREFETCH, 4, PF_FLAG), (READ, 5, 0),
              (PREFETCH, 4, PF_FLAG), (READ, 4, 0), (PREFETCH, 6, PF_FLAG)]
    expected = dict(prefetchFills=3, highPriorityFills=1, lowPriorityFills=2,
                    usefulPrefetches=1, unusedPrefetches=1, epfHits=0,
                    epfSuperseded=1)
elif args.scenario == "merged":
    trace += [(PREFETCH, 4, PF_FLAG), (READ, 4, 0)]
    trace += [(READ, line, 0) for line in range(5, 9)]
    expected = dict(prefetchFills=1, highPriorityFills=1, lowPriorityFills=0,
                    usefulPrefetches=1, unusedPrefetches=0, epfHits=0)
elif args.scenario == "secure":
    trace += [(PREFETCH, 4, PF_FLAG), (READ, 5, 0),
              (READ, 4, SECURE_FLAG), (READ, 4, 0)]
    expected = dict(prefetchFills=1, highPriorityFills=0, lowPriorityFills=1,
                    usefulPrefetches=0, unusedPrefetches=1, epfHits=1)
elif args.scenario == "demotion":
    trace += [(PREFETCH, 4, PF_FLAG), (READ, 4, 0), (READ, 5, 0),
              (READ, 4, 0), (READ, 4, 0)]
    expected = dict(prefetchFills=1, highPriorityFills=1, lowPriorityFills=0,
                    usefulPrefetches=1, unusedPrefetches=0, epfHits=0)
else:
    trace += [(PREFETCH, 4, PF_FLAG), (READ, 5, 0), (READ, 4, 0),
              (PREFETCH, 6, PF_FLAG), (READ, 7, 0), (READ, 6, 0),
              (PREFETCH, 8, PF_FLAG), (READ, 8, 0), (READ, 8, 0),
              (PREFETCH, 9, PF_FLAG)]
    trace += [(READ, line, 0) for line in range(10, 14)]
    expected = dict(prefetchFills=4, usefulPrefetches=1,
                    unusedPrefetches=3)
    if args.scenario == "no-epf":
        expected.update(highPriorityFills=0, lowPriorityFills=4, epfHits=0)
    else:
        expected.update(highPriorityFills=2, lowPriorityFills=2, epfHits=2)

output = Path(m5.options.outdir)
trace_path = output / "icp-trace.pb"
with trace_path.open("wb") as file:
    file.write(b"gem5")
    name = b"ICP integration test"
    header = varint(10) + varint(len(name)) + name + integer(3, 10**12)
    message(file, header)
    for i, (cmd, line, flags) in enumerate(trace):
        packet = (integer(1, 505000 if args.scenario == "merged" and i == 5 else (i + 1) * 100000) + integer(2, cmd) +
                  integer(3, line * 64) + integer(4, 64) + integer(5, flags))
        if args.scenario == "pc-isolation":
            packet += integer(7, 0x2000 if i == 6 else 0x1000)
        message(file, packet)
state_path = output / "icp-traffic.cfg"
duration = (len(trace) + 2) * 100000
state_path.write_text(
    f"STATE 0 {duration} TRACE {trace_path} 0\n"
    "STATE 1 100000 EXIT\nINIT 0\nTRANSITION 0 1 1\nTRANSITION 1 1 1\n"
)

system = System(mem_mode="timing", mem_ranges=[AddrRange("1MiB")])
system.clk_domain = SrcClockDomain(clock="1GHz", voltage_domain=VoltageDomain())
system.cache_line_size = 64
system.cpu = TrafficGen(config_file=str(state_path))
system.l2 = Cache(
    size="256B", assoc=4, tag_latency=1, data_latency=1,
    response_latency=1, mshrs=8, tgts_per_mshr=8, cache_level=2,
    prefetcher=PrefetcherForwarder(),
    replacement_policy=ICPRP(
        num_sets=1, num_ways=4, block_size=64, pc_entries=args.pc_entries,
        high_priority=args.high_priority, low_priority=args.low_priority,
        initial_accuracy={"demotion": 2, "merged": 2, "superseded": 1,
                          "pc-isolation": 1,
                          "prefetch-recency": 2}.get(args.scenario, 0),
        accuracy_threshold=2,
        epf_entries=0 if args.scenario == "no-epf" else 8, epf_assoc=4,
        demote_on_first_use=args.scenario == "demotion",
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
    if len(fields) >= 2:
        values[fields[0]] = fields[1]
for name, value in expected.items():
    actual = float(values["system.l2.replacement_policy." + name])
    assert actual == value, (args.scenario, name, actual, value)
misses = {"secure": 7, "demotion": 6, "merged": 9, "superseded": 5,
          "pc-isolation": 4, "prefetch-recency": 7,
          "low-retention": 5 if args.low_priority == 2 else 6}.get(args.scenario, 12)
actual_misses = float(values["system.l2.demandMisses::total"])
assert actual_misses == misses, (args.scenario, "demandMisses", actual_misses, misses)
print("PASS ICP", args.scenario, expected, "demandMisses", actual_misses)
