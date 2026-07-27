import argparse
import os
import sys

import m5
from m5.defines import buildEnv
from m5.objects import *
from m5.util import addToPath, fatal, warn
from m5.util.fdthelper import *

from ruby import Ruby

from common.FSConfig import *
from common.SysPaths import *
from common.Benchmarks import *
from common import Simulation
from common import CacheConfig
from common import CpuConfig
from common import MemConfig
from common import ObjectList
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

addToPath('../')

_warned_deprecated_entrypoint = False


def _warn_if_deprecated_xiangshan_entrypoint():
    """
    Defensive UX: historically some docs/scripts used `configs/example/xiangshan.py`.

    We no longer keep that entrypoint in this repo. If users still run a legacy
    config script named `xiangshan.py`, emit a warning and point them to the
    maintained entrypoints.
    """
    global _warned_deprecated_entrypoint
    if _warned_deprecated_entrypoint:
        return

    # In gem5, sys.argv[0] is typically the config script path.
    argv0 = os.path.basename(sys.argv[0]) if sys.argv else ""
    argv_joined = " ".join(sys.argv) if sys.argv else ""

    if argv0 == "xiangshan.py" or "configs/example/xiangshan.py" in argv_joined:
        warn(
            "Deprecated config entrypoint detected (xiangshan.py). "
            "Please use configs/example/kmhv3.py (RTL-aligned) or "
            "configs/example/idealkmhv3.py (ideal/perf)."
        )
        _warned_deprecated_entrypoint = True


def _trace_timing_ptw_settings(args: argparse.Namespace):
    enabled = bool(getattr(args, 'trace_timing_ptw', False))
    if not enabled:
        return False, 0, 0

    reserved_bytes = int(getattr(args, 'trace_ptw_reserved_bytes', 64 * 1024 * 1024))
    if reserved_bytes <= 0:
        fatal(f"--trace-ptw-reserved-bytes must be > 0 (got {reserved_bytes})")

    page_size = getattr(args, 'trace_ptw_page_size', '4k')
    if page_size == '4k':
        leaf_page_size = 4 * 1024
    elif page_size == '2m':
        leaf_page_size = 2 * 1024 * 1024
    else:
        fatal(f"Unsupported --trace-ptw-page-size: {page_size}")

    return True, reserved_bytes, leaf_page_size


def _apply_trace_timing_ptw_cpu_params(args: argparse.Namespace, cpus, *, shrink_window: bool = True):
    enabled, reserved_bytes, leaf_page_size = _trace_timing_ptw_settings(args)
    if not enabled:
        return

    for cpu in cpus:
        cpu.traceTimingPTW = True
        cpu.tracePTReservedBytes = reserved_bytes
        cpu.tracePTLeafPageSize = leaf_page_size

    if not shrink_window:
        return

    for cpu in cpus:
        if int(cpu.traceAddrSize) <= reserved_bytes:
            fatal(
                "Trace timing PTW requires traceAddrSize > reserved bytes "
                f"(traceAddrSize=0x{int(cpu.traceAddrSize):x}, "
                f"reserved=0x{reserved_bytes:x})."
            )

        cpu.traceAddrSize = int(cpu.traceAddrSize) - reserved_bytes


def config_xiangshan_inputs(args: argparse.Namespace, sys):
    ref_so = None

    # configure difftest input
    if args.enable_difftest and args.difftest_ref_so is None:
        # ref so should be either provided from the command line or from the env
        if args.num_cpus > 1 and "GCBV_MULTI_CORE_REF_SO" in os.environ:
            ref_so = os.environ["GCBV_MULTI_CORE_REF_SO"]
            print("Obtained ref_so from GCBV_MULTI_CORE_REF_SO: ", ref_so)
        elif "GCBV_REF_SO" in os.environ:
            ref_so = os.environ["GCBV_REF_SO"]
            print("Obtained ref_so from GCBV_REF_SO: ", ref_so)
        elif "GCBH_REF_SO" in os.environ:
            ref_so = os.environ["GCBH_REF_SO"]
            print("Obtained ref_so from GCBH_REF_SO: ", ref_so)
        elif "NEMU_HOME" in os.environ:
            ref_so = os.path.join(os.environ["NEMU_HOME"], "build/riscv64-nemu-interpreter-so")
            print("Obtained ref_so from NEMU_HOME: ", ref_so)
        else:
            if "GCBV_REF_SO" in os.environ:
                print("Currently XS-GEM5 always turn on RVV and require a ref_so with RVV support")
            fatal("No valid ref_so file specified for the functional model to "
                  "compare against. Please 1) either specify a valid ref_so file using "
                  "the --difftest-ref-so option;\n"
                  "2) or specify GCBV_REF_SO/GCBV_MULTI_CORE_REF_SO/GCBH_REF_SO that points to the ref_so file;\n"
                  "3) or specify NEMU_HOME that contains build/riscv64-nemu-interpreter-so")
    elif args.enable_difftest and args.difftest_ref_so is not None:
        ref_so = args.difftest_ref_so
        print("Obtained ref_so from args.difftest_ref_so: ", ref_so)

    args.difftest_ref_so = ref_so

    if args.gcpt_restorer is None:
        if args.raw_cpt:
            # If using raw binary, no restorer is needed.
            gcpt_restorer = None
        elif args.num_cpus > 1:
            if "GCB_MULTI_CORE_RESTORER" in os.environ:
                gcpt_restorer = os.environ["GCB_MULTI_CORE_RESTORER"]
                print("Obtained gcpt_restorer from GCB_MULTI_CORE_RESTORER: ", gcpt_restorer)
            else:
                fatal("Plz set $GCB_MULTI_CORE_RESTORER when model Xiangshan with multi-core")
        elif args.restore_rvv_cpt:
            if "GCBV_RESTORER" in os.environ:
                gcpt_restorer = os.environ["GCBV_RESTORER"]
                print("Obtained gcpt_restorer from GCBV_RESTORER: ", gcpt_restorer)
            else:
                fatal("Plz set $GCBV_RESTORER when running RVV checkpoints")
        elif args.restore_rvh_cpt:
            if "GCBH_RESTORER" in os.environ:
                gcpt_restorer = os.environ["GCBH_RESTORER"]
                print("Obtained gcpt_restorer from GCBH_RESTORER: ", gcpt_restorer)
            else:
                fatal("Plz set $GCBH_RESTORER when running RVH checkpoints")
        else:
            if "GCB_RESTORER" in os.environ:
                gcpt_restorer = os.environ["GCB_RESTORER"]
                print("Obtained gcpt_restorer from GCB_RESTORER: ", gcpt_restorer)
            else:
                fatal("Plz set $GCB_RESTORER or pass it through --gcpt-restorer"
                      " when running non-RVV checkpoints")
    else:
        print("Obtained gcpt_restorer from args.gcpt_restorer: ", args.gcpt_restorer)
        gcpt_restorer = args.gcpt_restorer

    if args.num_cpus > 1:
        print("Simulating a multi-core system, demanding a larger GCPT restorer size (2M).")
        sys.gcpt_restorer_size_limit = 2**20
    elif args.restore_rvv_cpt:
        print("Simulating single core with RVV, demanding GCPT restorer size of 0x1000.")
        sys.gcpt_restorer_size_limit = 0x1000
    elif args.restore_rvh_cpt:
        print("Simulating single core with RVH, demanding GCPT restorer size of 0x1000.")
        sys.gcpt_restorer_size_limit = 0x1000
    else:
        print("Simulating single core without RVV, demanding GCPT restorer size of 0x700.")
        sys.gcpt_restorer_size_limit = 0x700

    # configure gcpt input
    if args.generic_rv_cpt is not None:
        assert(buildEnv['TARGET_ISA'] == "riscv")
        sys.restore_from_gcpt = True
        sys.gcpt_file = args.generic_rv_cpt

        sys.workload.bootloader = ''
        sys.workload.xiangshan_cpt = True

        if args.raw_cpt:
            assert not args.gcpt_restorer  # raw_cpt and gcpt_restorer are exclusive
            print('Using raw bbl', args.generic_rv_cpt)
            sys.map_to_raw_cpt = True
            sys.workload.raw_bootloader = True
        else:
            sys.gcpt_restorer_file = gcpt_restorer
    # enable h checkpoint
    if args.enable_h_gcpt:
        sys.enable_h_gcpt = True
    # configure DRAMSim input
    if args.mem_type == 'DRAMsim3' and args.dramsim3_ini is None:
        # use relative path to find the dramsim3 ini file, from configs/common/ to root
        root_dir = os.path.dirname(os.path.dirname(os.path.dirname(__file__)))
        args.dramsim3_ini = os.path.join(root_dir, 'ext/dramsim3/xiangshan_configs/xiangshan_DDR4_8Gb_x8_3200_2ch.ini')

    if args.mem_type == 'Ramulator2' and args.ramulator2_ini is None:
        # use relative path to find the ramulator ini file, from configs/common/ to root
        root_dir = os.path.dirname(os.path.dirname(os.path.dirname(__file__)))
        args.ramulator2_ini = os.path.join(root_dir, 'ext/ramulator2/xs_ramulator_config.yaml')
    return gcpt_restorer, ref_so

def config_difftest(cpu_list, args, sys):
    if not args.enable_difftest:
        return
    else:
        if len(cpu_list) > 1:
            sys.enable_mem_dedup = True
            for cpu in cpu_list:
                cpu.enable_mem_dedup = True
                cpu.enable_difftest = True
                cpu.difftest_ref_so = args.difftest_ref_so
        else:
            # sys.enable_mem_dedup = True
            # cpu_list[0].enable_mem_dedup = True
            cpu_list[0].enable_difftest = True
            cpu_list[0].difftest_ref_so = args.difftest_ref_so

def build_xiangshan_system(args):
    np = args.num_cpus
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
        if bool(getattr(args, 'trace_timing_ptw', False)):
            print("Trace mode: Using FS mode with timing MMU (timing-PTW enabled)")
        else:
            print("Trace mode: Using FS mode with functional TLB to bypass MMU translation issues")
        print("Trace mode: Configuring expanded memory ranges for trace address mapping")
        # Force functional TLB to bypass complex MMU translation
        args.functional_tlb = True
    else:
        print("Checkpoint mode: Using standard FS mode with normal MMU translation")
    test_sys.num_cpus = np

    test_sys.xiangshan_system = True
    # args.enable_difftest should be normalized by xiangshan_system_init().
    test_sys.enable_difftest = args.enable_difftest

    # Configure XiangShan inputs - skip checkpoint loading in trace mode
    if hasattr(args, 'enable_trace_mode') and args.enable_trace_mode:
        args.difftest_ref_so = None

        # Trace mode FS configuration with functional TLB.
        # We run without a bootloader but must still set the bootloader
        # parameter explicitly, since RiscvBareMetal.bootloader has no
        # default. An empty string is treated as "no bootloader" and we
        # reuse the xiangshan_cpt flag to take the no-bootloader path in
        # the BareMetal workload implementation.
        test_sys.workload.bootloader = ''
        test_sys.workload.xiangshan_cpt = True   # Reuse GCPT path to skip bootloader
        test_sys.restore_from_gcpt = False       # Disable GCPT restoration
        print("Trace mode: Running without bootloader (no GCPT)")

        # Configure DRAMsim3 if needed for memory controller
        if args.mem_type == 'DRAMsim3' and args.dramsim3_ini is None:
            root_dir = os.path.dirname(os.path.dirname(os.path.dirname(__file__)))
            args.dramsim3_ini = os.path.join(root_dir,
                                             'ext/dramsim3/xiangshan_configs/xiangshan_DDR4_8Gb_x8_3200_2ch.ini')

        if bool(getattr(args, 'trace_timing_ptw', False)):
            print("Trace mode: Timing MMU will be applied for timing-PTW")
        else:
            print("Trace mode: FS mode with functional TLB configured to bypass MMU translation issues")
    else:
        # Standard checkpoint-based configuration
        config_xiangshan_inputs(args, test_sys)

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
        cpu.mmu.pma_checker = PMAChecker(
            uncacheable=[AddrRange(0, size=0x80000000)])
        cpu.mmu.functional = args.functional_tlb
        cpu.mmu.enable_sv48 = args.open_sv48

        if hasattr(args, 'enable_trace_mode') and args.enable_trace_mode:
            timing_ptw = bool(getattr(args, 'trace_timing_ptw', False))
            cpu.mmu.functional = not timing_ptw
            mode_str = "timing" if timing_ptw else "functional"
            print(f"Trace mode: CPU {cpu.cpu_id} configured with {mode_str} translation")

    # configure BP
    for i in range(np):
        if args.kmh_align:
            test_sys.cpu[i].enable_storeSet_train = False

        if args.bp_type != 'DecoupledBPUWithBTB':
            fatal(
                "Only --bp-type=DecoupledBPUWithBTB is supported for Xiangshan in this repo "
                f"(got --bp-type={args.bp_type})."
            )

        enable_bp_db = len(args.enable_bp_db) > 1
        if enable_bp_db:
            bp_db_switches = args.enable_bp_db[1] + ['basic']
            print("BP db switches:", bp_db_switches)
        else:
            bp_db_switches = []

        test_sys.cpu[i].branchPred = DecoupledBPUWithBTB(
            bpDBSwitches=bp_db_switches,
        )
        test_sys.cpu[i].branchPred.isDumpMisspredPC = True

    # configure memory related
    if args.mem_type == 'DRAMsim3':
        assert args.dramsim3_ini is not None

    for cpu in test_sys.cpu:
        cpu.store_prefetch_train = not args.kmh_align

    # Configure trace mode if enabled
    if hasattr(args, 'enable_trace_mode') and args.enable_trace_mode:
        if not getattr(args, 'trace_file', None):
            fatal("--trace-file is required when --enable-trace-mode is set")
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

        _apply_trace_timing_ptw_cpu_params(args, test_sys.cpu, shrink_window=False)

        print(f"  Trace file: {args.trace_file}")
        print(f"  Trace format: {args.trace_format}")
        print(f"  Max instructions: {args.maxinsts}")
        print(f"  Decoupled BP: {hasattr(args, 'trace_enable_decoupled_bp') and args.trace_enable_decoupled_bp}")
        if bool(getattr(args, 'trace_timing_ptw', False)):
            print(
                "  Timing PTW: enabled "
                f"(ptw_page_size={getattr(args, 'trace_ptw_page_size', '4k')}, "
                f"reserved_bytes=0x{int(getattr(args, 'trace_ptw_reserved_bytes', 0)):x})"
            )

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
            aligned_base = None
            aligned_total = None
            try:
                base = int(test_sys.mem_ranges[0].start)
                total = 0
                for r in test_sys.mem_ranges:
                    total += int(r.size())
                for cpu in test_sys.cpu:
                    cpu.traceAddrBase = base
                    cpu.traceAddrSize = total
                    cpu.traceAddrMapMode = "linear"
                aligned_base = base
                aligned_total = total
            except Exception as e:
                print(f"Warning: failed to align trace mapping to mem (Ruby path): {e}")
            _apply_trace_timing_ptw_cpu_params(args, test_sys.cpu)
            if aligned_base is not None:
                final_size = int(test_sys.cpu[0].traceAddrSize)
                reserved_bytes = int(getattr(args, 'trace_ptw_reserved_bytes', 0))
                if bool(getattr(args, 'trace_timing_ptw', False)):
                    print(
                        f"Trace mode: Align trace mapping to mem: base=0x{aligned_base:x}, "
                        f"size=0x{final_size:x} (reserved=0x{reserved_bytes:x})"
                    )
                else:
                    print(
                        f"Trace mode: Align trace mapping to mem: base=0x{aligned_base:x}, "
                        f"size=0x{aligned_total:x}"
                    )

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

        # CHI topologies expect a DDRWrapper-backed memory so that the
        # L2ToDramSys.dramsim3 parameter can be bound correctly. Override
        # any other mem_type to avoid type mismatches at assignment time.
        if args.CHI and getattr(args, 'mem_type', None) != 'DDRWrapper':
            warn(
                f"Overriding mem_type {getattr(args, 'mem_type', None)} to DDRWrapper for CHI"
            )
            args.mem_type = 'DDRWrapper'

        CacheConfig.config_cache(args, test_sys)

        l2l3_topologies = (
            'L2L3DramSys',
            'L2L3DramSys_M1Local1Dram',
            'L2L3DramSys_3x3',
            'L2L3DramSys_5x3',
            'L2L3DramSys_6x4',
            'L2L3DramSys_6x6',
        )
        if args.CHI and getattr(args, 'chi_topology', 'L2ToDramSys') in l2l3_topologies:
            # L2L3DramSys owns and wires its DDRWrapper internally.
            # Skip MemConfig to avoid creating an extra unconnected DDRWrapper.
            pass
        else:
            MemConfig.config_mem(args, test_sys)
        if args.CHI:
            if getattr(args, 'chi_topology', 'L2ToDramSys') not in l2l3_topologies:
                test_sys.CHIsys.dramsim3 = test_sys.mem_ctrls[0]
                chi_port_kwargs = dict(
                    credit_model=getattr(args, 'chi_credit_model', 'legacy'),
                    credit_return_direction='down',
                    up_crd_lat_int=getattr(args, 'chi_up_crd_lat_int', 1),
                    up_crd_lat_ext=getattr(args, 'chi_up_crd_lat_ext', 2),
                    dn_crd_lat_int=getattr(args, 'chi_dn_crd_lat_int', 2),
                    dn_crd_lat_ext=getattr(args, 'chi_dn_crd_lat_ext', 1),
                    internal_crd_lat=getattr(args, 'chi_internal_crd_lat', 1),
                )
                chi_rxbuf_num = getattr(args, 'chi_rxbuf_num', 0)
                if chi_rxbuf_num:
                    chi_port_kwargs['rxbuf_num'] = chi_rxbuf_num
                chi_skid_depth = getattr(args, 'chi_skid_depth', 0)
                if chi_skid_depth:
                    chi_port_kwargs['skid_depth'] = chi_skid_depth
                chi_initial_credit_count = getattr(
                    args, 'chi_initial_credit_count', 0)
                if chi_initial_credit_count:
                    chi_port_kwargs['initial_credit_count'] = (
                        chi_initial_credit_count)
                test_sys.CHIsys.dramsim3.networkPort = CHIPort(**chi_port_kwargs)

        # Align trace address mapping window to physical memory size (classic cache path)
        if hasattr(args, 'enable_trace_mode') and args.enable_trace_mode:
            aligned_base = None
            aligned_total = None
            try:
                base = int(test_sys.mem_ranges[0].start)
                total = 0
                for r in test_sys.mem_ranges:
                    total += int(r.size())
                for cpu in test_sys.cpu:
                    cpu.traceAddrBase = base
                    cpu.traceAddrSize = total
                aligned_base = base
                aligned_total = total
            except Exception as e:
                print(f"Warning: failed to align trace mapping to mem: {e}")
            _apply_trace_timing_ptw_cpu_params(args, test_sys.cpu)
            if aligned_base is not None:
                final_size = int(test_sys.cpu[0].traceAddrSize)
                reserved_bytes = int(getattr(args, 'trace_ptw_reserved_bytes', 0))
                if bool(getattr(args, 'trace_timing_ptw', False)):
                    print(
                        f"Trace mode: Align trace mapping to mem: base=0x{aligned_base:x}, "
                        f"size=0x{final_size:x} (reserved=0x{reserved_bytes:x})"
                    )
                else:
                    print(
                        f"Trace mode: Align trace mapping to mem: base=0x{aligned_base:x}, "
                        f"size=0x{aligned_total:x}"
                    )

    if args.mmc_img:
        for mmc, cpu in zip(test_sys.mmcs, test_sys.cpu):
            mmc.cpt_bin_path = args.mmc_cptbin
            mmc.img_path = args.mmc_img
            cpu.nemuSDCptBin = mmc.cpt_bin_path
            cpu.nemuSDimg = mmc.img_path

    config_difftest(test_sys.cpu, args, test_sys)

    # configure vector
    if args.enable_riscv_vector:
        test_sys.enable_riscv_vector = True
        for cpu in test_sys.cpu:
            cpu.enable_riscv_vector = True

    # config arch db
    if args.enable_arch_db:
        perfCCT_cmd = "CREATE TABLE LifeTimeCommitTrace(ID INTEGER PRIMARY KEY AUTOINCREMENT,"
        perfCCT_cmd += PerfRecord.vals[0] + " bigint unsigned NOT NULL"
        for i in range(1, len(PerfRecord.vals)):
            name = PerfRecord.vals[i]
            type_str = "bigint unsigned" if name.lower().startswith(('at', 'pc')) else "char(20)"
            perfCCT_cmd += "," + name + " " + type_str + " NOT NULL"
        perfCCT_cmd += ");"

        perfCCT_cmd += """
CREATE TABLE LoadLifeTimeCommitTrace(
    ID int unsigned PRIMARY KEY,
    VAddress bigint unsigned not null,
    PAddress bigint unsigned not null,
    LastReplay bigint unsigned not null,
    ReplayStr char(10) not null,
    constraint fk_id
        foreign key (ID) references LifeTimeCommitTrace(ID)
);

"""

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
        test_sys.arch_db.dump_vaddr_trace = False
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
            "CREATE TABLE vaddrTrace(" \
            "ID INTEGER PRIMARY KEY AUTOINCREMENT," \
            "PC INT NOT NULL," \
            "VADDR INT NOT NULL," \
            "Hit INT NOT NULL," \
            "Tick INT NOT NULL," \
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


def xiangshan_system_init():
    _warn_if_deprecated_xiangshan_entrypoint()
    # Add args
    parser = argparse.ArgumentParser()
    Options.addCommonOptions(parser, configure_xiangshan=True)
    Options.addXiangshanFSOptions(parser)
    Options.addXiangshanTraceOptions(parser)
    parser.add_argument(
        "--chi-topology",
        type=str,
        choices=[
            "L2ToDramSys",
            "L2ToDramSys_M1Local1Dram",
            "L2L3DramSys",
            "L2L3DramSys_M1Local1Dram",
            "L2L3DramSys_3x3",
            "L2L3DramSys_5x3",
            "L2L3DramSys_6x4",
            "L2L3DramSys_6x6",
        ],
        default="L2ToDramSys",
        help="Select CHI topology object when --CHI is enabled",
    )

    # Add the ruby specific and protocol specific args
    if '--ruby' in sys.argv:
        Ruby.define_options(parser)
    args = parser.parse_args()

    # Match the memories with the CPUs, based on the options for the test system
    TestMemClass = Simulation.setMemClass(args)

    args.xiangshan_system = True
    # Only enable difftest if not in trace mode - trace mode doesn't need reference model verification
    if not (hasattr(args, 'enable_trace_mode') and args.enable_trace_mode):
        args.enable_difftest = True
    else:
        args.enable_difftest = False
        print("Trace mode: Difftest disabled for trace execution")
    args.enable_riscv_vector = True

    return args
