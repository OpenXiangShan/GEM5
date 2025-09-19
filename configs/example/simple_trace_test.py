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
    parser = argparse.ArgumentParser(description='Simple XiangShan Trace Test')
    
    parser.add_argument('--trace-file', type=str, required=True,
                       help='Path to the trace file')
    parser.add_argument('--trace-format', type=str, default='champsim',
                       choices=['champsim', 'cbp2025'],
                       help='Trace format (default: champsim)')
    parser.add_argument('--max-insts', type=int, default=1000,
                       help='Maximum instructions to simulate')
    
    args = parser.parse_args()
    
    # Validate trace file exists
    if not os.path.exists(args.trace_file):
        fatal(f"Trace file not found: {args.trace_file}")
    
    # Create system
    system = System()
    system.clk_domain = SrcClockDomain()
    system.clk_domain.clock = '3GHz'
    system.clk_domain.voltage_domain = VoltageDomain()
    # Use a larger memory range for trace mode to accommodate arbitrary addresses
    system.mem_ranges = [AddrRange('4GB')]
    
    # Create CPU
    system.cpu = XiangshanCore()
    system.cpu.enableTraceMode = True
    system.cpu.traceFile = args.trace_file
    system.cpu.traceFormat = args.trace_format
    
    # For trace mode, create a minimal workload that doesn't interfere
    # We still need some workload for SE mode, but minimize its impact
    hello_binary = "tests/test-progs/hello/bin/riscv/linux/hello"
    if not os.path.exists(hello_binary):
        fatal(f"RISC-V test binary not found: {hello_binary}")
    
    # Create a minimal process - this is required for SE mode but won't actually run
    process = Process(pid=100)
    process.executable = hello_binary
    process.cmd = [hello_binary]
    process.cwd = os.getcwd()
    
    # Set up minimal workload for trace mode
    system.workload = SEWorkload.init_compatible(hello_binary)
    system.cpu.workload = process
    system.cpu.createThreads()
    
    # Simple memory system
    system.membus = SystemXBar()
    system.cpu.icache_port = system.membus.cpu_side_ports
    system.cpu.dcache_port = system.membus.cpu_side_ports
    
    # Memory controller with timing mode
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
    
    print(f"Starting simple trace test with {args.trace_file}")
    exit_event = m5.simulate()
    
    print(f"Simulation completed: {exit_event.getCause()}")
    print(f"Instructions simulated: {system.cpu.totalInsts()}")
    
    return 0

if __name__ == '__m5_main__':
    sys.exit(main())