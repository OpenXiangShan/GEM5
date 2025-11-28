import argparse
import sys

import m5
from m5.defines import buildEnv
from m5.objects import *
from m5.util import addToPath, fatal, warn
from m5.util.fdthelper import *

addToPath('../')

from ruby import Ruby

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
        cpu.enable_storeSet_train = False

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
        cpu.RobCompressPolicy = 'none'
        cpu.numROBEntries = 352
        cpu.CROB_instPerGroup = 2 # 1 if not using ROB compression

        # lsu
        cpu.StoreWbStage = 4
        cpu.EnableLdMissReplay = True
        cpu.EnablePipeNukeCheck = True
        cpu.BankConflictCheck = True
        cpu.sbufferBankWriteAccurately = False

        # lsq
        cpu.LQEntries = 72
        cpu.SQEntries = 56
        cpu.RARQEntries = 72
        cpu.RAWQEntries = 32
        cpu.LoadCompletionWidth = 8
        cpu.StoreCompletionWidth = 4
        cpu.RARDequeuePerCycle = 4
        cpu.RAWDequeuePerCycle = 4
        cpu.SbufferEntries = 16
        cpu.SbufferEvictThreshold = 7
        cpu.store_prefetch_train = False

        # branch predictor
        if args.bp_type == 'DecoupledBPUWithBTB':
            cpu.branchPred.ftq_size = 256
            cpu.branchPred.fsq_size = 256

            cpu.branchPred.mbtb.resolvedUpdate = True
            cpu.branchPred.tage.resolvedUpdate = True

            cpu.branchPred.ubtb.enabled = True
            cpu.branchPred.abtb.enabled = False
            cpu.branchPred.microtage.enabled = False
            cpu.branchPred.mbtb.enabled = True
            cpu.branchPred.tage.enabled = True
            cpu.branchPred.ittage.enabled = False
            cpu.branchPred.mgsc.enabled = False
            cpu.branchPred.ras.enabled = False

        # l1 cache per core
        if args.caches:
            cpu.icache.size = '64kB'
            cpu.dcache.size = '64kB'
            cpu.dcache.tag_load_read_ports = 3
            cpu.dcache.mshrs = 16

    # l2 caches
    if args.l2cache:
        for i in range(args.num_cpus):
            if args.classic_l2:
                system.l2_caches[i].slice_num = 4
                system.l2_caches[i].wpu = NULL
            else:
                l2_wrapper = system.l2_wrappers[i]
                l2_wrapper.data_sram_banks = 1
                l2_wrapper.dir_sram_banks = 1
                l2_wrapper.pipe_dir_write_stage = 3
                l2_wrapper.dir_read_bypass = False
                for j in range(args.l2_slices):
                    l2_wrapper.slices[j].inner_cache.wpu = NULL
            system.tol2bus_list[i].forward_latency = 3  # 3->0
            system.tol2bus_list[i].response_latency = 3  # 3->0
            system.tol2bus_list[i].hint_wakeup_ahead_cycles = 2  # 2->0

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

if __name__ == '__m5_main__':
    FutureClass = None

    args = xiangshan_system_init()

    assert not args.external_memory_system

    # Set default bp_type based on ideal_kmhv3 flag
    # If user didn't specify bp_type, set default based on ideal_kmhv3
    args.bp_type = 'DecoupledBPUWithBTB'
    args.l2_size = '1MB'
    args.kmh_align = True   # align prefetcher in RTL, spec06 decrease 1 score

    # Match the memories with the CPUs, based on the options for the test system
    TestMemClass = Simulation.setMemClass(args)

    test_sys = build_xiangshan_system(args)
    # Set ideal parameters here with the highest priority, over command-line arguments
    setKmhV3Params(args, test_sys)

    root = Root(full_system=True, system=test_sys)

    Simulation.run_vanilla(args, root, test_sys, FutureClass)
