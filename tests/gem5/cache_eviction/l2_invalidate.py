# Copyright (c) 2026 Institute of Computing Technology, CAS
# SPDX-License-Identifier: BSD-3-Clause

"""Check L2 victim invalidation with MemTest's independent data checker.

A small shared L2 forces replacements while larger L1s retain clean/dirty
copies. Reverse the sizes to exercise victims without upper copies.
"""

import argparse

import m5
from _m5.core import seedRandom
from m5.objects import (
    NULL,
    AddrRange,
    Cache,
    CoherentXBar,
    MemTest,
    L2CacheSlice,
    L2CacheWrapper,
    Root,
    SimpleMemory,
    SnoopFilter,
    SrcClockDomain,
    System,
    SystemXBar,
    VoltageDomain,
)

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--enable", action="store_true")
parser.add_argument("--sliced-l2", action="store_true")
parser.add_argument("--loads", type=int, default=100000)
parser.add_argument("--testers", type=int, default=2)
parser.add_argument("--reads", type=int, default=65)
parser.add_argument("--l1-size", default="8KiB")
parser.add_argument("--l2-size", default="1KiB")
parser.add_argument("--l1-data-latency", type=int, default=2)
parser.add_argument("--write-buffers", type=int, default=4)
parser.add_argument("--seed", type=int, default=1)
parser.add_argument("--functional", type=int, default=0)
parser.add_argument("--clean-evict", action="store_true")
parser.add_argument(
    "--cache-hierarchy-policy",
    choices=("legacy", "xiangshan_single_core"),
    default="legacy",
)
parser.add_argument(
    "--peer",
    action="store_true",
    help="Inject coherent lower-level traffic to exercise L3-to-L2 snoops",
)
args = parser.parse_args()
seedRandom(args.seed)
system = System(mem_mode="timing", mem_ranges=[AddrRange("16MiB")])
system.voltage_domain = VoltageDomain()
system.clk_domain = SrcClockDomain(
    clock="1GHz", voltage_domain=system.voltage_domain
)
system.cache_line_size = 64
system.to_l2 = CoherentXBar(
    width=64,
    frontend_latency=0,
    forward_latency=3,
    response_latency=1,
    snoop_response_latency=1,
    header_latency=0,
    snoop_filter=SnoopFilter(lookup_latency=0),
    point_of_unification=True,
)
l2 = Cache(
    size=args.l2_size,
    assoc=2,
    tag_latency=1,
    data_latency=2,
    response_latency=0,
    cache_level=2,
    cache_hierarchy_policy=args.cache_hierarchy_policy,
    mshrs=16,
    tgts_per_mshr=20,
    write_buffers=args.write_buffers,
    writeback_clean=not args.clean_evict,
    l2_evict_invalidate_l1=args.enable,
)
system.membus = SystemXBar()
lower_bus = system.membus
if args.peer:
    system.l3bus = CoherentXBar(
        width=32,
        frontend_latency=1,
        forward_latency=2,
        response_latency=2,
        snoop_response_latency=2,
        header_latency=1,
        snoop_filter=SnoopFilter(lookup_latency=1),
    )
    system.l3 = Cache(
        size="64KiB",
        assoc=4,
        tag_latency=2,
        data_latency=2,
        response_latency=2,
        cache_level=3,
        mshrs=16,
        tgts_per_mshr=20,
        clusivity="mostly_excl",
    )
    system.l3.cpu_side = system.l3bus.mem_side_ports
    system.l3.mem_side = system.membus.cpu_side_ports
    lower_bus = system.l3bus
if args.sliced_l2:
    system.l2_wrapper = L2CacheWrapper(
        num_slices=1,
        cache_size=args.l2_size,
        cache_assoc=2,
        prefetcher=NULL,
    )
    wrapper = system.l2_wrapper
    wrapper.slices = [L2CacheSlice(buffer_size=4)]
    cache_slice = wrapper.slices[0]
    cache_slice.inner_cache = l2
    wrapper.addCacheAccessor(l2)
    wrapper.addSliceAccessor(cache_slice)
    cache_slice.setCacheAccessor(l2)
    cache_slice.inner_cpu_port = l2.cpu_side
    l2.mem_side = cache_slice.inner_mem_port
    cache_slice.cpu_side = wrapper.slice_cpuside_ports
    wrapper.cpu_side = system.to_l2.mem_side_ports
    cache_slice.mem_side = lower_bus.cpu_side_ports
else:
    system.l2 = l2
    l2.cpu_side = system.to_l2.mem_side_ports
    l2.mem_side = lower_bus.cpu_side_ports
system.memory = SimpleMemory(range=system.mem_ranges[0], latency="30ns")
system.memory.port = system.membus.mem_side_ports
system.system_port = system.membus.cpu_side_ports
# Allocate tester requestor IDs before per-requestor cache statistics.
system.cpu = [
    MemTest(
        max_loads=args.loads,
        percent_reads=args.reads,
        percent_functional=args.functional,
        percent_uncacheable=0,
        size=16384,
        progress_check=1000000,
    )
    for _ in range(args.testers + int(args.peer))
]
system.l1 = [
    Cache(
        size=args.l1_size,
        assoc=2,
        tag_latency=1,
        data_latency=args.l1_data_latency,
        response_latency=0,
        cache_level=1,
        cache_hierarchy_policy=args.cache_hierarchy_policy,
        mshrs=8,
        tgts_per_mshr=20,
        write_buffers=4,
    )
    for _ in range(args.testers)
]
for tester, cache in zip(system.cpu[: args.testers], system.l1):
    tester.port = cache.cpu_side
    cache.mem_side = system.to_l2.cpu_side_ports
if args.peer:
    # A protocol test injector, not an additional modeled CPU. MemTest IDs
    # select distinct bytes of shared lines, checked against independent data.
    system.peer_cache = Cache(
        size="2KiB",
        assoc=2,
        tag_latency=1,
        data_latency=2,
        response_latency=1,
        cache_level=2,
        mshrs=8,
        tgts_per_mshr=20,
        write_buffers=4,
    )
    system.cpu[-1].port = system.peer_cache.cpu_side
    system.peer_cache.mem_side = system.l3bus.cpu_side_ports
root = Root(full_system=False, system=system)
m5.instantiate()
event = m5.simulate(10000000000)
print(f"Exit at tick {m5.curTick()}: {event.getCause()}")
if "maximum number of loads" not in event.getCause():
    raise RuntimeError("Memory checker did not finish")
