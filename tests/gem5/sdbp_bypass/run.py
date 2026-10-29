# Copyright (c) 2026 Institute of Computing Technology, CAS
# SPDX-License-Identifier: BSD-3-Clause

"""Run traffic.S in SE mode; force dead predictions to exercise every refill.

Compile traffic.S using its header command, then run:
  build/RISCV/gem5.opt tests/gem5/sdbp_bypass/run.py /tmp/sdbp-traffic
Repeat with --atomic and --disable-bypass to check both paths and the control.
For clean-only traffic compile with -DSDBP_READ_ONLY, then pass --clean-only.
Use --small-l3 to force L3 replacements and check real vs synthetic dirtiness.
"""

import argparse
import json
from pathlib import Path

import m5
from m5.objects import (
    AtomicSimpleCPU,
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
parser.add_argument("binary")
parser.add_argument("--atomic", action="store_true")
parser.add_argument("--disable-bypass", action="store_true")
parser.add_argument("--clean-only", action="store_true")
parser.add_argument("--small-l3", action="store_true")
parser.add_argument("--exclusive-l3", action="store_true")
args = parser.parse_args()

system = System()
system.clk_domain = SrcClockDomain(
    clock="1GHz", voltage_domain=VoltageDomain()
)
system.mem_mode = "atomic" if args.atomic else "timing"
system.mem_ranges = [m5.objects.AddrRange("128MiB")]
system.cpu = AtomicSimpleCPU() if args.atomic else TimingSimpleCPU()
system.cpu.workload = Process(cmd=[str(Path(args.binary).resolve())])
system.workload = SEWorkload.init_compatible(args.binary)
system.cpu.createThreads()


def cache(size, assoc, level, **kwargs):
    return Cache(
        size=size,
        assoc=assoc,
        cache_level=level,
        tag_latency=1,
        data_latency=1,
        response_latency=1,
        mshrs=16,
        tgts_per_mshr=8,
        write_buffers=16,
        writeback_clean=False,
        **kwargs,
    )


system.cpu.icache = cache("1KiB", 2, 1, is_read_only=True)
system.cpu.dcache = cache("1KiB", 2, 1)
system.l2bus = L2XBar()
system.l2 = cache(
    "4KiB",
    4,
    2,
    replacement_policy=SDBPRP(
        num_sets=16,
        sampler_num=4,
        sampler_assoc=4,
        dead_threshold=0,
        enable_bypass=not args.disable_bypass,
    ),
)
system.l3bus = L2XBar()
system.l3 = cache(
    "8KiB" if args.small_l3 else "128KiB",
    8,
    3,
    clusivity="mostly_excl" if args.exclusive_l3 else "mostly_incl",
)
system.membus = SystemXBar()
system.memory = SimpleMemory(range=system.mem_ranges[0])
system.cpu.icache_port = system.cpu.icache.cpu_side
system.cpu.dcache_port = system.cpu.dcache.cpu_side
system.cpu.icache.mem_side = system.l2bus.cpu_side_ports
system.cpu.dcache.mem_side = system.l2bus.cpu_side_ports
system.l2.cpu_side = system.l2bus.mem_side_ports
system.l2.mem_side = system.l3bus.cpu_side_ports
system.l3.cpu_side = system.l3bus.mem_side_ports
system.l3.mem_side = system.membus.cpu_side_ports
system.memory.port = system.membus.mem_side_ports
system.system_port = system.membus.cpu_side_ports
system.cpu.createInterruptController()
root = Root(full_system=False, system=system)
m5.instantiate()
event = m5.simulate(1000000000)
assert (
    event.getCause() == "exiting with last active thread context"
), event.getCause()
assert event.getCode() == 0, "Guest detected corrupted data"
m5.stats.dump()

stats = {}
for line in (Path(m5.options.outdir) / "stats.txt").read_text().splitlines():
    fields = line.split()
    if len(fields) >= 2:
        try:
            stats[fields[0]] = float(fields[1])
        except ValueError:
            pass


def stat(name):
    assert name in stats, f"Missing stat: {name}"
    return stats[name]


counts = {
    "bypasses": stat("system.l2.replacement_policy.bypasses"),
    "l1i_fills": stat("system.cpu.icache.sdbpBypassFills"),
    "l1d_fills": stat("system.cpu.dcache.sdbpBypassFills"),
    "l2_forwards": stat("system.l2.sdbpBypassWritebacks"),
    "l3_receives": stat("system.l3.sdbpBypassWritebacks"),
    "l3_memory_writebacks": stats.get("system.l3.writebacks::total", 0),
}
if args.disable_bypass:
    assert all(
        counts[key] == 0 for key in counts if key != "l3_memory_writebacks"
    )
else:
    assert all(
        counts[key] > 0 for key in counts if key != "l3_memory_writebacks"
    )
    assert counts["l2_forwards"] == counts["l3_receives"]
    assert stat("system.l2.tags.tagsInUse") == 0, "Dead refills allocated L2"
    clean_wbs = stats.get("system.l3.WritebackClean.accesses::writebacks", 0)
    dirty_wbs = stats.get("system.l3.WritebackDirty.accesses::writebacks", 0)
    counts["l3_clean_writebacks_received"] = clean_wbs
    counts["l3_dirty_writebacks_received"] = dirty_wbs
    assert clean_wbs > 0, "Clean bypass victims failed to reach L3"
    if args.exclusive_l3 and args.clean_only:
        # Exclusive L3 drops clean demand fills. Resident tags must therefore
        # have been installed by returning clean writebacks. The writeback
        # access path counts allocations as hits, so miss counters cannot
        # distinguish allocating writebacks from overwriting writebacks.
        resident_lines = stat("system.l3.tags.tagsInUse")
        counts["l3_average_resident_lines"] = resident_lines
        assert resident_lines > 0, "Clean bypass victims did not allocate L3"
    if args.clean_only:
        assert dirty_wbs == 0, "Read-only traffic created dirty writebacks"
    else:
        assert dirty_wbs > 0, "Modified data lost its dirty ownership"
if args.small_l3 and not args.clean_only:
    assert counts["l3_memory_writebacks"] > 0, "Dirty L3 victims were lost"
else:
    assert (
        counts["l3_memory_writebacks"] == 0
    ), "Unexpected L3 memory writeback"
print("SDBP bypass regression passed:", json.dumps(counts, sort_keys=True))
