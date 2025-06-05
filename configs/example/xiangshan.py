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
from common import CacheConfig
from common import CpuConfig
from common import MemConfig
from common import ObjectList
from common import XSConfig
from common.Caches import *
from common import Options
from common.FUScheduler import *
from m5.objects import PerfRecord


class XiangshanCore(RiscvO3CPU):
    scheduler = KunminghuScheduler()

class XiangshanECore(XiangshanCore):
    fetchWidth = 8
    decodeWidth = 4
    renameWidth = 4

    numROBEntries = 150
    LQEntries = 48
    SQEntries = 32
    numPhysIntRegs = 108
    numPhysFloatRegs = 112
    numPhysVecRegs = 112
    numPhysVecPredRegs = 36
    numPhysCCRegs = 0
    numPhysRMiscRegs = 40
    scheduler = ECoreScheduler()

class XiangshanECore2Read(XiangshanCore):
    fetchWidth = 8
    decodeWidth = 4
    renameWidth = 4

    numROBEntries = 150
    LQEntries = 48
    SQEntries = 32
    numPhysIntRegs = 108
    numPhysFloatRegs = 112
    numPhysVecRegs = 112
    numPhysVecPredRegs = 36
    numPhysCCRegs = 0
    numPhysRMiscRegs = 40
    scheduler = ECore2ReadScheduler()

def build_test_system(np, args):
    assert buildEnv['TARGET_ISA'] == "riscv"

    # override cpu class and clock
    if args.xiangshan_ecore:
        TestCPUClass = XiangshanECore
        args.cpu_clock = '2.4GHz'
    else:
        TestCPUClass = XiangshanCore

    ruby = False
    if hasattr(args, 'ruby') and args.ruby:
        ruby = True
    test_sys = makeBareMetalXiangshanSystem('timing', SysConfig(mem=args.mem_size), None, np=np, ruby=ruby)
    test_sys.num_cpus = np

    test_sys.xiangshan_system = True
    test_sys.enable_difftest = args.enable_difftest

    XSConfig.config_xiangshan_inputs(args, test_sys)

     # Set the cache line size for the entire system
    test_sys.cache_line_size = args.cacheline_size

    # Create a top-level voltage domain
    test_sys.voltage_domain = VoltageDomain(voltage = args.sys_voltage)

    # Create a source clock for the system and set the clock period
    test_sys.clk_domain = SrcClockDomain(clock =  args.sys_clock,
            voltage_domain = test_sys.voltage_domain)

    # Create a CPU voltage domain
    test_sys.cpu_voltage_domain = VoltageDomain()

    # Create a source clock for the CPUs and set the clock period
    test_sys.cpu_clk_domain = SrcClockDomain(clock = args.cpu_clock,
                                             voltage_domain =
                                             test_sys.cpu_voltage_domain)

    # For now, assign all the CPUs to the same clock domain
    test_sys.cpu = [TestCPUClass(clk_domain=test_sys.cpu_clk_domain, cpu_id=i)
                    for i in range(np)]
    for cpu in test_sys.cpu:
        cpu.mmu.pma_checker = PMAChecker(
            uncacheable=[AddrRange(0, size=0x80000000)])
        cpu.mmu.functional = args.functional_tlb

    # configure BP
    args.enable_loop_predictor = True
    if args.enable_riscv_vector:
        args.enable_loop_buffer = True

    for i in range(np):
        if args.kmh_align:
            test_sys.cpu[i].enable_storeSet_train = False

        if args.bp_type is None or args.bp_type == 'DecoupledBPUWithFTB' or args.bp_type == 'DecoupledBPUWithBTB':
            enable_bp_db = len(args.enable_bp_db) > 1
            if enable_bp_db:
                bp_db_switches = args.enable_bp_db[1] + ['basic']
                print("BP db switches:", bp_db_switches)
            else:
                bp_db_switches = []
            # for DecoupledBPUWithBTB, loop predictor and jump ahead predictor are not supported
            #if args.bp_type == 'DecoupledBPUWithBTB':
            if args.enable_loop_predictor or args.enable_loop_buffer:
                print("loop predictor and loop buffer not supported for DecoupledBPUWithBTB")
                args.enable_loop_predictor = False
                args.enable_loop_buffer = False
            if args.enable_jump_ahead_predictor:
                print("jump ahead predictor not supported for DecoupledBPUWithBTB")
                args.enable_jump_ahead_predictor = False

            BPClass = DecoupledBPUWithBTB() if args.bp_type == 'DecoupledBPUWithBTB' else DecoupledBPUWithFTB()
            test_sys.cpu[i].branchPred = BPClass(
                                            bpDBSwitches=bp_db_switches,
                                            enableLoopBuffer=args.enable_loop_buffer,
                                            enableLoopPredictor=args.enable_loop_predictor,
                                            enableJumpAheadPredictor=args.enable_jump_ahead_predictor
                                            )
            test_sys.cpu[i].branchPred.tage.enableSC = not args.disable_sc
            test_sys.cpu[i].branchPred.isDumpMisspredPC = True
        else:
            test_sys.cpu[i].branchPred = ObjectList.bp_list.get(args.bp_type)

        if args.indirect_bp_type:
            IndirectBPClass = ObjectList.indirect_bp_list.get(
                args.indirect_bp_type)
            test_sys.cpu[i].branchPred.indirectBranchPred = \
                    IndirectBPClass()

    # configure memory related
    if args.mem_type == 'DRAMsim3':
        assert args.dramsim3_ini is not None

    for cpu in test_sys.cpu:
        cpu.store_prefetch_train = not args.kmh_align
    # ruby will overwrite the store_prefetch_train
    if ruby:
        test_sys._dma_ports = []
        bootmem = getattr(test_sys, '_bootmem', None)
        Ruby.create_system(args, True, test_sys, test_sys.iobus,
                           test_sys._dma_ports, bootmem)

        # Create a seperate clock domain for Ruby
        test_sys.ruby.clk_domain = SrcClockDomain(clock = args.ruby_clock,
                                        voltage_domain = test_sys.voltage_domain)

        # Connect the ruby io port to the PIO bus,
        # assuming that there is just one such port.
        test_sys.iobus.mem_side_ports = test_sys.ruby._io_port.in_ports

        for (i, cpu) in enumerate(test_sys.cpu):
            # Tie the cpu ports to the correct ruby system ports
            cpu.clk_domain = test_sys.cpu_clk_domain
            cpu.createThreads()
            print("Create threads for test sys cpu ({})".format(type(cpu)))
            cpu.createInterruptController()

            test_sys.ruby._cpu_ports[i].connectCpuPorts(cpu)

            # Ruby D-cache does not support store prefetch yet
            cpu.store_prefetch_train = False

    else:
        if args.caches or args.l2cache:
            # By default the IOCache runs at the system clock
            test_sys.iocache = IOCache(addr_ranges = test_sys.mem_ranges)
            test_sys.iocache.cpu_side = test_sys.iobus.mem_side_ports
            test_sys.iocache.mem_side = test_sys.membus.cpu_side_ports
        elif not args.external_memory_system:
            test_sys.iobridge = Bridge(delay='50ns', ranges = test_sys.mem_ranges)
            test_sys.iobridge.cpu_side_port = test_sys.iobus.mem_side_ports
            test_sys.iobridge.mem_side_port = test_sys.membus.cpu_side_ports

        for i in range(np):
            test_sys.cpu[i].createThreads()
            print("Create threads for test sys cpu ({})".format(type(test_sys.cpu[i])))

        for opt in ['caches', 'l2cache', 'l1_to_l2_pf_hint']:
            if hasattr(args, opt) and not getattr(args, opt):
                setattr(args, opt, True)

        if not args.no_l3cache:
            for opt in ['l3cache', 'l2_to_l3_pf_hint']:
                if hasattr(args, opt) and not getattr(args, opt):
                    setattr(args, opt, True)

        if args.xiangshan_ecore and args.no_l3cache:
            args.l2_size = '4MB'

        CacheConfig.config_cache(args, test_sys)

        MemConfig.config_mem(args, test_sys)

    if args.mmc_img:
        for mmc, cpu in zip(test_sys.mmcs, test_sys.cpu):
            mmc.cpt_bin_path = args.mmc_cptbin
            mmc.img_path = args.mmc_img
            cpu.nemuSDCptBin = mmc.cpt_bin_path
            cpu.nemuSDimg = mmc.img_path

    XSConfig.config_difftest(test_sys.cpu, args, test_sys)

    # configure vector
    if args.enable_riscv_vector:
        test_sys.enable_riscv_vector = True
        for cpu in test_sys.cpu:
            cpu.enable_riscv_vector = True

    # config arch db
    if args.enable_arch_db:
        perfCCT_cmd = "CREATE TABLE LifeTimeCommitTrace(ID INTEGER PRIMARY KEY AUTOINCREMENT,"
        perfCCT_cmd += PerfRecord.vals[0] + " INT NOT NULL"
        for i in range(1, len(PerfRecord.vals)):
            name = PerfRecord.vals[i]
            type_str = "INT" if name.lower().startswith(('at', 'pc')) else "CHAR(20)"
            perfCCT_cmd += "," + name + " " + type_str + " NOT NULL"
        perfCCT_cmd += ");"

        test_sys.arch_db = ArchDBer(arch_db_file=args.arch_db_file)
        test_sys.arch_db.dump_from_start = args.arch_db_fromstart
        test_sys.arch_db.enable_rolling = args.enable_rolling
        test_sys.arch_db.dump_l1_pf_trace = False
        test_sys.arch_db.dump_mem_trace = False
        test_sys.arch_db.dump_l1_evict_trace = False
        test_sys.arch_db.dump_l2_evict_trace = False
        test_sys.arch_db.dump_l3_evict_trace = False
        test_sys.arch_db.dump_l1_miss_trace = False
        test_sys.arch_db.dump_bop_train_trace = False
        test_sys.arch_db.dump_sms_train_trace = False
        test_sys.arch_db.dump_lifetime = False
        test_sys.arch_db.table_cmds = [
            "CREATE TABLE L1MissTrace(" \
            "ID INTEGER PRIMARY KEY AUTOINCREMENT," \
            "PC INT NOT NULL," \
            "SOURCE INT NOT NULL," \
            "PADDR INT NOT NULL," \
            "VADDR INT NOT NULL," \
            "STAMP INT NOT NULL," \
            "SITE TEXT);"
            ,
            "CREATE TABLE CacheEvictTrace(" \
            "ID INTEGER PRIMARY KEY AUTOINCREMENT," \
            "Tick INT NOT NULL," \
            "PADDR INT NOT NULL," \
            "STAMP INT NOT NULL," \
            "Level INT NOT NULL," \
            "SITE TEXT);"
            ,
            "CREATE TABLE MemTrace(" \
            "ID INTEGER PRIMARY KEY AUTOINCREMENT," \
            "Tick INT NOT NULL," \
            "IsLoad BOOL NOT NULL," \
            "PC INT NOT NULL," \
            "VADDR INT NOT NULL," \
            "PADDR INT NOT NULL," \
            "Issued INT NOT NULL," \
            "Translated INT NOT NULL," \
            "Completed INT NOT NULL," \
            "Committed INT NOT NULL," \
            "Writenback INT NOT NULL," \
            "PFSrc INT NOT NULL," \
            "SITE TEXT);"
            ,
            "CREATE TABLE L1PFTrace(" \
            "ID INTEGER PRIMARY KEY AUTOINCREMENT," \
            "Tick INT NOT NULL," \
            "TriggerPC INT NOT NULL," \
            "TriggerVAddr INT NOT NULL," \
            "PFVAddr INT NOT NULL," \
            "PFSrc INT NOT NULL," \
            "SITE TEXT);"
            ,
            "CREATE TABLE BOPTrainTrace(" \
            "ID INTEGER PRIMARY KEY AUTOINCREMENT," \
            "Tick INT NOT NULL," \
            "OldAddr INT NOT NULL," \
            "CurAddr INT NOT NULL," \
            "Offset INT NOT NULL," \
            "Score INT NOT NULL," \
            "Miss BOOL NOT NULL," \
            "SITE TEXT);"
            ,
            "CREATE TABLE SMSTrainTrace(" \
            "ID INTEGER PRIMARY KEY AUTOINCREMENT," \
            "Tick INT NOT NULL," \
            "OldAddr INT NOT NULL," \
            "CurAddr INT NOT NULL," \
            "TriggerOffset INT NOT NULL," \
            "Conf INT NOT NULL," \
            "Miss BOOL NOT NULL," \
            "SITE TEXT);"
            ,# perfCounter CommitTrace
            perfCCT_cmd
        ]

    # config debug trace
    for i in range(np):
        if args.dump_commit:
            test_sys.cpu[i].dump_commit = True
            test_sys.cpu[i].dump_start = args.dump_start
        else:
            test_sys.cpu[i].dump_commit = False
            test_sys.cpu[i].dump_start = 0

    return test_sys

def setKmhV3IdealParams(args, system):
    for cpu in system.cpu:

        cpu.mmu.itb.size = 96

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
        cpu.numPhysIntRegs = 354
        cpu.numPhysFloatRegs = 384
        cpu.numROBEntries = 640
        cpu.enableDispatchStage = True
        cpu.numDQEntries = [8, 8, 8]
        cpu.dispWidth = [10, 10, 10] # 6->10
        cpu.scheduler = KMHV3Scheduler()

        cpu.BankConflictCheck = True   # real bank conflict 0.2 score
        cpu.EnableLdMissReplay = False
        cpu.EnablePipeNukeCheck = False
        cpu.StoreWbStage = 4 # store writeback at s4

        # ideal decoupled frontend
        if args.bp_type == 'DecoupledBPUWithFTB' or args.bp_type == 'DecoupledBPUWithBTB':
            if args.bp_type == 'DecoupledBPUWithFTB':
                cpu.branchPred.enableTwoTaken = False
                cpu.branchPred.numBr = 8    # numBr must be a power of 2, see getShuffledBrIndex()
                cpu.branchPred.predictWidth = 64
                cpu.branchPred.uftb.numEntries = 1024
                cpu.branchPred.ftb.numEntries = 16384
                cpu.branchPred.tage.baseTableSize = 16384
                cpu.branchPred.tage.tableSizes = [2048] * 14
            else:
                cpu.branchPred.predictWidth = 64              # max width of a fetch block
                cpu.branchPred.btb.numEntries = 16384
                # TODO: BTB TAGE do not bave base table, do not support SC
                cpu.branchPred.tage.tableSizes = [2048] * 14  # 2ways, 2048 sets

            cpu.branchPred.tage.enableSC = False # TODO(bug): When numBr changes, enabling SC will trigger an assert
            cpu.branchPred.ftq_size = 256
            cpu.branchPred.fsq_size = 256
            cpu.branchPred.tage.numPredictors = 14
            cpu.branchPred.tage.TTagBitSizes = [13] * 14
            cpu.branchPred.tage.TTagPcShifts = [1] * 14
            cpu.branchPred.tage.histLengths = [4, 7, 12, 16, 21, 29, 38, 51, 68, 90, 120, 160, 283, 499]

        # ideal l1 caches
        if args.caches:
            cpu.icache.size = '64kB'
            cpu.dcache.size = '64kB'
            cpu.icache.enable_wayprediction = False
            cpu.dcache.enable_wayprediction = False
            cpu.dcache.tag_load_read_ports = 100 # 3->100
            cpu.dcache.mshrs = 16

    if args.l2cache:
        for i in range(args.num_cpus):
            system.l2_caches[i].size = '2MB'
            system.l2_caches[i].enable_wayprediction = False
            system.l2_caches[i].slice_num = 0   # 4 -> 0, no slice
            system.tol2bus_list[i].forward_latency = 0  # 3->0
            system.tol2bus_list[i].response_latency = 0  # 3->0
            system.tol2bus_list[i].hint_wakeup_ahead_cycles = 0  # 2->0

    if args.l3cache:
        system.l3.enable_wayprediction = False
        system.l3.mshrs = 128

if __name__ == '__m5_main__':
    # Add args
    parser = argparse.ArgumentParser()
    Options.addCommonOptions(parser, configure_xiangshan=True)
    Options.addXiangshanFSOptions(parser)

    # Add the ruby specific and protocol specific args
    if '--ruby' in sys.argv:
        Ruby.define_options(parser)

    args = parser.parse_args()

    if args.xiangshan_ecore:
        FutureClass = None
        args.cpu_clock = '2.4GHz'
    else:
        FutureClass = None

    args.xiangshan_system = True
    args.enable_difftest = True
    args.enable_riscv_vector = True

    assert not args.external_memory_system

    # Match the memories with the CPUs, based on the options for the test system
    TestMemClass = Simulation.setMemClass(args)

    test_sys = build_test_system(args.num_cpus, args)

    # Set ideal parameters here with the highest priority, over command-line arguments
    if args.ideal_kmhv3:
        setKmhV3IdealParams(args, test_sys)

    root = Root(full_system=True, system=test_sys)

    Simulation.run_vanilla(args, root, test_sys, FutureClass)
