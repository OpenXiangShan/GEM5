# Copyright (c) 2026 Institute of Computing Technology, CAS
# SPDX-License-Identifier: BSD-3-Clause

"""Verify that missing PCs never train/predict/bypass, even at threshold zero."""

import json
from pathlib import Path

import m5
from m5.objects import (
    AddrRange,
    Cache,
    MemTest,
    Root,
    SDBPRP,
    SimpleMemory,
    SrcClockDomain,
    System,
    SystemXBar,
    VoltageDomain,
)

system = System(mem_mode="timing", mem_ranges=[AddrRange("16MiB")])
system.clk_domain = SrcClockDomain(
    clock="1GHz", voltage_domain=VoltageDomain()
)
system.tester = MemTest(
    max_loads=10000,
    percent_functional=0,
    percent_uncacheable=0,
)
system.cache = Cache(
    size="4KiB",
    assoc=4,
    tag_latency=1,
    data_latency=1,
    response_latency=1,
    mshrs=8,
    tgts_per_mshr=8,
    cache_level=1,
    replacement_policy=SDBPRP(
        num_sets=16,
        sampler_num=4,
        sampler_assoc=3,
        dead_threshold=0,
        enable_bypass=True,
    ),
)
system.membus = SystemXBar()
system.memory = SimpleMemory(range=system.mem_ranges[0], latency="20ns")
system.tester.port = system.cache.cpu_side
system.cache.mem_side = system.membus.cpu_side_ports
system.memory.port = system.membus.mem_side_ports
system.system_port = system.membus.cpu_side_ports
root = Root(full_system=False, system=system)
m5.instantiate()
event = m5.simulate()
assert event.getCause() == "maximum number of loads reached", event.getCause()
m5.stats.dump()
stats = {}
for line in (Path(m5.options.outdir) / "stats.txt").read_text().splitlines():
    fields = line.split()
    if len(fields) > 1 and fields[0].startswith(
        "system.cache.replacement_policy."
    ):
        stats[fields[0].split(".")[-1]] = float(fields[1])
for name in (
    "samplerAccesses",
    "predictionQueries",
    "deadVictims",
    "bypasses",
):
    assert stats[name] == 0, (name, stats[name])
assert stats["noPcAccesses"] > 10000
assert stats["lruVictims"] > 0
print("No-PC filtering PASS: " + json.dumps(stats, sort_keys=True))
