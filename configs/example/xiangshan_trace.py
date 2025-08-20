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
    
    # Memory ranges
    system.mem_ranges = [AddrRange('4GB')]
    
    # Create XiangShan CPU with trace mode enabled
    system.cpu = XiangshanCore()
    
    # Enable trace mode - will be overridden by command line
    system.cpu.enableTraceMode = True
    system.cpu.traceFile = ""
    system.cpu.traceFormat = "champsim"
    
    # Set up minimal memory system for trace simulation
    system.membus = SystemXBar()
    system.cpu.icache_port = system.membus.cpu_side_ports
    system.cpu.dcache_port = system.membus.cpu_side_ports
    
    # Create simple memory controller
    system.mem_ctrl = MemCtrl()
    system.mem_ctrl.dram = DDR4_2400_16x4()
    system.mem_ctrl.dram.range = system.mem_ranges[0]
    system.mem_ctrl.port = system.membus.mem_side_ports
    
    # System port for functional access
    system.system_port = system.membus.cpu_side_ports
    
    # Create simple interrupt controller
    system.cpu.createInterruptController()
    
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
    
    # Set up root and instantiate
    root = Root(full_system=False, system=system)
    
    # Instantiate all SimObjects
    m5.instantiate()
    
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