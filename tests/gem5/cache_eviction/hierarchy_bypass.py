# Copyright (c) 2026 Institute of Computing Technology, CAS
# SPDX-License-Identifier: BSD-3-Clause

"""Check sampler observation and clean/dirty L1 bypass victim handling.

Run bypass_workload.c in a real three-level cache hierarchy. All L2 sets
are sampled, allowing every bypass decision to be matched to a sampler event.
The workload verifies data after eviction and reuse beyond LLC capacity.
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
    LRURP,
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
parser.add_argument("--bypass", action="store_true")
parser.add_argument("--l2-evict-invalidate-l1", action="store_true")
parser.add_argument(
    "--cache-hierarchy-policy",
    choices=("legacy", "xiangshan_single_core"),
    default="xiangshan_single_core",
)
parser.add_argument("--replacement", choices=("sdbp", "lru"), default="sdbp")
parser.add_argument("--llc-exclusive", action="store_true")
parser.add_argument("--deferred", action="store_true")
parser.add_argument("--read-only", action="store_true")
parser.add_argument("--l1-writeback-clean", action="store_true")
parser.add_argument("--threshold", type=int, default=0)
parser.add_argument("--max-memory-reads", type=int)
parser.add_argument("--mode", choices=("timing", "atomic"), default="timing")
args = parser.parse_args()

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
        cache_hierarchy_policy=args.cache_hierarchy_policy,
        tag_latency=1,
        data_latency=1,
        response_latency=1,
        mshrs=8,
        tgts_per_mshr=8,
        prefetcher=NULL,
        **kwargs,
    )


system.icache = cache(
    "2KiB",
    2,
    1,
    is_read_only=True,
    writeback_clean=args.l1_writeback_clean,
)
system.dcache = cache(
    "2KiB",
    2,
    1,
    writeback_clean=args.l1_writeback_clean,
)
system.l2bus = L2XBar()
system.l2 = cache(
    "8KiB",
    2,
    2,
    writeback_clean=True,
    l2_evict_invalidate_l1=args.l2_evict_invalidate_l1,
    replacement_policy=(
        SDBPRP(
            num_sets=64,
            sampler_num=64,
            sampler_assoc=6,
            dead_threshold=args.threshold,
            enable_bypass=args.bypass,
            # Force transport coverage independently of the admission policy.
            bypass_dead_victims=True,
        )
        if args.replacement == "sdbp"
        else LRURP()
    ),
)
system.l3bus = L2XBar()
system.l3 = cache(
    "64KiB",
    4,
    3,
    clusivity="mostly_excl" if args.llc_exclusive else "mostly_incl",
)
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
event = m5.simulate()
assert event.getCause() == "exiting with last active thread context", event
assert event.getCode() == 0, event.getCode()
m5.drain()
m5.stats.dump()
stats = {}
for line in (Path(m5.options.outdir) / "stats.txt").read_text().splitlines():
    fields = line.split()
    if len(fields) > 1:
        try:
            stats[fields[0]] = float(fields[1])
        except ValueError:
            pass

policy = "system.l2.replacement_policy."
xs_policy = args.cache_hierarchy_policy == "xiangshan_single_core"
assert not args.bypass or args.replacement == "sdbp"
bypasses = stats.get(policy + "bypasses", 0)
sampled = stats.get(policy + "samplerBypassAccesses", 0)
assert sampled == bypasses, (sampled, bypasses)
if args.replacement == "sdbp":
    if not args.read_only:
        assert stats[policy + "liveTraining"] > 0
    assert stats[policy + "deadTraining"] > 0
for command in ("clean", "dirty"):
    suffix = f".bypassL2Writebacks::{command}"
    l1 = sum(stats[f"system.{name}{suffix}"] for name in ("icache", "dcache"))
    l2 = stats[f"system.l2{suffix}"]
    l3 = stats[f"system.l3{suffix}"]
    if (
        xs_policy
        and args.bypass
        and (command == "clean" or not args.read_only)
    ):
        assert l1 > 0 and l2 > 0 and l3 > 0, (command, l1, l2, l3)
        if args.read_only:
            assert l1 == l2 == l3, (command, l1, l2, l3)
    else:
        assert l1 == l2 == l3 == 0, (command, l1, l2, l3)
clean_evictions = stats["system.dcache.bypassL2CleanEvictions"]
if xs_policy and args.bypass:
    assert bypasses > 1000, bypasses
    assert stats["system.dcache.bypassL2Fills"] > 1000

else:
    assert stats["system.dcache.bypassL2Fills"] == 0
    if not args.bypass:
        assert bypasses == 0
# Marked clean victims now carry data; ordinary clean victims stay control-only.
assert clean_evictions == 0, clean_evictions
assert stats["system.l3.bypassL2Fills"] == 0
if xs_policy:
    # Ordinary reads remain shared requests, but the grants are writable.
    for bus in ("l2bus", "l3bus"):
        read_ex = stats.get(f"system.{bus}.transDist::ReadExReq", 0)
        store_misses = stats.get("system.dcache.WriteReq.misses::total", 0)
        # The L2 eviction experiment adds express ReadExReq probes.
        if not args.l2_evict_invalidate_l1:
            assert read_ex <= store_misses, (bus, read_ex, store_misses)
        assert stats.get(f"system.{bus}.transDist::ReadSharedReq", 0) > 1000
        if args.read_only and not args.l2_evict_invalidate_l1:
            assert read_ex == 0, (bus, read_ex)
        assert stats.get(f"system.{bus}.transDist::UpgradeReq", 0) == 0
    assert stats["system.l2.defaultWritableGrants"] > 1000
    assert stats["system.l3.defaultWritableGrants"] > 1000
    if args.mode == "timing":
        assert stats["system.dcache.writableReadExpectations"] > 1000
        if args.deferred:
            assert stats["system.l2.deferredWritableGrants"] > 0
    else:
        assert stats["system.dcache.writableReadExpectations"] == 0
    if args.read_only:
        # Every L1 data writeback must be a marked clean bypass victim.
        # Unmarked clean lines still generate only CleanEvict.
        for cache_name in ("icache", "dcache"):
            prefix = f"system.{cache_name}"
            assert stats.get(f"{prefix}.writebacks::total", 0) == stats.get(
                f"{prefix}.bypassL2Writebacks::clean", 0
            )
else:
    for level in ("icache", "dcache", "l2", "l3"):
        assert stats[f"system.{level}.defaultWritableGrants"] == 0
        assert stats[f"system.{level}.deferredWritableGrants"] == 0
        assert stats[f"system.{level}.writableReadExpectations"] == 0
assert stats["system.membus.transDist::ReadResp"] > 0
assert stats.get("system.membus.transDist::WritebackClean", 0) == 0
if args.read_only:
    assert stats["system.dcache.bypassL2Writebacks::dirty"] == 0
    # The bus histogram omits command bins with a zero count.
    assert stats.get("system.membus.transDist::WritebackDirty", 0) == 0
if args.max_memory_reads is not None:
    # A repeated clean footprint that fits LLC must survive L2 bypass.
    reads = stats["system.memory.numReads::total"]
    assert reads <= args.max_memory_reads, (reads, args.max_memory_reads)
report = {
    k: v
    for k, v in stats.items()
    if "bypassL2" in k
    or "samplerBypass" in k
    or "WritableGrant" in k
    or "writableReadExpectation" in k
}
report.update(
    bypasses=bypasses, samplerAccesses=stats.get(policy + "samplerAccesses", 0)
)
(Path(m5.options.outdir) / "bypass-check.json").write_text(
    json.dumps(report, indent=2) + "\n"
)
print("Bypass sampler and writeback checks PASS")
