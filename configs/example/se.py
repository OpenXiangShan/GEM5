# Copyright (c) 2012-2013 ARM Limited
# All rights reserved.
#
# The license below extends only to copyright in the software and shall
# not be construed as granting a license to any other intellectual
# property including but not limited to intellectual property relating
# to a hardware implementation of the functionality of the software
# licensed hereunder.  You may use the software subject to the license
# terms below provided that you ensure that this notice is replicated
# unmodified and in its entirety in all distributions of the software,
# modified or unmodified, in source code or in binary form.
#
# Copyright (c) 2006-2008 The Regents of The University of Michigan
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

# Simple test script
#
# "m5 test.py"

import argparse
import sys
import os

import m5
from m5.defines import buildEnv
from m5.objects import *
from m5.params import NULL
from m5.util import addToPath, fatal, warn

addToPath('../')

from ruby import Ruby

from common import Options
from common import Simulation
from common import CacheConfig
from common import CpuConfig
from common import ObjectList
from common import MemConfig
from common.FileSystemConfig import config_filesystem
from common.Caches import *
from common.cpu2000 import *
from common.FUScheduler import *

def get_processes(args):
    """Interprets provided args and returns a list of processes"""

    multiprocesses = []
    inputs = []
    outputs = []
    errouts = []
    pargs = []

    workloads = args.cmd.split(';')
    if args.input != "":
        inputs = args.input.split(';')
    if args.output != "":
        outputs = args.output.split(';')
    if args.errout != "":
        errouts = args.errout.split(';')
    if args.options != "":
        pargs = args.options.split(';')

    idx = 0
    for wrkld in workloads:
        process = Process(pid = 100 + idx)
        process.executable = wrkld
        process.cwd = os.getcwd()
        process.gid = os.getgid()

        if args.env:
            with open(args.env, 'r') as f:
                process.env = [line.rstrip() for line in f]

        if len(pargs) > idx:
            process.cmd = [wrkld] + pargs[idx].split()
        else:
            process.cmd = [wrkld]

        if len(inputs) > idx:
            process.input = inputs[idx]
        if len(outputs) > idx:
            process.output = outputs[idx]
        if len(errouts) > idx:
            process.errout = errouts[idx]

        multiprocesses.append(process)
        idx += 1

    if args.smt:
        assert(args.cpu_type == "DerivO3CPU")
        return multiprocesses, idx
    else:
        return multiprocesses, 1


parser = argparse.ArgumentParser()
Options.addCommonOptions(parser)
Options.addSEOptions(parser)

if '--ruby' in sys.argv:
    Ruby.define_options(parser)

def setDefaultArgs(args):
    """Set default configurations to match xiangshan.py SE mode defaults"""

    # Set defaults only if not already specified by user
    defaults = {
        'cpu_type': 'DerivO3CPU',
        'mem_size': '8GB',
        'mem_type': 'DRAMsim3',
        'caches': True,
        'cacheline_size': 64,
        'l1i_size': '64kB',
        'l1i_assoc': 8,
        'l1d_size': '64kB',
        'l1d_assoc': 4,
        'l1d_hwp_type': 'XSCompositePrefetcher',
        'l2cache': True,
        'l2_size': '1MB',
        'l2_assoc': 8,
        'l2_hwp_type': 'WorkerPrefetcher',
        'l3cache': True,
        'l3_size': '16MB',
        'l3_assoc': 16,
        'l3_hwp_type': 'WorkerPrefetcher',
        'l1_to_l2_pf_hint': True,
        'l2_to_l3_pf_hint': True,
        'bp_type': 'DecoupledBPUWithFTB',
        'enable_loop_predictor': True,
        'enable_jump_ahead_predictor': True,
        'warmup_insts_no_switch': 100000
    }   # default warmup 100k instructions!

    for key, value in defaults.items():
        # if not hasattr(args, key) or getattr(args, key) is None:
        setattr(args, key, value)

    # Set dramsim3_ini path
    if not hasattr(args, 'dramsim3_ini') or args.dramsim3_ini is None:
        root_dir = os.path.dirname(os.path.dirname(os.path.dirname(__file__)))
        args.dramsim3_ini = os.path.join(root_dir, 'ext/dramsim3/xiangshan_configs/xiangshan_DDR4_8Gb_x8_3200_2ch.ini')

args = parser.parse_args()

# Set default configurations
setDefaultArgs(args)

multiprocesses = []
numThreads = 1

if args.bench:
    apps = args.bench.split("-")
    if len(apps) != args.num_cpus:
        print("number of benchmarks not equal to set num_cpus!")
        sys.exit(1)

    for app in apps:
        try:
            if buildEnv['TARGET_ISA'] == 'arm':
                exec("workload = %s('arm_%s', 'linux', '%s')" % (
                        app, args.arm_iset, args.spec_input))
            else:
                exec("workload = %s(buildEnv['TARGET_ISA', 'linux', '%s')" % (
                        app, args.spec_input))
            multiprocesses.append(workload.makeProcess())
        except:
            print("Unable to find workload for %s: %s" %
                  (buildEnv['TARGET_ISA'], app),
                  file=sys.stderr)
            sys.exit(1)
elif args.cmd:
    multiprocesses, numThreads = get_processes(args)
else:
    print("No workload specified. Exiting!\n", file=sys.stderr)
    sys.exit(1)


(CPUClass, test_mem_mode, FutureClass) = Simulation.setCPUClass(args)
CPUClass.numThreads = numThreads

# Check -- do not allow SMT with multiple CPUs
if args.smt and args.num_cpus > 1:
    fatal("You cannot use SMT with multiple CPUs!")

np = args.num_cpus
mp0_path = multiprocesses[0].executable
system = System(cpu = [CPUClass(cpu_id=i) for i in range(np)],
                mem_mode = test_mem_mode,
                mem_ranges = [AddrRange(args.mem_size)],
                cache_line_size = args.cacheline_size)

if numThreads > 1:
    system.multi_thread = True

# Create a top-level voltage domain
system.voltage_domain = VoltageDomain(voltage = args.sys_voltage)

# Create a source clock for the system and set the clock period
system.clk_domain = SrcClockDomain(clock =  args.sys_clock,
                                   voltage_domain = system.voltage_domain)

# Create a CPU voltage domain
system.cpu_voltage_domain = VoltageDomain()

# Create a separate clock domain for the CPUs
system.cpu_clk_domain = SrcClockDomain(clock = args.cpu_clock,
                                       voltage_domain =
                                       system.cpu_voltage_domain)

# If elastic tracing is enabled, then configure the cpu and attach the elastic
# trace probe
if args.elastic_trace_en:
    CpuConfig.config_etrace(CPUClass, system.cpu, args)

# All cpus belong to a common cpu_clk_domain, therefore running at a common
# frequency.
for cpu in system.cpu:
    cpu.clk_domain = system.cpu_clk_domain
    # Add scheduler for RISCV CPUs
    if buildEnv['TARGET_ISA'] == 'riscv':
        cpu.scheduler = DefaultScheduler()

if ObjectList.is_kvm_cpu(CPUClass) or ObjectList.is_kvm_cpu(FutureClass):
    if buildEnv['TARGET_ISA'] == 'x86':
        system.kvm_vm = KvmVM()
        system.m5ops_base = 0xffff0000
        for process in multiprocesses:
            process.useArchPT = True
            process.kvmInSE = True
    else:
        fatal("KvmCPU can only be used in SE mode with x86")

# Sanity check
if args.simpoint_profile:
    if not ObjectList.is_noncaching_cpu(CPUClass):
        fatal("SimPoint/BPProbe should be done with an atomic cpu")
    if np > 1:
        fatal("SimPoint generation not supported with more than one CPUs")

for i in range(np):
    if args.smt:
        system.cpu[i].workload = multiprocesses
    elif len(multiprocesses) == 1:
        system.cpu[i].workload = multiprocesses[0]
    else:
        system.cpu[i].workload = multiprocesses[i]

    if args.simpoint_profile:
        system.cpu[i].addSimPointProbe(args.simpoint_interval)

    if args.checker:
        system.cpu[i].addCheckerCpu()

    if args.bp_type:
        bpClass = ObjectList.bp_list.get(args.bp_type)
        system.cpu[i].branchPred = bpClass()

    if args.indirect_bp_type:
        indirectBPClass = \
            ObjectList.indirect_bp_list.get(args.indirect_bp_type)
        system.cpu[i].branchPred.indirectBranchPred = indirectBPClass()

    system.cpu[i].createThreads()

def setKmhV3IdealParams(args, system):
    # Change to BTB for v3 and recreate branch predictors
    args.bp_type = 'DecoupledBPUWithBTB'

    for cpu in system.cpu:
        # Recreate branch predictor with BTB
        bpClass = ObjectList.bp_list.get('DecoupledBPUWithBTB')
        cpu.branchPred = bpClass()

        cpu.mmu.itb.size = 96

        cpu.fetchWidth = 32     # 64byte fetch block have up to 32 instructions
        cpu.commitToFetchDelay = 2
        cpu.fetchQueueSize = 64
        cpu.fetchToDecodeDelay = 2

        cpu.decodeWidth = 8
        cpu.renameWidth = 8
        cpu.commitWidth = 12
        cpu.squashWidth = 12
        cpu.replayWidth = 12
        cpu.LQEntries = 128
        cpu.SQEntries = 64
        cpu.SbufferEntries = 24
        cpu.SbufferEvictThreshold = 16
        # RAR/RAW replay queue thresholds
        cpu.RARQEntries = 96 # set 72 in the RTL model.
        cpu.RAWQEntries = 56 # set 32 in the RTL model.
        cpu.LoadCompletionWidth = 8
        cpu.StoreCompletionWidth = 4
        cpu.RARDequeuePerCycle = 4
        cpu.RAWDequeuePerCycle = 4
        cpu.numPhysIntRegs = 224
        cpu.numPhysFloatRegs = 256
        cpu.phyregReleaseWidth = 12
        cpu.RobCompressPolicy = 'kmhv3'
        cpu.numROBEntries = 160
        cpu.CROB_instPerGroup = 2 # 1 if not using ROB compression
        cpu.enableDispatchStage = True
        cpu.numDQEntries = [8, 8, 8]
        cpu.dispWidth = [8, 8, 8]
        cpu.scheduler = KMHV3Scheduler()

        cpu.BankConflictCheck = True   # real bank conflict 0.2 score
        # cpu.EnableLdMissReplay = False
        # cpu.EnablePipeNukeCheck = False
        cpu.StoreWbStage = 4 # store writeback at s4

        # enable constant folding
        cpu.enableConstantFolding = False

        # ideal decoupled frontend
        if args.bp_type == 'DecoupledBPUWithFTB' or args.bp_type == 'DecoupledBPUWithBTB':
            if args.bp_type == 'DecoupledBPUWithFTB':
                cpu.branchPred.enableTwoTaken = False
                cpu.branchPred.numBr = 8    # numBr must be a power of 2, see getShuffledBrIndex()
                cpu.branchPred.predictWidth = 64
                cpu.branchPred.uftb.numEntries = 1024
                cpu.branchPred.ftb.numEntries = 16384
                cpu.branchPred.tage.baseTableSize = 16384
                cpu.branchPred.tage.tableSizes = [2048] * 8
            else:
                cpu.branchPred.predictWidth = 64              # max width of a fetch block
                cpu.branchPred.btb.numEntries = 16384
                # TODO: BTB TAGE do not bave base table, do not support SC
                cpu.branchPred.tage.tableSizes = [2048] * 8  # 2 way, 2048 sets
                cpu.branchPred.tage.numWays = 2

            cpu.branchPred.tage.enableSC = False # TODO(bug): When numBr changes, enabling SC will trigger an assert
            cpu.branchPred.ftq_size = 256
            cpu.branchPred.fsq_size = 256
            cpu.branchPred.tage.numPredictors = 8
            cpu.branchPred.tage.TTagBitSizes = [13] * 8
            cpu.branchPred.tage.TTagPcShifts = [1] * 8
            cpu.branchPred.tage.histLengths = [4, 8, 15, 28, 50, 90, 160, 300]

        # ideal l1 caches
        if args.caches:
            cpu.icache.size = '64kB'
            cpu.dcache.size = '64kB'
            cpu.dcache.tag_load_read_ports = 100 # 3->100
            cpu.dcache.mshrs = 16

    if args.l2cache:
        for i in range(args.num_cpus):
            system.l2_caches[i].size = '2MB'
            system.l2_caches[i].slice_num = 0   # 4 -> 0, no slice
            system.tol2bus_list[i].forward_latency = 0  # 3->0
            system.tol2bus_list[i].response_latency = 0  # 3->0
            system.tol2bus_list[i].hint_wakeup_ahead_cycles = 0  # 2->0

    if args.l3cache:
        system.l3.mshrs = 128

if args.ruby:
    Ruby.create_system(args, False, system)
    assert(args.num_cpus == len(system.ruby._cpu_ports))

    system.ruby.clk_domain = SrcClockDomain(clock = args.ruby_clock,
                                        voltage_domain = system.voltage_domain)
    for i in range(np):
        ruby_port = system.ruby._cpu_ports[i]

        # Create the interrupt controller and connect its ports to Ruby
        # Note that the interrupt controller is always present but only
        # in x86 does it have message ports that need to be connected
        system.cpu[i].createInterruptController()

        # Connect the cpu's cache ports to Ruby
        ruby_port.connectCpuPorts(system.cpu[i])
else:
    MemClass = Simulation.setMemClass(args)
    system.membus = SystemXBar()
    system.system_port = system.membus.cpu_side_ports
    CacheConfig.config_cache(args, system)
    MemConfig.config_mem(args, system)
    config_filesystem(system, args)

system.workload = SEWorkload.init_compatible(mp0_path)

if args.wait_gdb:
    system.workload.wait_for_remote_gdb = True

# Set ideal parameters here with the highest priority, over command-line arguments
if args.ideal_kmhv3:
    setKmhV3IdealParams(args, system)

root = Root(full_system = False, system = system)
Simulation.run(args, root, system, FutureClass)
