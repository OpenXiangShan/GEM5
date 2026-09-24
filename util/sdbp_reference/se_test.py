# Copyright (c) 2026 Institute of Computing Technology, CAS
# SPDX-License-Identifier: BSD-3-Clause

"""Small timing-cache integration test with real instruction PCs and data."""

import argparse

import m5
from m5.objects import (
    AddrRange,
    Cache,
    LRURP,
    NULL,
    Process,
    Root,
    SDBPRP,
    SEWorkload,
    SimpleMemory,
    SrcClockDomain,
    System,
    SystemXBar,
    TimingSimpleCPU,
    VIPTSetAssoc,
    VIPTSetAssociative,
    SkewedAssociative,
    VoltageDomain,
)

parser = argparse.ArgumentParser()
parser.add_argument("binary")
parser.add_argument("--policy", choices=("lru", "sdbp"), default="sdbp")
parser.add_argument("--bypass", action="store_true")
parser.add_argument("--threshold", type=int, default=8)
parser.add_argument("--num-sets", type=int, default=64)
parser.add_argument("--sampler-num", type=int, default=8)
parser.add_argument("--sampler-assoc", type=int, default=6)
parser.add_argument(
    "--tags", choices=("standard", "vipt", "skewed"), default="standard"
)
args = parser.parse_args()

system = System(
    mem_mode="timing",
    mem_ranges=[AddrRange("128MiB")],
    cache_line_size=64,
)
system.clk_domain = SrcClockDomain(
    clock="1GHz",
    voltage_domain=VoltageDomain(),
)
system.cpu = TimingSimpleCPU()
system.membus = SystemXBar()
system.icache = Cache(
    size="16KiB",
    assoc=4,
    tag_latency=1,
    data_latency=1,
    response_latency=1,
    mshrs=4,
    tgts_per_mshr=8,
    cache_level=1,
    is_read_only=True,
    prefetcher=NULL,
)
system.dcache = Cache(
    size="32KiB",
    assoc=8,
    tag_latency=1,
    data_latency=1,
    response_latency=1,
    mshrs=8,
    tgts_per_mshr=8,
    cache_level=1,
    prefetcher=NULL,
)
if args.policy == "sdbp":
    system.dcache.replacement_policy = SDBPRP(
        num_sets=args.num_sets,
        sampler_num=args.sampler_num,
        sampler_assoc=args.sampler_assoc,
        enable_bypass=args.bypass,
        dead_threshold=args.threshold,
    )
else:
    system.dcache.replacement_policy = LRURP()
if args.tags == "vipt":
    system.dcache.tags = VIPTSetAssoc(indexing_policy=VIPTSetAssociative())
elif args.tags == "skewed":
    system.dcache.tags.indexing_policy = SkewedAssociative()
system.cpu.icache_port = system.icache.cpu_side
system.cpu.dcache_port = system.dcache.cpu_side
system.icache.mem_side = system.membus.cpu_side_ports
system.dcache.mem_side = system.membus.cpu_side_ports
system.system_port = system.membus.cpu_side_ports
system.memory = SimpleMemory(range=system.mem_ranges[0], latency="40ns")
system.memory.port = system.membus.mem_side_ports
system.cpu.createInterruptController()
system.workload = SEWorkload.init_compatible(args.binary)
system.cpu.workload = Process(cmd=[args.binary])
system.cpu.createThreads()
root = Root(full_system=False, system=system)
m5.instantiate()
event = m5.simulate()
print(f"Exit: {event.getCause()} ({event.getCode()}) at tick {m5.curTick()}")
if (
    event.getCause() != "exiting with last active thread context"
    or event.getCode()
):
    raise RuntimeError("SE integration workload did not complete successfully")
