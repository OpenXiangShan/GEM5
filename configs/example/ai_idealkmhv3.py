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

from m5.objects.ValuePredictor import *


AI_MATRIX_TIMING_DEFAULTS = {
    "matrix_issue_interval_cycles": 1,
    "matrix_load_base_cycles": 4,
    "matrix_store_base_cycles": 4,
    "matrix_zero_cycles": 1,
    "matrix_compute_base_cycles": 2,
    "matrix_compute_read_cycles": 1,
    "matrix_release_cycles": 1,
    "matrix_local_mmu_issue_per_cycle": 1,
    "matrix_local_mmu_arb_cycles": 1,
    "matrix_l2_request_pipeline_cycles": 1,
    "matrix_l2_response_pipeline_cycles": 1,
    "matrix_local_mmu_read_latency_cycles": 20,
    "matrix_local_mmu_write_ack_latency_cycles": 12,
}


def setAiMatrixTimingDefaults(args):
    # Keep CUTE timing coarse by default. Command-line --matrix-* options remain
    # higher priority so calibration runs can sweep these knobs directly.
    for attr, value in AI_MATRIX_TIMING_DEFAULTS.items():
        if getattr(args, attr, None) is None:
            setattr(args, attr, value)


def setAiKmhV3IdealParams(args, system):
    for cpu in system.cpu:

        # fetch
        cpu.mmu.itb.size = 96
        cpu.fetchWidth = 32
        cpu.iewToFetchDelay = 2 # for resolved update, should train branch after squash
        cpu.commitToFetchDelay = 2
        cpu.fetchQueueSize = 64

        # decode
        cpu.fetchToDecodeDelay = 5
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

        # value predictor
        cpu.valuePred = IdealConstantLVP()

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
            # TAGE table sizes and numWays tuning
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
                system.l2_caches[i].slice_num = 0 # 4 -> 0, no slice
            else:
                l2_wrapper = system.l2_wrappers[i]
                l2_wrapper.data_sram_banks = 2
                l2_wrapper.dir_sram_banks = 2
                l2_wrapper.pipe_dir_write_stage = 4
                l2_wrapper.dir_read_bypass = True
                for j in range(args.l2_slices):
                    # Configure XSDRRIP replacement policy (DRRIP mode)
                    # Each slice: 2MB/4 = 512KB, 8-way, 64B line -> 1024 sets
                    l2_wrapper.slices[j].inner_cache.replacement_policy = XSDRRIPRP(mode=2, num_sets=1024)
            system.tol2bus_list[i].forward_latency = 0  # 3->0
            system.tol2bus_list[i].response_latency = 0  # 3->0
            system.tol2bus_list[i].hint_wakeup_ahead_cycles = 0  # 2->0

            # ReqLayer[0]: ICache+DCache+ITB+DTB -> L2, allow 2 requests per cycle
            # RespLayer[1]: L2 -> DCache, allow 2 responses per cycle
            system.tol2bus_list[i].layer_bandwidth_configs = [
                LayerBandwidthConfig(direction="req", port_index=0, max_per_cycle=2),
                LayerBandwidthConfig(direction="resp", port_index=1, max_per_cycle=2),
            ]

    # l3 cache
    if args.l3cache:
        system.l3.mshrs = 128


if __name__ == '__m5_main__':
    FutureClass = None

    args = xiangshan_system_init()

    assert not args.external_memory_system

    # AI performance runs use the same ideal KMHV3 CPU/cache envelope as
    # idealkmhv3.py, plus explicit coarse CUTE timing defaults.
    args.bp_type = 'DecoupledBPUWithBTB'
    args.l2_size = '2MB'
    args.l3_size = '32MB'
    args.enable_pf_buffer = False
    args.enable_riscv_vector = True
    setAiMatrixTimingDefaults(args)

    # Match the memories with the CPUs, based on the options for the test system
    TestMemClass = Simulation.setMemClass(args)

    test_sys = build_xiangshan_system(args)
    if args.raw_cpt and args.generic_rv_cpt and os.path.basename(args.generic_rv_cpt) == "linux.bin":
        configure_xiangshan_linux_workload(test_sys, args)

    # Set ideal parameters here with the highest priority, over command-line arguments
    setAiKmhV3IdealParams(args, test_sys)

    root = Root(full_system=True, system=test_sys)

    Simulation.run_vanilla(args, root, test_sys, FutureClass)
