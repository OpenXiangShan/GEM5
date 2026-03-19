import argparse
import os
import sys

import m5
from m5.defines import buildEnv
from m5.objects import *
from m5.util import addToPath, fatal, warn
from m5.util.fdthelper import *

addToPath('../')

from ruby import Ruby
from common.LSQBankConflict import set_lsq_bank_conflict_cache_params

from common.FSConfig import *
from common.SysPaths import *
from common.Benchmarks import *
from common import Simulation
from common.Caches import *
from common.xiangshan import *

def setKmhV3Params(args, system):
    for cpu in system.cpu:

        # fetch (idealfetch not care)
        cpu.mmu.itb.size = 96
        cpu.fetchWidth = 32
        cpu.iewToFetchDelay = 2 # for resolved update, should train branch after squash
        cpu.commitToFetchDelay = 2
        cpu.fetchQueueSize = 64

        # decode
        cpu.decodeWidth = 8
        cpu.enable_loadFusion = False
        cpu.enableConstantFolding = False

        # rename
        cpu.renameWidth = 8
        cpu.numPhysIntRegs = 224
        cpu.numPhysFloatRegs = 256

        # dispatch
        cpu.enableDispatchStage = True
        cpu.numDQEntries = [8, 8, 8]
        cpu.dispWidth = [8, 8, 8]

        # scheduler
        cpu.scheduler = KMHV3Scheduler()

        # rob
        cpu.commitWidth = 12
        cpu.squashWidth = 12
        cpu.phyregReleaseWidth = 8
        cpu.RobCompressPolicy = 'kmhv3'
        cpu.numROBEntries = 160
        cpu.CROB_instPerGroup = 2 # 1 if not using ROB compression

        # lsu
        cpu.StoreWbStage = 4
        cpu.EnableLdMissReplay = True
        cpu.EnablePipeNukeCheck = True
        cpu.BankConflictCheck = True
        cpu.sbufferBankWriteAccurately = True
        cpu.DcacheSetDivNum = 2

        # lsq
        cpu.LQEntries = 128
        cpu.SQEntries = 64
        cpu.RARQEntries = 96
        cpu.RAWQEntries = 56
        cpu.LoadCompletionWidth = 8
        cpu.StoreCompletionWidth = 4
        cpu.RARDequeuePerCycle = 4
        cpu.RAWDequeuePerCycle = 4
        cpu.SbufferEntries = 24
        cpu.SbufferEvictThreshold = 16
        cpu.store_prefetch_train = False

        # branch predictor
        if args.bp_type == 'DecoupledBPUWithBTB':
            cpu.branchPred.ftq_size = 64
            cpu.branchPred.fsq_size = 64

        # l1 cache per core
        if args.caches:
            cpu.icache.size = '64kB'
            cpu.dcache.size = '64kB'
            cpu.dcache.tag_load_read_ports = 3
            cpu.dcache.mshrs = 16
            cpu.dcache.do_fast_writeline = False
            cpu.dcache.simulate_dcache_refill = True
            cpu.dcache.prefetch_can_offload = False
            if getattr(cpu.dcache, "prefetcher", None) is not None and \
                    hasattr(cpu.dcache.prefetcher, "enable_berti"):
                cpu.dcache.prefetcher.enable_berti = True
            set_lsq_bank_conflict_cache_params(cpu, system)

    # l2 caches
    if args.l2cache:
        for i in range(args.num_cpus):
            if args.classic_l2:
                system.l2_caches[i].slice_num = 4
                system.l2_caches[i].wpu = NULL
                system.l2_caches[i].do_fast_writeline = False
                system.l2_caches[i].prefetch_can_offload = False
                # Configure XSDRRIP replacement policy (DRRIP mode)
                # L2: 2MB, 8-way, 64B line → 4096 sets
                system.l2_caches[i].replacement_policy = XSDRRIPRP(mode=2, num_sets=4096)
            else:
                l2_wrapper = system.l2_wrappers[i]
                l2_wrapper.data_sram_banks = 2
                l2_wrapper.dir_sram_banks = 2
                l2_wrapper.pipe_dir_write_stage = 4
                l2_wrapper.dir_read_bypass = True
                if getattr(l2_wrapper, "prefetcher", None) is not None and \
                        hasattr(l2_wrapper.prefetcher, "enable_bop"):
                    l2_wrapper.prefetcher.enable_bop = False
                for j in range(args.l2_slices):
                    l2_wrapper.slices[j].inner_cache.wpu = NULL
                    l2_wrapper.slices[j].inner_cache.do_fast_writeline = False
                    l2_wrapper.slices[j].inner_cache.prefetch_can_offload = False
                    # Configure XSDRRIP replacement policy (DRRIP mode)
                    # Each slice: 2MB/4 = 512KB, 8-way, 64B line → 1024 sets
                    l2_wrapper.slices[j].inner_cache.replacement_policy = XSDRRIPRP(mode=2, num_sets=1024)
            system.tol2bus_list[i].forward_latency = 0  # 3->0
            system.tol2bus_list[i].response_latency = 0  # 3->0
            system.tol2bus_list[i].hint_wakeup_ahead_cycles = 0  # 1->0

            # Enable dual-port for DCache → L2 communication
            # ReqLayer[0]: ICache+DCache+ITB+DTB → L2, allow 2 requests per cycle
            # RespLayer[1]: L2 → DCache, allow 2 responses per cycle
            system.tol2bus_list[i].layer_bandwidth_configs = [
                LayerBandwidthConfig(direction="req", port_index=0, max_per_cycle=2),
                LayerBandwidthConfig(direction="resp", port_index=1, max_per_cycle=2),
            ]

    # l3 cache
    if args.l3cache:
        system.l3.mshrs = 64
        system.l3.do_fast_writeline = False
        system.l3.prefetch_can_offload = False
        system.l3.num_slices = 4

if __name__ == '__m5_main__':
    FutureClass = None

    args = xiangshan_system_init()

    assert not args.external_memory_system

    # Set default bp_type based on ideal_kmhv3 flag
    # If user didn't specify bp_type, set default based on ideal_kmhv3
    args.enable_pf_buffer = True
    args.bp_type = 'DecoupledBPUWithBTB'
    args.l2_size = '2MB'
    # args.kmh_align = True   # align prefetcher in RTL, spec06 decrease 1 score

    # Match the memories with the CPUs, based on the options for the test system
    TestMemClass = Simulation.setMemClass(args)

    test_sys = build_xiangshan_system(args)
    if args.raw_cpt and args.generic_rv_cpt and os.path.basename(args.generic_rv_cpt) == "linux.bin":
        configure_xiangshan_linux_workload(test_sys, args)
    # Set ideal parameters here with the highest priority, over command-line arguments
    setKmhV3Params(args, test_sys)

    root = Root(full_system=True, system=test_sys)

    Simulation.run_vanilla(args, root, test_sys, FutureClass)
