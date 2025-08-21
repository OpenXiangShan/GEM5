# Copyright (c) 2016 Georgia Institute of Technology
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
#
# Author: Tushar Krishna

import m5
from m5.objects import *
from m5.defines import buildEnv
from m5.util import addToPath
import os, argparse, sys

addToPath('../')

from common import Options
from ruby import Ruby

# Get paths we might need.  It's expected this file is in m5/configs/example.
config_path = os.path.dirname(os.path.abspath(__file__))
config_root = os.path.dirname(config_path)
m5_root = os.path.dirname(config_root)

parser = argparse.ArgumentParser()
Options.addNoISAOptions(parser)

parser.add_argument("--synthetic", default="gemm",
                    choices=['gemm', 'uniform_random', 'tornado', 'bit_complement', \
                             'bit_reverse', 'bit_rotation', 'neighbor', \
                             'shuffle', 'transpose'])

parser.add_argument("--precision", type=int, default=3,
                    help="Number of digits of precision after decimal point\
                        for injection rate")

parser.add_argument("--sim-cycles", type=int, default=10000,
                    help="Number of simulation cycles")

parser.add_argument("--num-packets-max", type=int, default=-1,
                    help="Stop injecting after --num-packets-max.\
                        Set to -1 to disable.")

parser.add_argument("--single-sender-id", type=int, default=-1,
                    help="Only inject from this sender.\
                        Set to -1 to disable.")

parser.add_argument("--trace-file", default="", type=str, help="Trace file path")

#
# Add the ruby specific and protocol specific options
#
Ruby.define_options(parser)

args = parser.parse_args()

# default options
print("Overriding default options...")
# args.network = "garnet"
args.simple_physical_channels = True
args.chi_config = "configs/example/noc_config/GEMM.py"
args.num_cpus = 16
args.num_dirs = 8
# args.num_rows = 6
# args.num_columns = 4
args.num_l3caches = 16
args.router_link_latency = 0
args.node_link_latency = 1
args.mem_size = "8GB"

cpus = [ SimpleTrace(
    trace_file = args.trace_file if i == 0 else "",
    sim_cycles = args.sim_cycles,
    max_requests = 1
)
         for i in range(args.num_cpus) ]

# create the desired simulated system
system = System(cpu = cpus, mem_ranges = [AddrRange(start=0x80000000, size=args.mem_size)])


# Create a top-level voltage domain and clock domain
system.voltage_domain = VoltageDomain(voltage = args.sys_voltage)

system.clk_domain = SrcClockDomain(clock = args.sys_clock,
                                   voltage_domain = system.voltage_domain)


# This first Network.create_network
Ruby.create_system(args, False, system)

# Create a seperate clock domain for Ruby
    # which then calls GarnetNetwork() returns it
# Then CHI.create_system which returns topology
    # calls create topology
system.ruby.clk_domain = SrcClockDomain(clock = args.ruby_clock,
                                        voltage_domain = system.voltage_domain)

i = 0
for ruby_port in system.ruby._cpu_ports:
     #
     # Tie the cpu test ports to the ruby cpu port
     #
     cpus[i].test = ruby_port.in_ports
     i += 1

# -----------------------
# run simulation
# -----------------------

root = Root(full_system = False, system = system)
root.system.mem_mode = 'timing'

# Not much point in this being higher than the L1 latency
m5.ticks.setGlobalFrequency('1ps')

# instantiate configuration
m5.instantiate()

# simulate until program terminates
exit_event = m5.simulate(args.abs_max_tick)

print('Exiting @ tick', m5.curTick(), 'because', exit_event.getCause())
