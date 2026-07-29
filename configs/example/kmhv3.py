import argparse
import os
import sys

import m5
from m5.defines import buildEnv
from m5.objects import *
from m5.util import addToPath, fatal, warn
from m5.util.fdthelper import *

addToPath('../')
addToPath('../../')

from ruby import Ruby
from common.LSQBankConflict import set_lsq_bank_conflict_cache_params

from common.FSConfig import *
from common.SysPaths import *
from common.Benchmarks import *
from common import Simulation
from common.Caches import *
from common.xiangshan import *
from util.solver.runtime.integration import maybe_handle_solver_runtime

def setPtwLevelLimitParams(args, tlb):
    tlb.walker.enable_ptw_level_limit = args.enable_ptw_level_limit
    tlb.walker.ptw_level0_limit = args.ptw_level0_limit
    tlb.walker.ptw_level1_limit = args.ptw_level1_limit
    tlb.walker.ptw_level2_limit = args.ptw_level2_limit
    tlb.walker.ptw_level3_limit = args.ptw_level3_limit
    tlb.walker.ptw_miss_queue_size = args.ptw_miss_queue_size

def setKmhV3Params(args, system):
    for cpu in system.cpu:

        # fetch (idealfetch not care)
        cpu.mmu.itb.size = 96
        cpu.mmu.itb.enable_l1_direct_compression = args.enable_l1_direct_compression
        cpu.mmu.dtb.enable_l1_direct_compression = args.enable_l1_direct_compression
        setPtwLevelLimitParams(args, cpu.mmu.itb)
        setPtwLevelLimitParams(args, cpu.mmu.dtb)
        cpu.fetchWidth = 32
        cpu.iewToFetchDelay = 4 # for resolved update, should train branch after squash
        cpu.commitToFetchDelay = 4
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
        cpu.enable_storeSet_train = True

        # dispatch
        cpu.enableDispatchStage = False
        cpu.numDQEntries = [8, 8, 8]
        cpu.dispWidth = [8, 8, 8]

        # scheduler
        cpu.scheduler = KMHV3Scheduler()
        cpu.scheduler.disableAllRegArb()
        cpu.scheduler.enableMainRdpOpt = False
        cpu.scheduler.intRegfileBanks = 1
        # intiq0
        cpu.scheduler.IQs[0].oports[0].rp = [IntRD(0, 0), IntRD(1, 0)]
        cpu.scheduler.IQs[0].oports[1].rp = [IntRD(0, 1), IntRD(1, 1)]

        # intiq1
        cpu.scheduler.IQs[1].oports[0].rp = [IntRD(2, 0), IntRD(3, 0)]
        cpu.scheduler.IQs[1].oports[1].rp = [IntRD(2, 1), IntRD(3, 1)]

        # intiq2
        cpu.scheduler.IQs[2].oports[0].rp = [IntRD(4, 0), IntRD(5, 0)]
        cpu.scheduler.IQs[2].oports[1].rp = [IntRD(4, 1), IntRD(5, 1)]

        # rob
        cpu.commitWidth = 8
        cpu.squashWidth = 8
        cpu.phyregReleaseWidth = 8
        cpu.RobCompressPolicy = 'none'
        cpu.numROBEntries = 352
        cpu.CROB_instPerGroup = 2 # 1 if not using ROB compression
        cpu.robWalkPolicy = args.rob_walk_policy

        # lsu
        cpu.StoreWbStage = 4
        cpu.EnableLdMissReplay = True
        cpu.EnablePipeNukeCheck = True
        cpu.BankConflictCheck = True
        cpu.sbufferBankWriteAccurately = False

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

            # Align the parameter-visible BPU structures with RTL DefaultConfig.
            # Banked storage, history hashing, and compressed targets still
            # require model-level alignment in the corresponding components.
            cpu.branchPred.microtage.numPredictors = 2
            cpu.branchPred.microtage.tableSizes = [512, 512]
            cpu.branchPred.microtage.TTagBitSizes = [8, 8]
            cpu.branchPred.microtage.TTagPcShifts = [1, 1]
            cpu.branchPred.microtage.histLengths = [5, 9]
            cpu.branchPred.microtage.numWays = 1
            cpu.branchPred.microtage.baseTableSize = 512
            cpu.branchPred.microtage.numBanks = 4

            cpu.branchPred.ras.numEntries = 16
            cpu.branchPred.ras.numInflightEntries = 32
            cpu.branchPred.ras.ctrWidth = 3

            cpu.branchPred.abtb.numEntries = 1024
            cpu.branchPred.abtb.numWays = 4
            cpu.branchPred.abtb.tagBits = 24
            cpu.branchPred.abtb.blockSize = 64
            cpu.branchPred.abtb.entryHalfAligned = True

            cpu.branchPred.mbtb.numEntries = 8192
            cpu.branchPred.mbtb.numWays = 4
            cpu.branchPred.mbtb.tagBits = 16
            cpu.branchPred.mbtb.blockSize = 32
            cpu.branchPred.mbtb.victimCacheSize = 0

            if args.btb_tage_upper_bound:
                cpu.branchPred.tage = BTBTAGEUpperBound(
                    usePathHashHistory=True)

            cpu.branchPred.tage.numPredictors = 8
            cpu.branchPred.tage.tableSizes = [2048] * 8
            cpu.branchPred.tage.TTagBitSizes = [13] * 8
            cpu.branchPred.tage.TTagPcShifts = [1] * 8
            cpu.branchPred.tage.histLengths = [4, 9, 17, 29, 56, 109, 211, 397]
            cpu.branchPred.tage.numWays = [2] * 8
            cpu.branchPred.tage.numBanks = 4
            cpu.branchPred.tage.enableBankConflict = True

            cpu.branchPred.mbtb.resolvedUpdate = True
            cpu.branchPred.tage.resolvedUpdate = True
            cpu.branchPred.ittage.resolvedUpdate = True

            cpu.branchPred.ubtb.enabled = True
            cpu.branchPred.abtb.enabled = True
            cpu.branchPred.microtage.enabled = True
            cpu.branchPred.microtage.usingS3Pred = True
            cpu.branchPred.mbtb.enabled = True
            cpu.branchPred.tage.enabled = True
            cpu.branchPred.ittage.enabled = True
            cpu.branchPred.mgsc.enabled = True
            cpu.branchPred.ras.enabled = True

            if getattr(args, 'standalone_sc', False):
                cpu.branchPred.microtage.enabled = False
                cpu.branchPred.tage.enabled = False

                cpu.branchPred.mgsc.forceUseSC = True
                cpu.branchPred.mgsc.allowMissingTageInfo = True

        # l1 cache per core
        if args.caches:
            cpu.icache.size = '64kB'
            cpu.dcache.size = '64kB'
            cpu.dcache.tag_load_read_ports = 3
            cpu.dcache.mshrs = 16
            cpu.dcache.do_fast_writeline = False
            cpu.dcache.simulate_dcache_refill = True
            cpu.dcache.prefetch_can_offload = False
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
                l2_wrapper.data_sram_banks = 1
                l2_wrapper.dir_sram_banks = 1
                l2_wrapper.pipe_dir_write_stage = 3
                l2_wrapper.dir_read_bypass = False
                for j in range(args.l2_slices):
                    l2_wrapper.slices[j].inner_cache.wpu = NULL
                    l2_wrapper.slices[j].inner_cache.do_fast_writeline = False
                    l2_wrapper.slices[j].inner_cache.prefetch_can_offload = False
                    # Configure XSDRRIP replacement policy (DRRIP mode)
                    # Each slice: 2MB/4 = 512KB, 8-way, 64B line → 1024 sets
                    l2_wrapper.slices[j].inner_cache.replacement_policy = XSDRRIPRP(mode=2, num_sets=1024)
            system.tol2bus_list[i].forward_latency = 3  # 3->0
            system.tol2bus_list[i].response_latency = 3  # 3->0
            system.tol2bus_list[i].hint_wakeup_ahead_cycles = 1  # 1->0

            # Enable dual-port for DCache → L2 communication
            # ReqLayer[0]: ICache+DCache+ITB+DTB → L2, allow 2 requests per cycle
            # RespLayer[1]: L2 → DCache, allow 2 responses per cycle
            # system.tol2bus_list[i].layer_bandwidth_configs = [
            #     LayerBandwidthConfig(direction="req", port_index=0, max_per_cycle=2),
            #     LayerBandwidthConfig(direction="resp", port_index=1, max_per_cycle=2),
            # ]

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
    args.bp_type = 'DecoupledBPUWithBTB'
    args.l2_size = '2MB'
    args.kmh_align = True   # align prefetcher in RTL, spec06 decrease 1 score
    args.no_pf = True       # disable cache hardware prefetchers, including L1 Dcache

    # Match the memories with the CPUs, based on the options for the test system
    TestMemClass = Simulation.setMemClass(args)

    test_sys = build_xiangshan_system(args)
    if args.raw_cpt and args.generic_rv_cpt and os.path.basename(args.generic_rv_cpt) == "linux.bin":
        configure_xiangshan_linux_workload(test_sys, args)
    # Set ideal parameters here with the highest priority, over command-line arguments
    setKmhV3Params(args, test_sys)

    root = Root(full_system=True, system=test_sys)
    if maybe_handle_solver_runtime(root, args):
        sys.exit(0)

    Simulation.run_vanilla(args, root, test_sys, FutureClass)
