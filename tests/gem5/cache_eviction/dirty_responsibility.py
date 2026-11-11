# Copyright (c) 2026 Institute of Computing Technology, CAS
# SPDX-License-Identifier: BSD-3-Clause

"""Check retained L2 dirty responsibility and data recovery in five phases.

Compile dirty_responsibility.S as a static freestanding RISC-V executable.
The guest checks values; per-phase statistics distinguish inherited dirty
responsibility from stores issued by L1 itself.
"""

import argparse
import json
from pathlib import Path

import m5
from m5.objects import (
    NULL,
    AddrRange,
    AtomicSimpleCPU,
    Cache,
    L2XBar,
    Process,
    Root,
    SEWorkload,
    SimpleMemory,
    SrcClockDomain,
    System,
    SystemXBar,
    TimingSimpleCPU,
    VoltageDomain,
)

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("binary")
parser.add_argument("--mode", choices=("timing", "atomic"), default="timing")
parser.add_argument("--legacy", action="store_true")
parser.add_argument("--invalidate", action="store_true")
args = parser.parse_args()
assert not (args.mode == "atomic" and args.invalidate)

system = System(
    mem_mode=args.mode,
    mem_ranges=[AddrRange("128MiB")],
    cache_line_size=64,
)
system.clk_domain = SrcClockDomain(
    clock="1GHz", voltage_domain=VoltageDomain()
)
system.cpu = TimingSimpleCPU() if args.mode == "timing" else AtomicSimpleCPU()


def cache(size, assoc, level, **kwargs):
    return Cache(
        size=size,
        assoc=assoc,
        cache_level=level,
        cache_hierarchy_policy="legacy"
        if args.legacy
        else "xiangshan_single_core",
        tag_latency=1,
        data_latency=2,
        response_latency=1,
        mshrs=8,
        tgts_per_mshr=8,
        prefetcher=NULL,
        **kwargs,
    )


system.icache = cache("2KiB", 2, 1, is_read_only=True)
system.dcache = cache("2KiB", 2, 1)
system.l2bus = L2XBar()
system.l2 = cache(
    "32KiB",
    4,
    2,
    writeback_clean=True,
    l2_evict_invalidate_l1=args.invalidate,
)
system.l3bus = L2XBar()
system.l3 = cache("128KiB", 4, 3, clusivity="mostly_excl")
system.membus = SystemXBar()
system.memory = SimpleMemory(range=system.mem_ranges[0], latency="40ns")
system.cpu.icache_port = system.icache.cpu_side
system.cpu.dcache_port = system.dcache.cpu_side
system.icache.mem_side = system.l2bus.cpu_side_ports
system.dcache.mem_side = system.l2bus.cpu_side_ports
system.l2.cpu_side = system.l2bus.mem_side_ports
system.l2.mem_side = system.l3bus.cpu_side_ports
system.l3.cpu_side = system.l3bus.mem_side_ports
system.l3.mem_side = system.membus.cpu_side_ports
system.memory.port = system.membus.mem_side_ports
system.system_port = system.membus.cpu_side_ports
system.cpu.createInterruptController()
system.workload = SEWorkload.init_compatible(args.binary)
system.cpu.workload = Process(cmd=[args.binary])
system.cpu.createThreads()
root = Root(full_system=False, system=system)
m5.instantiate()


def read_stats():
    text = (Path(m5.options.outdir) / "stats.txt").read_text()
    text = text.split("---------- Begin Simulation Statistics")[-1]
    result = {}
    for line in text.splitlines():
        fields = line.split()
        if len(fields) > 1:
            try:
                result[fields[0]] = float(fields[1])
            except ValueError:
                pass
    return result


phases = []
for phase in range(5):
    event = m5.simulate()
    assert event.getCause() == "m5_exit instruction encountered", event
    m5.stats.dump()
    values = read_stats()
    selected = {
        key: value
        for key, value in values.items()
        if "retainedDirty" in key
        or "writebacks::total" in key
        or "evictionSnoop" in key
    }
    phases.append(selected)
    if phase == 0:
        assert values.get("system.dcache.writebacks::total", 0) > 0
    if phase in (1, 4):
        writebacks = values.get("system.dcache.writebacks::total", 0)
        if not args.legacy:
            assert writebacks == 0, selected
        elif phase == 1:
            assert writebacks > 0, selected
    if phase == 1 and not args.legacy:
        assert values["system.l2.retainedDirtyGrants"] > 0, selected
    if phase == 3 and args.mode == "timing" and not args.legacy:
        assert values["system.l2.evictionSnoop.dirtyResponses"] > 0, selected
    m5.stats.reset()

event = m5.simulate()
assert event.getCause() == "exiting with last active thread context", event
assert event.getCode() == 0, event.getCode()
(Path(m5.options.outdir) / "dirty-check.json").write_text(
    json.dumps(phases, indent=2) + "\n"
)
print("Retained dirty responsibility and latest-data checks PASS")
