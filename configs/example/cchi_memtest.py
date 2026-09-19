# Copyright (c) 2026
#
# Functional validation config for the CCHI integration (Phase 1).
#
# Topology under test:
#   MemTest (data-checking traffic tester)
#     -> L1 DCache (classic, untouched)
#       -> CCHIL1Agent (Taurus UpstreamNode)
#         -> CCHIFabric (Cohestra::Instance + Earth home + CacheLineDataMonitor)
#           -> membus -> MemCtrl (DRAM)
#
# The MemTester's own data model plus the fabric's CacheLineDataMonitor give
# end-to-end data-integrity checking without needing a RISC-V binary or a
# difftest reference.
#
# Run (from the repo root):
#   ./build/RISCV_CCHI/gem5.opt configs/example/cchi_memtest.py \
#       [--max-loads 100000] [--uncacheable 10] [--functional 10]

import argparse

import m5
from m5.objects import *
from m5.util import addToPath

addToPath('../')
addToPath('../../')

from common.Caches import L1_DCache

parser = argparse.ArgumentParser()
parser.add_argument("--max-loads", type=int, default=100000,
                    help="loads to execute before exiting (0 = run forever)")
parser.add_argument("--uncacheable", type=int, default=10,
                    help="percentage of uncacheable accesses")
parser.add_argument("--functional", type=int, default=0,
                    help="percentage of functional accesses. NOTE: functional "
                    "side-channel writes update gem5 memory outside CCHI "
                    "visibility (acknowledged approximation): the "
                    "CacheLineDataMonitor's scoreboard cannot track them and "
                    "will flag them as mismatches. Keep 0 for monitor-clean "
                    "validation runs.")
parser.add_argument("--snoop-merge", action="store_true",
                    help="enable the agent's snoop_merge path")
parser.add_argument("--flit-trace", action="store_true",
                    help="attach CHIron's CCHIFlitLogger ([cchi] verbose=1; "
                    "flit-level transaction log, very verbose)")
parser.add_argument("--num-cpus", type=int, default=1,
                    help="number of testers (each gets its own L1 + agent; "
                    ">1 exercises the home snoop paths)")
parser.add_argument("--downstream", type=str, default="earth",
                    choices=["earth", "rtl"],
                    help="CCHI downstream endpoint (rtl requires a "
                    "WITH_CCHI_RTL build)")
parser.add_argument("--interval", type=int, default=10,
                    help="MemTest request interval (cycles); raise to "
                    "de-rate the offered load")
args = parser.parse_args()

system = System()
system.clk_domain = SrcClockDomain(clock="2GHz",
                                   voltage_domain=VoltageDomain())
system.mem_mode = "timing"
system.mem_ranges = [AddrRange("2GB")]

# CCHI: one fabric (Earth or Verilator downstream) shared by all agents
system.cchi_fabric = CCHIFabric(clk_domain=system.clk_domain,
                                upstream_node_count=args.num_cpus,
                                downstream=args.downstream,
                                flit_trace=args.flit_trace,
                                memory_start=0x0,
                                memory_end=0x80000000)

system.membus = SystemXBar()
system.cchi_fabric.mem_side = system.membus.cpu_side_ports
system.system_port = system.membus.cpu_side_ports

# Per-core tester + L1 + agent. All testers share the same default address
# regions, so with num_cpus > 1 the home must snoop lines between agents.
# With shared regions the per-tester reference models race on writes to
# the same line (each tracks only its own writes), so their data check is
# only meaningful single-core; multi-core coherence is checked by the
# fabric's CacheLineDataMonitor instead.
for i in range(args.num_cpus):
    tester = MemTest(interval=args.interval,
                     percent_uncacheable=args.uncacheable,
                     percent_functional=args.functional,
                     max_loads=args.max_loads,
                     progress_interval=10000,
                     progress_check=1000000,
                     check_data=(args.num_cpus == 1))
    setattr(system, "tester%d" % i, tester)

    l1 = L1_DCache(size="64kB", assoc=8, clk_domain=system.clk_domain)
    setattr(system, "l1_%d" % i, l1)
    tester.port = l1.cpu_side

    agent = CCHIL1Agent(clk_domain=system.clk_domain,
                        fabric=system.cchi_fabric,
                        node_id=i,
                        snoop_merge=args.snoop_merge)
    setattr(system, "agent%d" % i, agent)
    l1.mem_side = agent.cpu_side
    agent.mem_side = system.membus.cpu_side_ports

system.mem_ctrl = MemCtrl()
system.mem_ctrl.dram = DDR4_2400_16x4(range=system.mem_ranges[0])
system.mem_ctrl.port = system.membus.mem_side_ports

root = Root(full_system=False, system=system)
m5.instantiate()

print("CCHI memtest: starting simulation (max_loads=%d)" % args.max_loads)
exit_event = m5.simulate()
print("Exiting @ tick %i because %s" % (m5.curTick(),
                                        exit_event.getCause()))
