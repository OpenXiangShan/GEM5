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
    
    # Create system using FS mode with trace-specific memory configuration
    test_sys = makeBareMetalXiangshanSystem('timing', SysConfig(mem=args.mem_size), None, np=np, ruby=ruby)
    
    # CRITICAL FIX: Configure trace-specific memory ranges and functional TLB for trace mode
    if hasattr(args, 'enable_trace_mode') and args.enable_trace_mode:
        print("Trace mode: Using FS mode with functional TLB to bypass MMU translation issues")
        print("Trace mode: Configuring expanded memory ranges for trace address mapping")
        # Force functional TLB to bypass complex MMU translation
        args.functional_tlb = True
    else:
        print("Checkpoint mode: Using standard FS mode with normal MMU translation")
    test_sys.num_cpus = np

    test_sys.xiangshan_system = True
    # Disable difftest for trace mode - trace execution doesn't need verification with reference model
    # Only enable difftest if not in trace mode - trace mode doesn't need reference model verification
    test_sys.enable_difftest = (args.enable_difftest
                                if not (hasattr(args, 'enable_trace_mode')
                                        and args.enable_trace_mode)
                                else False)

    # Configure XiangShan inputs - skip checkpoint loading in trace mode  
    if hasattr(args, 'enable_trace_mode') and args.enable_trace_mode:
        # For trace mode in SE mode, we skip most FS-specific configuration
        ref_so = None
        if args.enable_difftest and args.difftest_ref_so is None:
            # Use same logic as XSConfig.config_xiangshan_inputs for ref_so
            if "GCBV_REF_SO" in os.environ:
                ref_so = os.environ["GCBV_REF_SO"]
                print("Obtained ref_so from GCBV_REF_SO: ", ref_so)
            elif "NEMU_HOME" in os.environ:
                ref_so = os.path.join(os.environ["NEMU_HOME"], "build/riscv64-nemu-interpreter-so")
                print("Obtained ref_so from NEMU_HOME: ", ref_so)
            else:
                fatal("No valid ref_so file specified for trace mode difftest")
        elif args.enable_difftest and args.difftest_ref_so is not None:
            ref_so = args.difftest_ref_so
            print("Obtained ref_so from args.difftest_ref_so: ", ref_so)

        args.difftest_ref_so = ref_so

        # Trace mode FS configuration with functional TLB - bootloader needed but no checkpoint
        test_sys.workload.bootloader = '/nfs/home/goulingrui/project/riscv-environments/riscv-pk/build/bbl'
        test_sys.workload.xiangshan_cpt = False  # No checkpoint in trace mode
        test_sys.restore_from_gcpt = False       # Disable GCPT restoration
        
        # Configure DRAMsim3 if needed for memory controller
        if args.mem_type == 'DRAMsim3' and args.dramsim3_ini is None:
            root_dir = os.path.dirname(os.path.dirname(os.path.dirname(__file__)))
            args.dramsim3_ini = os.path.join(root_dir,
                                             'ext/dramsim3/xiangshan_configs/xiangshan_DDR4_8Gb_x8_3200_2ch.ini')

        print("Trace mode: FS mode with functional TLB configured to bypass MMU translation issues")
    else:
        # Standard checkpoint-based configuration
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
    # Configure MMU for trace-aware FS mode
    for cpu in test_sys.cpu:
        if hasattr(args, 'enable_trace_mode') and args.enable_trace_mode:
            # Trace mode: Ensure trace memory region (0x80000000+) is cacheable
            # PMAChecker excludes regions from being cacheable, so avoid trace region
            # Physical memory starts at 0x80000000, so make everything below that uncacheable
            cpu.mmu.pma_checker = PMAChecker(
                uncacheable=[AddrRange(0x1000, size=0x80000000-0x1000)])  # Exclude everything below physical memory
            cpu.mmu.functional = True  # Use functional TLB for reliable trace address translation
            print(f"Trace mode: CPU {cpu.cpu_id} configured with trace-aware PMAChecker and functional TLB")
        else:
            # Standard FS mode: Standard PMAChecker configuration  
            cpu.mmu.pma_checker = PMAChecker(
                uncacheable=[AddrRange(0x1000, size=0x80000000-0x1000)])
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

    # Configure trace mode if enabled
    if hasattr(args, 'enable_trace_mode') and args.enable_trace_mode:
        print(f"Configuring CPUs for trace mode...")
        for cpu in test_sys.cpu:
            # Enable trace mode
            cpu.enableTraceMode = True
            cpu.traceFile = args.trace_file
            cpu.traceFormat = args.trace_format
            # Unify with normal mode option: use --maxinsts
            cpu.max_insts_any_thread = args.maxinsts

            # Trace address mapping: Map to existing physical memory range
            # System physical memory starts at 0x80000000, so map trace addresses there
            cpu.traceAddrBase = 0x80000000  # Start of physical memory
            cpu.traceAddrSize = 0x40000000  # 1GB window within physical memory

            # Disable strict memory ordering for trace mode compatibility
            # Trace instructions may not follow strict ordering requirements
            cpu.needsTSO = False

            # Trace mispredict modeling controls
            cpu.traceMispredictPenalty = args.trace_mispredict_penalty
            cpu.traceEnableWrongPath = (not args.trace_disable_wrongpath)
            if hasattr(args, 'trace_wrongpath_use_traceinst') and args.trace_wrongpath_use_traceinst:
                cpu.traceWrongPathUseTraceInst = True

            # Note: Difftest configured at system level, not CPU level

            # Configure trace-specific parameters
            if hasattr(args, 'trace_enable_decoupled_bp') and args.trace_enable_decoupled_bp:
                cpu.enableDecoupledBPInTrace = True
            else:
                cpu.enableDecoupledBPInTrace = False

            cpu.traceCheckpointInterval = (args.trace_checkpoint_interval
                                           if hasattr(args, 'trace_checkpoint_interval')
                                           else 64)
            cpu.traceBPValidation = not (hasattr(args, 'trace_disable_bp_validation')
                                         and args.trace_disable_bp_validation)

        print(f"  Trace file: {args.trace_file}")
        print(f"  Trace format: {args.trace_format}")
        print(f"  Max instructions: {args.maxinsts}")
        print(f"  Decoupled BP: {hasattr(args, 'trace_enable_decoupled_bp') and args.trace_enable_decoupled_bp}")

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

        # Align trace address mapping window to physical memory size (Ruby path)
        if hasattr(args, 'enable_trace_mode') and args.enable_trace_mode:
            try:
                base = int(test_sys.mem_ranges[0].start)
                total = 0
                for r in test_sys.mem_ranges:
                    total += int(r.size())
                for cpu in test_sys.cpu:
                    cpu.traceAddrBase = base
                    cpu.traceAddrSize = total
                print(f"Trace mode: Align trace mapping to mem: base=0x{base:x}, size=0x{total:x}")
            except Exception as e:
                print(f"Warning: failed to align trace mapping to mem (Ruby path): {e}")

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

        # Align trace address mapping window to physical memory size (classic cache path)
        if hasattr(args, 'enable_trace_mode') and args.enable_trace_mode:
            try:
                base = int(test_sys.mem_ranges[0].start)
                total = 0
                for r in test_sys.mem_ranges:
                    total += int(r.size())
                for cpu in test_sys.cpu:
                    cpu.traceAddrBase = base
                    cpu.traceAddrSize = total
                print(f"Trace mode: Align trace mapping to mem: base=0x{base:x}, size=0x{total:x}")
            except Exception as e:
                print(f"Warning: failed to align trace mapping to mem: {e}")

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
        cpu.numPhysIntRegs = 224
        cpu.numPhysFloatRegs = 256
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
        cpu.enableConstantFolding = True

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

if __name__ == '__m5_main__':
    # Add args
    parser = argparse.ArgumentParser()
    Options.addCommonOptions(parser, configure_xiangshan=True)
    Options.addXiangshanFSOptions(parser)

    # Add the ruby specific and protocol specific args
    if '--ruby' in sys.argv:
        Ruby.define_options(parser)

    # Add trace-specific arguments for trace-driven simulation
    parser.add_argument('--enable-trace-mode', action='store_true',
                       help='Enable trace-driven simulation mode (alternative to checkpoints)')
    parser.add_argument('--trace-file', type=str,
                       help='Path to the trace file (required for trace mode)')
    parser.add_argument('--trace-format', type=str, default='champsim',
                       choices=['champsim', 'cbp2025'],
                       help='Trace format (default: champsim)')
    # Use the common --maxinsts option provided by common Options; no trace-specific max

    # Decoupled branch predictor options for trace mode
    parser.add_argument('--trace-enable-decoupled-bp', action='store_true',
                       help='Enable decoupled branch predictor in trace mode')
    parser.add_argument('--trace-checkpoint-interval', type=int, default=64,
                       help='Checkpoint interval for trace rollback (default: 64)')
    parser.add_argument('--trace-disable-bp-validation', action='store_true',
                       help='Disable branch predictor validation against trace')
    parser.add_argument('--trace-mispredict-penalty', type=int, default=8,
                       help='Cycles to penalize on mispredict (default: 8)')
    parser.add_argument('--trace-disable-wrongpath', action='store_true',
                       help='Disable explicit wrong-path injection (use stall model)')
    parser.add_argument('--trace-wrongpath-use-traceinst', action='store_true',
                       help='Wrong-path injection uses trace instructions with checkpoint/restore (default: NOPs)')

    # Check for trace mode before parsing to make generic-rv-cpt conditional
    if '--enable-trace-mode' in sys.argv:
        # In trace mode, make generic-rv-cpt optional by providing a dummy value
        # Find the generic-rv-cpt action and remove its required flag
        for action in parser._actions:
            if action.dest == 'generic_rv_cpt':
                action.required = False
                action.default = "trace_mode_dummy"
                break

    args = parser.parse_args()

    # Validate trace mode arguments
    if args.enable_trace_mode:
        if not args.trace_file:
            fatal("--trace-file is required when --enable-trace-mode is specified")
        if not os.path.exists(args.trace_file):
            fatal(f"Trace file not found: {args.trace_file}")

        print(f"Trace mode enabled:")
        print(f"  Trace file: {args.trace_file}")
        print(f"  Trace format: {args.trace_format}")
        print(f"  Max instructions: {args.maxinsts}")
    else:
        # In checkpoint mode, ensure generic_rv_cpt is provided and valid
        if args.generic_rv_cpt == "trace_mode_dummy":
            fatal("--generic-rv-cpt is required for checkpoint mode")
        if not os.path.exists(args.generic_rv_cpt):
            fatal(f"Checkpoint file not found: {args.generic_rv_cpt}")

    if args.xiangshan_ecore:
        FutureClass = None
        args.cpu_clock = '2.4GHz'
    else:
        FutureClass = None

    args.xiangshan_system = True
    # Only enable difftest if not in trace mode - trace mode doesn't need reference model verification
    if not (hasattr(args, 'enable_trace_mode') and args.enable_trace_mode):
        args.enable_difftest = True
    else:
        args.enable_difftest = False
        print("Trace mode: Difftest disabled for trace execution")
    args.enable_riscv_vector = True

    assert not args.external_memory_system

    # Match the memories with the CPUs, based on the options for the test system
    TestMemClass = Simulation.setMemClass(args)

    test_sys = build_test_system(args.num_cpus, args)

    # Set ideal parameters here with the highest priority, over command-line arguments
    if args.ideal_kmhv3:
        setKmhV3IdealParams(args, test_sys)

    # FS mode for both checkpoint and trace simulation (with expanded memory ranges for trace)
    root = Root(full_system=True, system=test_sys)

    # Run simulation - different execution paths for trace mode vs checkpoint mode
    if hasattr(args, 'enable_trace_mode') and args.enable_trace_mode:
        print("Starting trace-driven simulation...")
        print("This will replay the trace through the XiangShan pipeline")
        print("and generate detailed performance statistics.")

        # For trace mode, we still use run_vanilla but may need to modify some parameters
        # The trace infrastructure will handle the actual trace execution
        Simulation.run_vanilla(args, root, test_sys, FutureClass)

        print("Trace simulation completed.")
        print(f"Statistics available in m5out/stats.txt")
    else:
        # Standard checkpoint-based simulation (existing functionality)
        Simulation.run_vanilla(args, root, test_sys, FutureClass)
