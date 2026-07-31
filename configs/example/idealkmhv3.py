import argparse
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

from m5.objects.ValuePredictor import *

def setPtwLevelLimitParams(args, tlb):
    tlb.walker.enable_ptw_level_limit = args.enable_ptw_level_limit
    tlb.walker.ptw_level0_limit = args.ptw_level0_limit
    tlb.walker.ptw_level1_limit = args.ptw_level1_limit
    tlb.walker.ptw_level2_limit = args.ptw_level2_limit
    tlb.walker.ptw_level3_limit = args.ptw_level3_limit
    tlb.walker.ptw_miss_queue_size = args.ptw_miss_queue_size

def setKmhV3IdealParams(args, system):
    for cpu in system.cpu:

        # fetch
        #cpu.mmu.itb.enable_l1_direct_compression = args.enable_l1_direct_compression
        #cpu.mmu.dtb.enable_l1_direct_compression = args.enable_l1_direct_compression
        setPtwLevelLimitParams(args, cpu.mmu.itb)
        setPtwLevelLimitParams(args, cpu.mmu.dtb)
        cpu.fetchWidth = 32
        cpu.iewToFetchDelay = 2 # for resolved update, should train branch after squash
        cpu.commitToFetchDelay = 4  # maybe we need to change iewToFetchDelay to 4, but now we use commit update bpu
        cpu.fetchQueueSize = 64

        # decode
        cpu.fetchToDecodeDelay = 3
        cpu.decodeWidth = 8
        cpu.enable_loadFusion = False
        cpu.enableConstantFolding = False

        # rename
        cpu.renameWidth = 8
        cpu.numPhysIntRegs = 224
        cpu.numPhysFloatRegs = 256

        # dispatch
        cpu.enableDispatchStage = False
        cpu.numDQEntries = [8, 8, 8]
        cpu.dispWidth = [8, 8, 8]

        # scheduler
        cpu.scheduler = KMHV3Scheduler()

        # rob
        cpu.commitWidth = 8
        cpu.squashWidth = 8
        cpu.phyregReleaseWidth = 8
        cpu.RobCompressPolicy = 'kmhv3'
        cpu.numROBEntries = args.ROBTotalEntry
        cpu.CROB_instPerGroup = 2 # 1 if not using ROB compression
        cpu.smtBorrowDonorReserveEntries = args.smtROBDonorEntry
        cpu.smtBorrowBaseReserveEntries = args.smtROBBaseEntry

        # lsu
        cpu.StoreWbStage = 4
        cpu.EnableLdMissReplay = True
        cpu.EnablePipeNukeCheck = True
        cpu.BankConflictCheck = True
        cpu.sbufferBankWriteAccurately = True
        cpu.DcacheSetDivNum = 2

        # value predictor
        cpu.valuePred = CompositeValuePredictor(
                            predictors=[
                                IdealConstantLVP(),
                                # ExampleValuePredictor(),
                                # EStride(logMaxConfidence=13, thresholdPercent=0.35)
                            ],
                            arb=CVPConfidenceArb(counterBits=6)
                        )

        # lsq
        cpu.LQEntries = 120
        cpu.SQEntries = 64
        cpu.RARQEntries = 96
        cpu.RAWQEntries = 56
        cpu.LoadCompletionWidth = 8
        cpu.StoreCompletionWidth = 4
        cpu.RARDequeuePerCycle = 4
        cpu.RAWDequeuePerCycle = 4
        cpu.SbufferEntries = 16
        cpu.SbufferEvictThreshold = 8
        cpu.store_prefetch_train = False

        # branch predictor
        if args.bp_type == 'DecoupledBPUWithBTB':
            cpu.branchPred.ftq_size = 64
            cpu.branchPred.fsq_size = 64
            # TAGE table sizes and numWays tunning
            cpu.branchPred.tage.tableSizes = [2048, 2048, 8192, 8192, 8192, 8192, 8192, 2048]
            cpu.branchPred.tage.numWays = [2, 2, 4, 2, 2, 2, 2, 2]
            # cpu.branchPred.microtage.enabled = False

        # l1 cache per core
        if args.caches:
            cpu.icache.size = '64kB'
            cpu.dcache.size = '64kB'
            cpu.dcache.tag_load_read_ports = 100
            cpu.dcache.mshrs = 16
            cpu.dcache.simulate_dcache_refill = True
            set_lsq_bank_conflict_cache_params(cpu, system)

    # l2 caches
    if args.l2cache:
        for i in range(args.num_cpus):
            if args.classic_l2:
                system.l2_caches[i].wpu = NULL
                system.l2_caches[i].slice_num = 0 # 4 -> 0, no slice
            else:
                l2_wrapper = system.l2_wrappers[i]
                l2_wrapper.data_sram_banks = 2
                l2_wrapper.dir_sram_banks = 2
                l2_wrapper.pipe_dir_write_stage = 3
                l2_wrapper.dir_read_bypass = False
                for j in range(args.l2_slices):
                    l2_wrapper.slices[j].inner_cache.wpu = NULL
                    # Configure XSDRRIP replacement policy (DRRIP mode)
                    # Each slice: 2MB/4 = 512KB, 8-way, 64B line → 1024 sets
                    l2_wrapper.slices[j].inner_cache.replacement_policy = XSDRRIPRP(mode=2, num_sets=1024)
            system.tol2bus_list[i].forward_latency = 3  # 0->3
            system.tol2bus_list[i].response_latency = 3  # 0->3
            system.tol2bus_list[i].hint_wakeup_ahead_cycles = 1  # 0->1

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
        system.l3.num_slices = 4

if __name__ == '__m5_main__':
    FutureClass = None

    args = xiangshan_system_init()

    assert not args.external_memory_system

    # Set default bp_type based on ideal_kmhv3 flag
    # If user didn't specify bp_type, set default based on ideal_kmhv3
    args.bp_type = 'DecoupledBPUWithBTB'
    args.l2_size = '2MB'
    args.l3_size = '32MB'
    # Match the memories with the CPUs, based on the options for the test system
    TestMemClass = Simulation.setMemClass(args)

    test_sys = build_xiangshan_system(args)
    # Set ideal parameters here with the highest priority, over command-line arguments
    setKmhV3IdealParams(args, test_sys)

    root = Root(full_system=True, system=test_sys)

    Simulation.run_vanilla(args, root, test_sys, FutureClass)
