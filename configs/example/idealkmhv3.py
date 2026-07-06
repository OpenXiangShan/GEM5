import argparse
import sys

import m5
from m5.defines import buildEnv
from m5.objects import *
from m5.objects.Prefetcher import XSPhysicalSmallBOP, XSVirtualLargeBOP
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

def setKmhV3IdealParams(args, system):
    for cpu in system.cpu:

        # fetch
        cpu.mmu.itb.size = 96
        cpu.fetchWidth = 32
        cpu.iewToFetchDelay = 2 # for resolved update, should train branch after squash
        cpu.commitToFetchDelay = 2
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
        cpu.scheduler.enableMainRdpOpt = False
        cpu.scheduler.intRegfileBanks = 4
        # intiq0
        cpu.scheduler.IQs[0].oports[0].rp = [IntRD(0, 0), IntRD(1, 0)]
        cpu.scheduler.IQs[0].oports[1].rp = [IntRD(1, 1), IntRD(7, 2)]

        # intiq1
        cpu.scheduler.IQs[1].oports[0].rp = [IntRD(2, 0), IntRD(3, 0)]
        cpu.scheduler.IQs[1].oports[1].rp = [IntRD(3, 1), IntRD(9, 2)]

        # intiq2
        cpu.scheduler.IQs[2].oports[0].rp = [IntRD(4, 0), IntRD(5, 0)]
        cpu.scheduler.IQs[2].oports[1].rp = [IntRD(5, 1), IntRD(11, 2)]

        # rob
        cpu.commitWidth = 8
        cpu.squashWidth = 8
        cpu.phyregReleaseWidth = 8
        cpu.RobCompressPolicy = 'none'
        cpu.numROBEntries = 352
        cpu.CROB_instPerGroup = 2 # 1 if not using ROB compression

        # lsu
        cpu.StoreWbStage = 4
        cpu.EnableLdMissReplay = True
        cpu.EnablePipeNukeCheck = True
        cpu.BankConflictCheck = True
        cpu.sbufferBankWriteAccurately = True
        cpu.DcacheSetDivNum = 2

        # value predictor
        # cpu.valuePred = IdealConstantLVP()

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
        cpu.SbufferEvictThreshold = 9
        cpu.enable_storeSet_train = True
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
            cpu.dcache.tag_load_read_ports = 3
            cpu.dcache.mshrs = 16
            cpu.dcache.do_fast_writeline = True
            cpu.dcache.pipe_latency = 3
            cpu.dcache.simulate_dcache_refill = True
            cpu.dcache.prefetch_can_offload = False
            if cpu.dcache.prefetcher != NULL:
                cpu.dcache.prefetcher.pht_pf_level = 2
                cpu.dcache.prefetcher.enable_temporal = False
                cpu.dcache.prefetcher.enable_berti = False
                cpu.dcache.prefetcher.enable_sstride = True
                cpu.dcache.prefetcher.enable_activepage = False
                cpu.dcache.prefetcher.enable_pht = True
                cpu.dcache.prefetcher.enable_xsstream = True
            set_lsq_bank_conflict_cache_params(cpu, system)

    # l2 caches
    if args.l2cache:
        for i in range(args.num_cpus):
            if args.classic_l2:
                system.l2_caches[i].slice_num = 0 # 4 -> 0, no slice
                if system.l2_caches[i].prefetcher != NULL:
                    system.l2_caches[i].prefetcher.enable_cmc = True
                    system.l2_caches[i].prefetcher.enable_bop = True
                    system.l2_caches[i].prefetcher.enable_cdp = False
                    system.l2_caches[i].prefetcher.enable_despacito_stream = False
                    system.l2_caches[i].prefetcher.bop_large = XSVirtualLargeBOP(
                        is_sub_prefetcher=True, enable_adaptoffset=False)
                    system.l2_caches[i].prefetcher.bop_small = XSPhysicalSmallBOP(
                        is_sub_prefetcher=True, enable_adaptoffset=False)
            else:
                l2_wrapper = system.l2_wrappers[i]
                l2_wrapper.data_sram_banks = 2
                l2_wrapper.dir_sram_banks = 2
                l2_wrapper.pipe_dir_write_stage = 3
                l2_wrapper.dir_read_bypass = False
                if l2_wrapper.prefetcher != NULL:
                    l2_wrapper.prefetcher.enable_cmc = False
                    l2_wrapper.prefetcher.enable_bop = True
                    l2_wrapper.prefetcher.enable_cdp = False
                    l2_wrapper.prefetcher.enable_despacito_stream = False
                    l2_wrapper.prefetcher.bop_large = XSVirtualLargeBOP(
                        is_sub_prefetcher=True, enable_adaptoffset=False)
                    l2_wrapper.prefetcher.bop_small = XSPhysicalSmallBOP(
                        is_sub_prefetcher=True, enable_adaptoffset=False)
                for j in range(args.l2_slices):
                    l2_wrapper.slices[j].inner_cache.wpu = NULL
                    l2_wrapper.slices[j].inner_cache.do_fast_writeline = True
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
        system.l3.do_fast_writeline = True
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
    args.l3_size = '32MB'
    args.pht_pf_level = 2
    # Enable prefetch buffers for all hardware prefetchers in this config.
    args.enable_pf_buffer = True
    # Match the memories with the CPUs, based on the options for the test system
    TestMemClass = Simulation.setMemClass(args)

    test_sys = build_xiangshan_system(args)
    # Set ideal parameters here with the highest priority, over command-line arguments
    setKmhV3IdealParams(args, test_sys)

    root = Root(full_system=True, system=test_sys)

    Simulation.run_vanilla(args, root, test_sys, FutureClass)
