#!/usr/bin/env python3

import argparse
import sys
import os

import m5
from m5.defines import buildEnv
from m5.objects import *
from m5.util import addToPath, fatal, warn

addToPath('../')

from common import Options
from common import Simulation
from common import CacheConfig
from common import CpuConfig
from common import MemConfig
from common.Caches import *
from xiangshan import XiangshanCore

def main():
    parser = argparse.ArgumentParser(description='Trace-Only Test for ChampSim traces')
    
    parser.add_argument('--trace-file', type=str, required=True,
                       help='Path to the trace file')
    parser.add_argument('--trace-format', type=str, default='champsim',
                       choices=['champsim', 'cbp2025'],
                       help='Trace format (default: champsim)')
    parser.add_argument('--max-insts', type=int, default=10000,
                       help='Maximum instructions to simulate')
    parser.add_argument('--debug', action='store_true',
                       help='Enable debug output')
    
    args = parser.parse_args()
    
    # Validate trace file exists
    if not os.path.exists(args.trace_file):
        fatal(f"Trace file not found: {args.trace_file}")
    
    # Create system with minimal configuration
    system = System()
    system.clk_domain = SrcClockDomain()
    system.clk_domain.clock = '3GHz'
    system.clk_domain.voltage_domain = VoltageDomain()
    # Large memory range for trace addresses
    system.mem_ranges = [AddrRange('4GB')]
    
    # Create CPU with trace mode enabled
    system.cpu = XiangshanCore()
    system.cpu.enableTraceMode = True
    system.cpu.traceFile = args.trace_file
    system.cpu.traceFormat = args.trace_format
    
    # Minimal workload setup - just enough to make GEM5 happy
    # We'll use a simple RISC-V binary but trace mode will override execution
    hello_binary = "tests/test-progs/hello/bin/riscv/linux/hello"
    if not os.path.exists(hello_binary):
        # Try alternative path
        hello_binary = "/nfs/home/goulingrui/project/GEM5/tests/test-progs/hello/bin/riscv/linux/hello"
        if not os.path.exists(hello_binary):
            fatal(f"RISC-V test binary not found at expected locations")
    
    # Create process but it won't actually execute due to trace mode
    process = Process(pid=100)
    process.executable = hello_binary
    process.cmd = [hello_binary]
    process.cwd = os.getcwd()
    
    # Set up workload
    system.workload = SEWorkload.init_compatible(hello_binary)
    system.cpu.workload = process
    system.cpu.createThreads()
    
    # Simple memory system
    system.membus = SystemXBar()
    system.cpu.icache_port = system.membus.cpu_side_ports
    system.cpu.dcache_port = system.membus.cpu_side_ports
    
    # Memory controller
    system.mem_ctrl = MemCtrl()
    system.mem_ctrl.dram = DDR4_2400_16x4()
    system.mem_ctrl.dram.range = system.mem_ranges[0]
    system.mem_ctrl.port = system.membus.mem_side_ports
    
    system.system_port = system.membus.cpu_side_ports
    
    # Set memory mode to timing (required for O3 CPU)
    system.mem_mode = 'timing'
    
    # Create interrupt controller
    system.cpu.createInterruptController()
    
    # Set instruction limit
    system.cpu.max_insts_any_thread = args.max_insts
    
    # Create root and run
    root = Root(full_system=False, system=system)
    m5.instantiate()
    
    print(f"Starting trace-only test with {args.trace_file}")
    print(f"Max instructions: {args.max_insts}")
    
    try:
        exit_event = m5.simulate()
        print(f"Simulation completed: {exit_event.getCause()}")
        print(f"Instructions simulated: {system.cpu.totalInsts()}")
        
        # Print basic statistics
        print("\n=== Basic Statistics ===")
        print(f"Total instructions: {system.cpu.totalInsts()}")
        if hasattr(system.cpu, 'commitStats'):
            print(f"Committed instructions: {system.cpu.commitStats.committedInsts}")
        
    except Exception as e:
        print(f"Simulation failed: {e}")
        return 1
    
    return 0

if __name__ == '__m5_main__':
    sys.exit(main())