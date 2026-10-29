# Copyright (c) 2026 Institute of Computing Technology, CAS
# SPDX-License-Identifier: BSD-3-Clause

"""Check ownership handoffs between two private bypassing L2 hierarchies.

Build shared.S twice with -DROLE=0 and -DROLE=1, then pass both binaries.
Long response latencies exercise snoops while exclusive fills are in flight.
"""

import argparse
from pathlib import Path

import m5
from m5.objects import (
    AddrRange,
    Cache,
    L2XBar,
    Process,
    Root,
    SDBPRP,
    SEWorkload,
    SimpleMemory,
    SrcClockDomain,
    System,
    SystemXBar,
    TimingSimpleCPU,
    VoltageDomain,
)

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("producer")
parser.add_argument("consumer")
parser.add_argument("--disable-bypass", action="store_true")
parser.add_argument(
    "--shared-reader",
    action="store_true",
    help="Use ordinary consumer reads to exercise downgrading snoops",
)
args = parser.parse_args()

system = System()
system.clk_domain = SrcClockDomain(
    clock="1GHz", voltage_domain=VoltageDomain()
)
system.mem_mode = "timing"
system.mem_ranges = [AddrRange("128MiB")]
system.workload = SEWorkload.init_compatible(args.producer)
system.cpu = [TimingSimpleCPU(cpu_id=i) for i in range(2)]
system.l2bus = [L2XBar() for _ in system.cpu]
system.l3bus = L2XBar(response_latency=10)
system.membus = SystemXBar()


def cache(size, level, **kwargs):
    return Cache(
        size=size,
        assoc=4,
        cache_level=level,
        tag_latency=2,
        data_latency=2,
        response_latency=7,
        mshrs=8,
        tgts_per_mshr=8,
        write_buffers=8,
        **kwargs,
    )


system.l2 = [
    cache(
        "4KiB",
        2,
        replacement_policy=SDBPRP(
            num_sets=16,
            sampler_num=4,
            sampler_assoc=6,
            sampler_protect_youngest_two=True,
            dead_threshold=0,
            enable_bypass=not args.disable_bypass,
        ),
    )
    for _ in system.cpu
]
for cpu, bus, l2, binary in zip(
    system.cpu, system.l2bus, system.l2, [args.producer, args.consumer]
):
    cpu.workload = Process(
        pid=100 + int(cpu.cpu_id), cmd=[str(Path(binary).resolve())]
    )
    cpu.createThreads()
    cpu.icache = cache("1KiB", 1, is_read_only=True)
    cpu.dcache = cache(
        "1KiB",
        1,
        sdbp_bypass_writable=not args.disable_bypass
        and not (args.shared_reader and int(cpu.cpu_id) == 1),
    )
    cpu.icache_port = cpu.icache.cpu_side
    cpu.dcache_port = cpu.dcache.cpu_side
    cpu.icache.mem_side = bus.cpu_side_ports
    cpu.dcache.mem_side = bus.cpu_side_ports
    l2.cpu_side = bus.mem_side_ports
    l2.mem_side = system.l3bus.cpu_side_ports
    cpu.createInterruptController()

system.l3 = cache("128KiB", 3)
system.l3.cpu_side = system.l3bus.mem_side_ports
system.l3.mem_side = system.membus.cpu_side_ports
system.memory = SimpleMemory(range=system.mem_ranges[0])
system.memory.port = system.membus.mem_side_ports
system.system_port = system.membus.cpu_side_ports
root = Root(full_system=False, system=system)
m5.instantiate()
for cpu in system.cpu:
    cpu.workload[0].map(0x20000000, 0x7000000, 4096, True)
event = m5.simulate(1000000000)
assert (
    event.getCause() == "exiting with last active thread context"
), event.getCause()
assert event.getCode() == 0, "Shared-data handoff was corrupted"
m5.stats.dump()
stats = {}
for line in (Path(m5.options.outdir) / "stats.txt").read_text().splitlines():
    fields = line.split()
    if len(fields) >= 2:
        try:
            stats[fields[0]] = float(fields[1])
        except ValueError:
            pass
for i in range(2):
    bypasses = stats.get(f"system.l2{i}.replacement_policy.bypasses", 0)
    fills = stats.get(f"system.cpu{i}.dcache.sdbpBypassFills", 0)
    if args.disable_bypass:
        assert bypasses == fills == 0
    elif args.shared_reader and i == 1:
        assert bypasses > 0 and fills == 0, "Shared data unexpectedly bypassed"
    else:
        assert bypasses > 0 and fills > 0, "Ownership bypass was not exercised"
print("SDBP shared ownership regression passed")
