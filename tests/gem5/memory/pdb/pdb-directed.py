# SPDX-License-Identifier: BSD-3-Clause
"""Real Cache/PDB coherence tests with a deterministic address stimulus."""
import argparse

import m5
from m5.objects import (
    AddrRange,
    ArchDBer,
    Cache,
    L2XBar,
    PdbTest,
    PdbTestPrefetcher,
    Root,
    SimpleMemory,
    SrcClockDomain,
    System,
    SystemXBar,
    VoltageDomain,
)

parser = argparse.ArgumentParser()
parser.add_argument("--branch", action="store_true")
parser.add_argument("--whole", action="store_true")
parser.add_argument("--evict-lower", action="store_true")
parser.add_argument("--probe", action="store_true")
parser.add_argument("--pdb-off", action="store_true")
parser.add_argument("--owned-store", action="store_true")
args = parser.parse_args()

system = System(mem_mode="timing", mem_ranges=[AddrRange("1MiB")])
system.voltage_domain = VoltageDomain()
system.clk_domain = SrcClockDomain(
    clock="1GHz", voltage_domain=system.voltage_domain
)
system.arch_db = ArchDBer(arch_db_file=m5.options.outdir + "/arch.db")
system.memory = SimpleMemory(range=system.mem_ranges[0], latency="20ns")
system.membus = SystemXBar()
system.l2bus = L2XBar()


def cache(level, size):
    return Cache(
        size=size,
        assoc=2,
        tag_latency=2,
        data_latency=2,
        response_latency=2,
        mshrs=8,
        tgts_per_mshr=8,
        write_buffers=8,
        cache_level=level,
    )


system.l2 = cache(2, "1KiB")
system.l1 = cache(1, "2KiB")
system.l1.pdb_entries = 0 if args.pdb_off else 64
system.l1.prefetcher = PdbTestPrefetcher(
    on_read=True, on_write=False, on_miss=True, prefetch_on_pf_hit=False
)
system.peer = cache(1, "2KiB")
system.l1.mem_side = system.l2bus.cpu_side_ports
system.peer.mem_side = system.l2bus.cpu_side_ports
system.l2.cpu_side = system.l2bus.mem_side_ports
system.l2.mem_side = system.membus.cpu_side_ports
system.memory.port = system.membus.mem_side_ports
system.system_port = system.membus.cpu_side_ports

seed = "dirty_branch" if args.branch else "dirty"
initial_read = "shared_read" if args.branch else "read"
# Establish a real downstream holder before injecting a dirty eviction.
actions = [
    f"lower,{initial_read},0x1040,64,0,20",
    f"lower,{seed},0x1040,64,17,100",
    "cpu,read,0x1000,64,0,100",
    "cpu,read,0x1040,64,0,20",
]
if args.evict_lower:
    actions += [
        f"lower,read,{hex(0x1040 + i * 1024)},64,0,1" for i in range(1, 5)
    ]
if args.probe:
    actions += ["peer,read,0x1040,64,0,20"]
size = 64 if args.whole else 1
actions += [
    f"cpu,store,0x1040,{size},34,100",
    "peer,read,0x1040,64,0,30",
]
if args.owned_store:
    # A local write to Owned must upgrade without fetching stale lower data.
    actions += [
        "cpu,store,0x1040,1,51,100",
        "cpu,read,0x1040,64,0,30",
    ]
actions += [
    "peer,store,0x1040,1,68,100",
    "cpu,read,0x1040,64,0,100",
]
# Register the test requestor before caches size their per-requestor arrays.
system.a_tester = PdbTest(actions=actions)
system.a_tester.port = system.l1.cpu_side
system.a_tester.peer_port = system.peer.cpu_side
system.a_tester.lower_port = system.l2bus.cpu_side_ports
root = Root(full_system=False, system=system)
m5.instantiate()
exit_event = m5.simulate(100000000)
m5.stats.dump()
assert (
    exit_event.getCause() == "PDB directed test passed"
), exit_event.getCause()
print("PDB directed test passed", vars(args))
