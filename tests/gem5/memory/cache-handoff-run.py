"""Check data integrity when an L2 pipeline delays dirty ownership handoff.

Each MemTest owns different bytes of shared cache lines. Tiny L1 caches evict
those lines into L2; contention in the production L2 wrapper then overlaps a
buffered read hit with a younger invalidating snoop. MemTest checks every read
against its written data. --pipelined=0 exercises synchronous classic-cache hits.
"""

import argparse

import m5
from m5.objects import (
    NULL,
    AddrRange,
    Cache,
    L2CacheSlice,
    L2CacheWrapper,
    L2XBar,
    MemTest,
    Root,
    SimpleMemory,
    SrcClockDomain,
    System,
    SystemXBar,
    VoltageDomain,
)

parser = argparse.ArgumentParser()
parser.add_argument('--pipelined', type=int, choices=(0, 1), default=1)
args = parser.parse_args()
system = System(mem_mode='timing', mem_ranges=[AddrRange('16MB')])
system.voltage_domain = VoltageDomain()
system.clk_domain = SrcClockDomain(
    clock='1GHz', voltage_domain=system.voltage_domain)
system.cpu = [
    MemTest(max_loads=400000, size=4096, percent_reads=65,
            percent_functional=0, percent_uncacheable=0)
    for _ in range(8)
]
system.l1s = [
    Cache(size='128B', assoc=2, tag_latency=1, data_latency=1,
          response_latency=1, mshrs=8, tgts_per_mshr=16,
          writeback_clean=True, prefetcher=NULL)
    for _ in system.cpu
]
system.to_l2 = L2XBar(width=64, frontend_latency=0, forward_latency=0,
                     response_latency=1, snoop_response_latency=1)
system.l2 = Cache(size='4kB', assoc=4, tag_latency=1, data_latency=8,
                  response_latency=4, mshrs=16, tgts_per_mshr=24,
                  writeback_clean=True, prefetcher=NULL)
system.membus = SystemXBar()
system.memory = SimpleMemory(range=system.mem_ranges[0], latency='20ns')
for tester, cache in zip(system.cpu, system.l1s):
    tester.port = cache.cpu_side
    cache.mem_side = system.to_l2.cpu_side_ports
if args.pipelined:
    system.pipeline = L2CacheSlice()
    system.pipeline.setCacheAccessor(system.l2)
    system.wrapper = L2CacheWrapper(num_slices=1, cache_size='4kB',
                                    cache_assoc=4, prefetcher=NULL)
    system.wrapper.addCacheAccessor(system.l2)
    system.wrapper.addSliceAccessor(system.pipeline)
    system.wrapper.cpu_side = system.to_l2.mem_side_ports
    system.pipeline.cpu_side = system.wrapper.slice_cpuside_ports
    system.pipeline.inner_cpu_port = system.l2.cpu_side
    system.l2.mem_side = system.pipeline.inner_mem_port
    system.pipeline.mem_side = system.membus.cpu_side_ports
else:
    system.l2.cpu_side = system.to_l2.mem_side_ports
    system.l2.mem_side = system.membus.cpu_side_ports
system.memory.port = system.membus.mem_side_ports
system.system_port = system.membus.cpu_side_ports
root = Root(full_system=False, system=system)
m5.instantiate()
event = m5.simulate()
print('Cache handoff test:', event.getCause())
if event.getCause() != 'maximum number of loads reached':
    raise SystemExit(1)
