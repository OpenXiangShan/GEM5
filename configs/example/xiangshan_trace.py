# Copyright (c) 2024 The Regents of The University of Michigan
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

"""
XiangShan Trace-driven Simulation Configuration

This configuration script sets up a XiangShan O3CPU for trace-driven simulation,
enabling performance analysis using ChampSim or CBP2025 traces while maintaining
the full pipeline timing model.

Usage:
    ./build/RISCV/gem5.opt configs/example/xiangshan_trace.py \
        --trace-file=/path/to/trace.bin \
        --trace-format=champsim \
        --max-insts=1000000

Supported trace formats:
    - champsim: ChampSim binary instruction traces
    - cbp2025: CBP2025 branch prediction traces (future)
"""

import argparse
import sys
import os

import m5
from m5.defines import buildEnv
from m5.objects import *
from m5.util import addToPath, fatal, warn

addToPath('../')

from ruby import Ruby
from common.FSConfig import *
from common.SysPaths import *
from common.Benchmarks import *
from common import Simulation
from common import CacheConfig
from common import CpuConfig
from common import MemConfig
from common import ObjectList
from common import XSConfig
from common.Caches import *
from common import Options
from common.FUScheduler import *

# Import the base XiangShan configuration
from xiangshan import XiangshanCore

def build_test_system():
    """Build a minimal system for trace-driven simulation."""

    # Create the system
    system = System()

    # Set up basic system parameters
    system.clk_domain = SrcClockDomain()
    system.clk_domain.clock = '3GHz'  # XiangShan target frequency
    system.clk_domain.voltage_domain = VoltageDomain()

    # Set memory mode to timing for O3CPU
    system.mem_mode = 'timing'

    # Memory ranges - ensure physical memory covers the trace mapping region.
    # Start at 4KB (0x1000) to keep zero page unmapped (standard practice).
    system.mem_ranges = [AddrRange(start=0x1000, size='32GB')]

    # Create XiangShan CPU with trace mode enabled
    # For trace mode, we need to use a coupled (non-decoupled) branch predictor
    # to avoid FTQ blocking issues that prevent trace instruction fetching
    from m5.objects import TournamentBP

    system.cpu = XiangshanCore()

    # Enable trace mode - will be overridden by command line
    system.cpu.enableTraceMode = True
    system.cpu.traceFile = ""
    system.cpu.traceFormat = "champsim"

    # Branch predictor configuration for trace mode
    # By default, use coupled BP for compatibility, but allow decoupled BP option
    system.cpu.branchPred = TournamentBP()

    # New configuration options for decoupled BP in trace mode
    system.cpu.enableDecoupledBPInTrace = False  # Enable decoupled BP with trace
    system.cpu.traceCheckpointInterval = 64      # Checkpoint frequency for rollback
    system.cpu.traceBPValidation = True          # Enable BP prediction validation

    # Set up minimal memory system for trace simulation
    system.membus = SystemXBar()
    system.cpu.icache_port = system.membus.cpu_side_ports
    system.cpu.dcache_port = system.membus.cpu_side_ports

    # Try SimpleMemory instead of DDR controller to avoid TLB issues
    from m5.objects import SimpleMemory
    system.physmem = SimpleMemory()
    system.physmem.range = system.mem_ranges[0]
    system.physmem.port = system.membus.mem_side_ports

    # System port for functional access
    system.system_port = system.membus.cpu_side_ports

    # For trace-driven simulation, use proper RISC-V binary with correct memory settings
    from m5.objects import Process
    process = Process()
    hello_binary = "tests/test-progs/hello/bin/riscv/linux/hello"
    process.executable = hello_binary
    process.cmd = [hello_binary]
    process.kvmInSE = False

    # RISC-V specific: Skip useArchPT as it's not implemented for RISC-V

    system.cpu.workload = [process]

    # Create threads (sets up ISAs) and interrupt controller
    system.cpu.createThreads()
    system.cpu.createInterruptController()

    # Set up minimal system workload for SE mode (required for SE infra)
    from m5.objects import SEWorkload
    hello_binary = "tests/test-progs/hello/bin/riscv/linux/hello"
    system.workload = SEWorkload.init_compatible(hello_binary)

    # Set up work items for simulation control
    system.cpu.max_insts_any_thread = 1000000  # Default limit

    return system

def main():
    """Main function to set up and run trace-driven simulation."""

    parser = argparse.ArgumentParser(description='XiangShan Trace-driven Simulation')

    # Trace-specific options
    parser.add_argument('--trace-file', type=str, required=True,
                       help='Path to the trace file')
    parser.add_argument('--trace-format', type=str, default='champsim',
                       choices=['champsim', 'cbp2025'],
                       help='Trace format (default: champsim)')
    parser.add_argument('--max-insts', type=int, default=1000000,
                       help='Maximum instructions to simulate (default: 1M)')
    parser.add_argument('--debug-flags', type=str, default='',
                       help='Comma-separated debug flags (e.g., Fetch,TraceReader)')
    parser.add_argument('--stats-file', type=str, default='m5out/stats.txt',
                       help='Statistics output file')

    # Decoupled branch predictor options for trace mode
    parser.add_argument('--enable-decoupled-bp', action='store_true',
                       help='Enable decoupled branch predictor in trace mode')
    parser.add_argument('--trace-checkpoint-interval', type=int, default=64,
                       help='Checkpoint interval for trace rollback (default: 64)')
    parser.add_argument('--disable-bp-validation', action='store_true',
                       help='Disable branch predictor validation against trace')

    # Trace address mapping options (must align with TraceReader settings)
    parser.add_argument('--trace-addr-base', type=lambda x: int(x, 0), default=0x10000000,
                       help='Virtual base address for mapped trace addresses (default: 0x10000000)')
    parser.add_argument('--trace-addr-size', type=lambda x: int(x, 0), default=0x40000000,
                       help='Size of mapped trace address region in bytes (default: 0x40000000 = 1GB)')
    parser.add_argument('--trace-addr-map-mode', type=str, choices=['hash', 'linear'], default='hash',
                       help='Address mapping mode used by TraceReader (default: hash)')
    parser.add_argument('--no-trace-addr-page-align', action='store_true',
                       help='Disable page alignment when mapping trace addresses (default: aligned)')
    parser.add_argument('--trace-map-zero-page', action='store_true',
                       help='Map a single zero page (vaddr 0x0) to avoid null-pointer page faults in trace mode')

    args = parser.parse_args()

    # Validate trace file exists
    if not os.path.exists(args.trace_file):
        fatal(f"Trace file not found: {args.trace_file}")

    # Set up debug flags if specified
    if args.debug_flags:
        for flag in args.debug_flags.split(','):
            m5.debug.flags[flag.strip()].enable()

    # Build the system
    print(f"Building XiangShan system for trace simulation...")
    print(f"  Trace file: {args.trace_file}")
    print(f"  Trace format: {args.trace_format}")
    print(f"  Max instructions: {args.max_insts}")

    system = build_test_system()

    # Configure trace mode parameters
    system.cpu.traceFile = args.trace_file
    system.cpu.traceFormat = args.trace_format
    system.cpu.max_insts_any_thread = args.max_insts

    # Configure decoupled branch predictor options
    system.cpu.enableDecoupledBPInTrace = args.enable_decoupled_bp
    system.cpu.traceCheckpointInterval = args.trace_checkpoint_interval
    system.cpu.traceBPValidation = not args.disable_bp_validation

    # Apply trace address mapping parameters to the CPU (consumed by TraceReader)
    system.cpu.traceAddrBase = args.trace_addr_base
    system.cpu.traceAddrSize = args.trace_addr_size
    system.cpu.traceAddrMapMode = args.trace_addr_map_mode
    system.cpu.traceAddrPageAlign = (not args.no_trace_addr_page_align)
    # Optional: map zero page to avoid SE-mode faults from stray vaddr=0 accesses
    # (argument already declared above; avoid duplicate add_argument)

    # If decoupled BP is enabled, switch to decoupled branch predictor
    if args.enable_decoupled_bp:
        print("  Enabling decoupled branch predictor for trace mode")
        # TODO: Replace with actual decoupled BP configuration
        # from m5.objects import DecoupleBranch  # or appropriate decoupled BP
        # system.cpu.branchPred = DecoupleBranch()
        print("  WARNING: Decoupled BP configuration not yet implemented - using coupled BP")

    print(f"  Decoupled BP enabled: {args.enable_decoupled_bp}")
    print(f"  Checkpoint interval: {args.trace_checkpoint_interval}")
    print(f"  BP validation: {not args.disable_bp_validation}")
    print("  Trace address mapping:")
    print(f"    base=0x{args.trace_addr_base:x}, size=0x{args.trace_addr_size:x}, "
          f"mode={args.trace_addr_map_mode}, page_align={not args.no_trace_addr_page_align}")

    # Set up root and instantiate
    root = Root(full_system=False, system=system)

    # Instantiate all SimObjects
    m5.instantiate()

    # After instantiation, explicitly map the trace virtual address window into
    # the SE process page table to avoid page faults during LSQ translations.
    try:
        process = system.cpu.workload[0]
        # Identity map: vaddr == paddr for simplicity.
        process.map(args.trace_addr_base, args.trace_addr_base, int(args.trace_addr_size), True)
    except Exception as e:
        fatal(
            "Failed to map trace addr region into process page table: "
            f"base=0x{args.trace_addr_base:x}, size=0x{args.trace_addr_size:x}, error={e}"
        )

    # Optional: map zero page to avoid SE-mode faults from traces or special ops
    if args.trace_map_zero_page:
        try:
            # Map one page at vaddr 0x0 to a safe physical page (e.g., at 0x1000)
            process.map(0x0, 0x1000, 0x1000, True)
            print("  Zero page mapped: vaddr=0x0 -> paddr=0x1000 size=0x1000")
        except Exception as e:
            warn(f"Failed to map zero page: {e}")

    print("Starting trace-driven simulation...")
    print("This will replay the trace through the XiangShan pipeline")
    print("and generate detailed performance statistics.")

    # Run simulation
    exit_event = m5.simulate()

    # Print results
    print(f"Simulation completed: {exit_event.getCause()}")
    print(f"Simulated {system.cpu.totalInsts()} instructions")
    print(f"Simulation ticks: {m5.curTick()}")

    # Calculate basic performance metrics
    if system.cpu.totalInsts() > 0:
        cycles = m5.curTick() // system.clk_domain.clock.period
        ipc = float(system.cpu.totalInsts()) / float(cycles) if cycles > 0 else 0
        print(f"Performance: {ipc:.3f} IPC")

    print(f"Statistics written to: {args.stats_file}")

    return 0

if __name__ == '__m5_main__':
    sys.exit(main())
