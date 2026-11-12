# Copyright (c) 2026 XiangShan
# SPDX-License-Identifier: BSD-3-Clause
"""Exercise KPC through real cache requests, refills and local prefetch probes.

Run with gem5.opt; --prefetch also exercises the bypass of accessBlock().
"""

import argparse
from pathlib import Path

import m5
from m5.objects import (
    AddrRange,
    Cache,
    KPCReplState,
    KPCRP,
    PyTrafficGen,
    Root,
    RiscvMMU,
    SimpleMemory,
    SrcClockDomain,
    System,
    SystemXBar,
    L2CompositeWithWorkerPrefetcher,
    VoltageDomain,
)

parser = argparse.ArgumentParser()
parser.add_argument("--prefetch", action="store_true")
parser.add_argument("--clean-writebacks", action="store_true")
args = parser.parse_args()
system = System(mem_mode="timing", mem_ranges=[AddrRange("1MiB")])
system.clk_domain = SrcClockDomain(
    clock="1GHz", voltage_domain=VoltageDomain()
)
system.kpc_state = KPCReplState(
    num_sets=8, num_ways=2, sampled_sets=8, block_size=64
)
system.cache = Cache(
    size="1KiB",
    assoc=2,
    tag_latency=1,
    data_latency=1,
    response_latency=1,
    mshrs=8,
    tgts_per_mshr=8,
    cache_level=2,
    replacement_policy=KPCRP(state=system.kpc_state),
)
if args.prefetch:
    # Use the XS composite: legacy generic prefetchers omit required source
    # metadata in this tree. Short BOP training keeps the fixture bounded.
    system.cache.prefetcher = L2CompositeWithWorkerPrefetcher(
        enable_bop=True, use_virtual_addresses=False
    )
    for bop in (system.cache.prefetcher.bop_large,
                system.cache.prefetcher.bop_small):
        bop.offsets = [1]
        bop.score_max = 1
        bop.bad_score = 0
        bop.delay_queue_enable = False
        bop.enable_adaptoffset = False
# Construct the requestor before the cache snapshots maxRequestors().
system.a_generator = PyTrafficGen()
system.bus = SystemXBar()
system.memory = SimpleMemory(range=system.mem_ranges[0], latency="5ns")
if args.clean_writebacks:
    system.l1 = Cache(
        size="128B", assoc=2, tag_latency=1, data_latency=1,
        response_latency=1, mshrs=8, tgts_per_mshr=8,
        cache_level=1, writeback_clean=True,
    )
    system.a_generator.port = system.l1.cpu_side
    system.l1.mem_side = system.cache.cpu_side
else:
    system.a_generator.port = system.cache.cpu_side
system.cache.mem_side = system.bus.cpu_side_ports
system.memory.port = system.bus.mem_side_ports
system.system_port = system.bus.cpu_side_ports
if args.prefetch:
    # The disabled LLDP child still registers probes and requires a data TLB.
    system.mmu = RiscvMMU()
    for tlb in (system.mmu.itb, system.mmu.dtb, system.mmu.l2_shared):
        tlb.walker.port = system.bus.cpu_side_ports
    system.cache.prefetcher.registerTLB(system.mmu.dtb, False)
root = Root(full_system=False, system=system)
m5.instantiate()


def traffic():
    # A streaming phase exceeds both the real cache and independent sampler.
    yield system.a_generator.createLinear(
        0, 0, 64 * 1024, 64, 20000, 20000, 100, 64 * 1024
    )
    # Then a repeatedly accessed small working set supplies sampler reuse.
    yield system.a_generator.createLinear(
        0, 0, 256, 64, 20000, 20000, 100, 4096
    )
    yield system.a_generator.createIdle(100000)
    yield system.a_generator.createExit(0)


system.a_generator.start(traffic())
event = m5.simulate()
m5.stats.dump()
values = {}
for line in (Path(m5.options.outdir) / "stats.txt").read_text().splitlines():
    fields = line.split()
    if len(fields) >= 2 and fields[0].startswith("system."):
        try:
            values[fields[0]] = float(fields[1])
        except ValueError:
            pass
assert values["system.kpc_state.demandAccesses"] > 0
assert values["system.kpc_state.samplerMisses"] > 0
assert values["system.kpc_state.demandFirstReuse"] > 0
if args.clean_writebacks:
    clean_accesses = sum(
        values.get(f"system.cache.WritebackClean.{kind}::total", 0)
        for kind in ("hits", "misses")
    )
    read_accesses = sum(
        values.get(f"system.cache.{cmd}.{kind}::total", 0)
        for cmd in ("ReadReq", "ReadSharedReq", "ReadCleanReq", "ReadExReq")
        for kind in ("hits", "misses")
    )
    assert clean_accesses > 0
    assert values["system.kpc_state.demandAccesses"] == (
        read_accesses + clean_accesses
    ), (values["system.kpc_state.demandAccesses"],
        read_accesses + clean_accesses)
    assert values["system.kpc_state.demandFirstReuse"] >= 1024
elif args.prefetch:
    assert values["system.kpc_state.prefetchAccesses"] > 0
    assert values["system.cache.replacement_policy.prefetchInsertions2"] > 0
else:
    assert values["system.kpc_state.demandDead"] > 0
    assert values["system.cache.replacement_policy.demandInsertions3"] > 0
print(f"KPC integration checks passed: {event.getCause()}")
