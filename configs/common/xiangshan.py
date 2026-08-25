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


def resolve_linux_cmdline(args: argparse.Namespace, default_cmdline: str) -> str:
    command_line = getattr(args, "command_line", None)
    command_line_file = getattr(args, "command_line_file", None)

    if command_line and command_line_file:
        fatal("--command-line and --command-line-file are mutually exclusive")

    if command_line:
        return command_line.strip()

    if command_line_file:
        with open(command_line_file) as cmdline_file:
            return cmdline_file.read().strip()

    return default_cmdline


def generate_xiangshan_dtb(system, *, cmdline: str, outdir: str = None) -> str:
    if outdir is None:
        outdir = m5.options.outdir
    dtb_path = os.path.join(outdir, "device.dtb")
    dts_path = os.path.join(outdir, "device.dts")
    state = FdtState(addr_cells=2, size_cells=2, cpu_cells=1)
    root = FdtNode("/")
    root.append(state.addrCellsProperty())
    root.append(state.sizeCellsProperty())
    root.appendCompatible(["freechips,rocketchip-unknown-soc"])
    root.append(FdtPropertyStrings("model", "xiangshan-raw-linux"))

    chosen = FdtNode("chosen")
    if cmdline:
        chosen.append(FdtPropertyStrings("bootargs", cmdline))
    chosen.append(FdtPropertyStrings("stdout-path", "/soc/serial@40600000"))
    chosen.append(FdtPropertyStrings("linux,stdout-path", "/soc/serial@40600000"))
    root.append(chosen)

    for mem_range in system.mem_ranges:
        node = FdtNode("memory@%x" % int(mem_range.start))
        node.append(FdtPropertyStrings("device_type", ["memory"]))
        node.append(
            FdtPropertyWords(
                "reg",
                state.addrCells(mem_range.start) +
                state.sizeCells(mem_range.size())
            )
        )
        root.append(node)

    cpus_node = FdtNode("cpus")
    cpus_state = FdtState(addr_cells=1, size_cells=0)
    cpus_node.append(cpus_state.addrCellsProperty())
    cpus_node.append(cpus_state.sizeCellsProperty())
    cpus_node.append(FdtPropertyWords("timebase-frequency", [1000000]))

    mmu_type = "riscv,sv48"
    isa_string = "rv64imafdc"

    for i, cpu in enumerate(system.cpu):
        node = FdtNode(f"cpu@{i}")
        node.append(FdtPropertyStrings("device_type", "cpu"))
        node.append(FdtPropertyWords("reg", state.CPUAddrCells(i)))
        node.append(FdtPropertyStrings("mmu-type", mmu_type))
        node.append(FdtPropertyStrings("status", "okay"))
        node.append(FdtPropertyStrings("riscv,isa", isa_string))
        freq = int(cpu.clk_domain.unproxy(cpu).clock[0].frequency)
        node.append(FdtPropertyWords("clock-frequency", freq))
        node.appendCompatible(["riscv"])
        node.appendPhandle(f"cpu@{i}")

        int_node = FdtNode("interrupt-controller")
        int_state = FdtState(interrupt_cells=1)
        int_phandle = int_state.phandle(f"cpu@{i}.int_state")
        int_node.append(int_state.interruptCellsProperty())
        int_node.append(FdtProperty("interrupt-controller"))
        int_node.appendCompatible("riscv,cpu-intc")
        int_node.append(FdtPropertyWords("phandle", [int_phandle]))

        node.append(int_node)
        cpus_node.append(node)

    root.append(cpus_node)

    soc_node = FdtNode("soc")
    soc_state = FdtState(addr_cells=2, size_cells=2)
    soc_node.append(soc_state.addrCellsProperty())
    soc_node.append(soc_state.sizeCellsProperty())
    soc_node.append(FdtProperty("ranges"))
    soc_node.appendCompatible(["simple-bus"])

    clint = system.lint
    clint_node = clint.generateBasicPioDeviceNode(
        soc_state, "clint", clint.pio_addr, clint.pio_size
    )
    clint_interrupts = []
    for i, _cpu in enumerate(system.cpu):
        phandle = soc_state.phandle(f"cpu@{i}.int_state")
        clint_interrupts.extend([phandle, 0x3, phandle, 0x7])
    clint_node.append(FdtPropertyWords("interrupts-extended", clint_interrupts))
    clint_node.appendCompatible(["riscv,clint0"])
    soc_node.append(clint_node)

    plic = system.plic
    plic_node = plic.generateBasicPioDeviceNode(
        soc_state, "plic", plic.pio_addr, plic.pio_size
    )
    plic_int_state = FdtState(addr_cells=0, interrupt_cells=1)
    plic_node.append(plic_int_state.addrCellsProperty())
    plic_node.append(plic_int_state.interruptCellsProperty())
    plic_phandle = plic_int_state.phandle("xiangshan-plic")
    plic_node.append(FdtPropertyWords("phandle", [plic_phandle]))
    plic_node.append(FdtPropertyWords("riscv,ndev", [31]))
    plic_interrupts = []
    for i, _cpu in enumerate(system.cpu):
        phandle = state.phandle(f"cpu@{i}.int_state")
        plic_interrupts.extend([phandle, 0xB, phandle, 0x9])
    plic_node.append(FdtPropertyWords("interrupts-extended", plic_interrupts))
    plic_node.append(FdtProperty("interrupt-controller"))
    plic_node.appendCompatible(["riscv,plic0"])
    soc_node.append(plic_node)

    uart = system.uartlite
    uart_node = uart.generateBasicPioDeviceNode(
        soc_state, "serial", uart.pio_addr, uart.pio_size
    )
    uart_node.append(FdtPropertyWords("clock-frequency", [0]))
    uart_node.append(FdtPropertyStrings("status", "okay"))
    uart_node.appendCompatible(["xlnx,xps-uartlite-1.00.a"])
    soc_node.append(uart_node)

    root.append(soc_node)

    fdt = Fdt()
    fdt.add_rootnode(root)
    fdt.writeDtsFile(dts_path)
    fdt.writeDtbFile(dtb_path)
    return dtb_path


def configure_xiangshan_linux_workload(system, args: argparse.Namespace,
                                       default_cmdline: str = "console=ttyS0 earlycon=sbi loglevel=7") -> None:
    cmdline = resolve_linux_cmdline(args, default_cmdline)
    if hasattr(system.workload, "command_line"):
        system.workload.command_line = cmdline
    system.workload.dtb_addr = 0x87e00000

    if getattr(args, "dtb_filename", None):
        dtb_path = args.dtb_filename
    else:
        dtb_path = generate_xiangshan_dtb(system, cmdline=cmdline)
    system.workload.dtb_filename = dtb_path


def resolve_xiangshan_ref_so(args: argparse.Namespace):
    ref_so = None
    if args.difftest_ref_so is not None:
        ref_so = args.difftest_ref_so
        print("Obtained ref_so from args.difftest_ref_so: ", ref_so)
    elif (args.num_cpus > 1 or args.smt) and "GCBV_MULTI_CORE_REF_SO" in os.environ:
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
        fatal("No valid ref_so file specified for the functional model to "
              "compare against. Please 1) either specify a valid ref_so file using "
              "the --difftest-ref-so option;\n"
              "2) or specify GCBV_REF_SO/GCBV_MULTI_CORE_REF_SO/GCBH_REF_SO that points to the ref_so file;\n"
              "3) or specify NEMU_HOME that contains build/riscv64-nemu-interpreter-so")
    return ref_so


def get_xiangshan_cpu_class(args: argparse.Namespace):
    if args.xiangshan_ecore:
        args.cpu_clock = '2.4GHz'
        return XiangshanECore
    return XiangshanCore


def config_xiangshan_inputs(args: argparse.Namespace, sys):
    ref_so = None

    if args.enable_difftest:
        ref_so = resolve_xiangshan_ref_so(args)

    args.difftest_ref_so = ref_so

    if args.gcpt_restorer is None:
        if args.raw_cpt:
            # If using raw binary, no restorer is needed.
            gcpt_restorer = None
        elif args.num_cpus > 1 or args.smt:
            if "GCB_MULTI_CORE_RESTORER" in os.environ:
                gcpt_restorer = os.environ["GCB_MULTI_CORE_RESTORER"]
                print("Obtained gcpt_restorer from GCB_MULTI_CORE_RESTORER: ", gcpt_restorer)
            else:
                fatal("Plz set $GCB_MULTI_CORE_RESTORER when model Xiangshan with multi-context difftest")
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
            gcpt_restorer = ""
            print("Using restorer embedded in checkpoint")
    else:
        print("Obtained gcpt_restorer from args.gcpt_restorer: ", args.gcpt_restorer)
        gcpt_restorer = args.gcpt_restorer

    if args.num_cpus > 1 or args.smt:
        print("Simulating a multi-context system, demanding a larger GCPT restorer size (2M).")
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
        if len(cpu_list) > 1 or args.smt:
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

def _finish_xiangshan_system(args, test_sys, TestCPUClass, ruby):
    np = args.num_cpus
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
    if args.smt:
        test_sys.multi_thread = True

    for cpu in test_sys.cpu:
        if args.smt:
            cpu.numThreads = 2
        cpu.mmu.pma_checker = PMAChecker(
            uncacheable=[AddrRange(0, size=0x80000000)])
        cpu.mmu.functional = args.functional_tlb

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
            bp_db_switches = list(args.enable_bp_db[1])
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

        CacheConfig.config_cache(args, test_sys)

        MemConfig.config_mem(args, test_sys)

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
    if args.dump_bop_replay_trace and not args.enable_arch_db:
        raise RuntimeError("--dump-bop-replay-trace requires --enable-arch-db")
    if args.dump_bop_direct_quality_trace and not args.enable_arch_db:
        raise RuntimeError(
            "--dump-bop-direct-quality-trace requires --enable-arch-db")
    if args.dump_bop_direct_quality_trace and not args.enable_bop_direct_quality_gate:
        raise RuntimeError(
            "--dump-bop-direct-quality-trace requires "
            "--enable-bop-direct-quality-gate")
    if args.dump_bop_direct_quality_trace and not args.arch_db_fromstart:
        raise RuntimeError(
            "--dump-bop-direct-quality-trace requires --arch-db-fromstart")

    if args.enable_arch_db:
        perfCCT_cmd = "CREATE TABLE LifeTimeCommitTrace(ID INTEGER PRIMARY KEY AUTOINCREMENT,"
        perfCCT_cmd += PerfRecord.vals[0] + " bigint unsigned NOT NULL"
        for i in range(1, len(PerfRecord.vals)):
            name = PerfRecord.vals[i]
            type_str = "bigint unsigned" if name.lower().startswith(('at', 'pc', 'result')) else "char(20)"
            perfCCT_cmd += "," + name + " " + type_str + " NOT NULL"
        perfCCT_cmd += ",TID int unsigned NOT NULL"
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
        test_sys.arch_db.dump_bop_validation_trace = args.dump_bop_validation_trace
        test_sys.arch_db.dump_bop_replay_trace = args.dump_bop_replay_trace
        test_sys.arch_db.dump_bop_direct_quality_trace = \
            args.dump_bop_direct_quality_trace
        test_sys.arch_db.dump_stride_train_trace = False
        test_sys.arch_db.dump_sms_train_trace = False
        test_sys.arch_db.dump_vaddr_trace = False
        test_sys.arch_db.dump_lifetime = args.arch_db_dump_lifetime
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
            "CREATE TABLE BOPValidationTrace(" \
            "ID INTEGER PRIMARY KEY AUTOINCREMENT," \
            "Tick INT NOT NULL," \
            "Event TEXT NOT NULL," \
            "BOPName TEXT NOT NULL," \
            "TriggerPC INT NOT NULL," \
            "TriggerAddr INT NOT NULL," \
            "ValidationAddr INT NOT NULL," \
            "PrefetchAddr INT NOT NULL," \
            "BestOffset INT NOT NULL," \
            "BestScore INT NOT NULL," \
            "Round INT NOT NULL," \
            "Late BOOL NOT NULL," \
            "TriggerIsDemand BOOL NOT NULL," \
            "TriggerCacheMiss BOOL NOT NULL," \
            "TriggerPFSource INT NOT NULL," \
            "TriggerPFFirstHit BOOL NOT NULL," \
            "TriggerPFHit BOOL NOT NULL," \
            "IssueEnabled INT NOT NULL," \
            "ValidationEnabled INT NOT NULL," \
            "ValidationHit INT NOT NULL," \
            "PCConfidenceEnabled INT NOT NULL," \
            "PCIndex INT NOT NULL," \
            "PCTag INT NOT NULL," \
            "PCEntryHit INT NOT NULL," \
            "PCConfidence INT NOT NULL," \
            "PCState INT NOT NULL," \
            "PCSampled INT NOT NULL," \
            "PCEpoch INT NOT NULL," \
            "Suppressed BOOL NOT NULL," \
            "Generated BOOL NOT NULL," \
            "Buffered BOOL NOT NULL," \
            "Filtered BOOL NOT NULL," \
            "FilterPassed BOOL NOT NULL," \
            "PCConfidenceAfter INT NOT NULL," \
            "PCUpdateDecayed BOOL NOT NULL," \
            "PCUpdateParticipants INT NOT NULL," \
            "PCOffsetChanged BOOL NOT NULL," \
            "OutcomeAddr INT NOT NULL," \
            "OutcomePC INT NOT NULL," \
            "OutcomePFSource INT NOT NULL," \
            "OutcomeIsDemand BOOL NOT NULL," \
            "OutcomeCacheMiss BOOL NOT NULL," \
            "SITE TEXT," \
            "PCLowEntryMissStreak INT NOT NULL," \
            "PCUpdateLowEntryMissStreakBefore INT NOT NULL," \
            "PCUpdateLowEntryMissStreakAfter INT NOT NULL," \
            "PCUpdateLowEntryHysteresisHeld BOOL NOT NULL," \
            "PCUpdateLowEntryHysteresisTransition BOOL NOT NULL);"
            ,
            "CREATE TABLE BOPReplayMeta(" \
            "SchemaVersion INT NOT NULL," \
            "BOPName TEXT PRIMARY KEY," \
            "BlockSize INT NOT NULL," \
            "ScoreMax INT NOT NULL," \
            "RoundMax INT NOT NULL," \
            "BadScore INT NOT NULL," \
            "RREntries INT NOT NULL," \
            "TagBits INT NOT NULL," \
            "DelayQueueEnabled BOOL NOT NULL," \
            "DelayQueueSize INT NOT NULL," \
            "DelayTicks INT NOT NULL," \
            "CrossPage BOOL NOT NULL," \
            "AdaptOffset BOOL NOT NULL," \
            "IssueValidation BOOL NOT NULL," \
            "PCValidationConfidence BOOL NOT NULL," \
            "PCValidationProducerConsumer BOOL NOT NULL," \
            "GlobalCoverageGuard BOOL NOT NULL," \
            "PCValidationEntries INT NOT NULL," \
            "PCValidationTagBits INT NOT NULL," \
            "PCValidationCounterBits INT NOT NULL," \
            "PCValidationInitial INT NOT NULL," \
            "PCValidationMediumThreshold INT NOT NULL," \
            "PCValidationHighThreshold INT NOT NULL," \
            "PCValidationHitIncrement INT NOT NULL," \
            "PCValidationMediumSamplePeriod INT NOT NULL," \
            "PCValidationMissDecayPeriod INT NOT NULL," \
            "PCValidationLowEntryMissStreakThreshold INT NOT NULL," \
            "PCValidationEpochBits INT NOT NULL," \
            "PCValidationOffsetContextSlots INT NOT NULL," \
            "GlobalBOPUnusedThreshold INT NOT NULL," \
            "GlobalBOPMinResolvedCoverageShift INT NOT NULL," \
            "NegativeOffsetsEnabled BOOL NOT NULL," \
            "AutoLearning BOOL NOT NULL," \
            "VictimOffsetsListSize INT NOT NULL," \
            "RestoreCycle INT NOT NULL," \
            "ClockPeriodTicks INT NOT NULL," \
            "Offsets TEXT NOT NULL);"
            ,
            "CREATE TABLE BOPReplayPhase(" \
            "PhaseId INT PRIMARY KEY," \
            "PhaseName TEXT NOT NULL UNIQUE," \
            "StartTick INT NOT NULL);"
            ,
            "CREATE TABLE L2DemandTrace(" \
            "AccessSeq INT PRIMARY KEY," \
            "PhaseId INT NOT NULL," \
            "Tick INT NOT NULL," \
            "Addr INT NOT NULL," \
            "PC INT NOT NULL," \
            "HasPC BOOL NOT NULL," \
            "CacheMiss BOOL NOT NULL," \
            "PrefetchSource INT NOT NULL," \
            "PfFirstHit BOOL NOT NULL," \
            "PfHit BOOL NOT NULL);"
            ,
            "CREATE TABLE BOPReplayEvent(" \
            "AccessSeq INT NOT NULL," \
            "BOPName TEXT NOT NULL," \
            "BOPKind TEXT NOT NULL," \
            "ReplayOrder INT NOT NULL," \
            "PhaseId INT NOT NULL," \
            "Tick INT NOT NULL," \
            "TriggerAddr INT NOT NULL," \
            "TriggerPC INT NOT NULL," \
            "TriggerHasPC BOOL NOT NULL," \
            "TriggerIsDemand BOOL NOT NULL," \
            "TriggerIsRead BOOL NOT NULL," \
            "TriggerCacheMiss BOOL NOT NULL," \
            "TriggerPFSource INT NOT NULL," \
            "TriggerPFFirstHit BOOL NOT NULL," \
            "TriggerPFHit BOOL NOT NULL," \
            "Late BOOL NOT NULL," \
            "BestOffsetBefore INT NOT NULL," \
            "BestOffsetAfter INT NOT NULL," \
            "BestScore INT NOT NULL," \
            "Round INT NOT NULL," \
            "BestOffsetChanged BOOL NOT NULL," \
            "IssueEnabled BOOL NOT NULL," \
            "ValidationEnabled BOOL NOT NULL," \
            "ValidationHit INT NOT NULL," \
            "PCConfidenceEnabled BOOL NOT NULL," \
            "PCIndex INT NOT NULL," \
            "PCTag INT NOT NULL," \
            "PCEntryHit INT NOT NULL," \
            "PCConfidence INT NOT NULL," \
            "PCState INT NOT NULL," \
            "PCSampled BOOL NOT NULL," \
            "PCLowEntryMissStreak INT NOT NULL," \
            "PCEpoch INT NOT NULL," \
            "GlobalBypassActive BOOL NOT NULL," \
            "PolicySuppressed BOOL NOT NULL," \
            "RawCandidateValid BOOL NOT NULL," \
            "RawCandidateAddr INT NOT NULL," \
            "PolicyCandidateValid BOOL NOT NULL," \
            "PolicyCandidateAddr INT NOT NULL," \
            "ValidationAddr INT NOT NULL," \
            "PrefetchAddr INT NOT NULL," \
            "OnlineGenerated BOOL NOT NULL," \
            "OnlineBuffered BOOL NOT NULL," \
            "OnlineFiltered BOOL NOT NULL," \
            "OnlineFilterPassed BOOL NOT NULL," \
            "PRIMARY KEY(AccessSeq, BOPName));"
            ,
            "CREATE TABLE BOPReplayDelayAction(" \
            "BOPName TEXT NOT NULL," \
            "ReplayOrder INT NOT NULL," \
            "Action TEXT NOT NULL," \
            "Tick INT NOT NULL," \
            "Addr INT NOT NULL," \
            "ProcessTick INT NOT NULL," \
            "QueueSizeAfter INT NOT NULL," \
            "PRIMARY KEY(BOPName, ReplayOrder));"
            ,
            "CREATE TABLE BOPDirectQualityMeta(" \
            "SchemaVersion INT PRIMARY KEY," \
            "Horizon INT NOT NULL," \
            "FeedbackEntries INT NOT NULL," \
            "FeedbackWays INT NOT NULL);"
            ,
            "CREATE TABLE BOPDirectQualityIssue(" \
            "EventSequence INT PRIMARY KEY," \
            "FeedbackId INT NOT NULL UNIQUE," \
            "IssueDemandSequence INT NOT NULL," \
            "Tick INT NOT NULL," \
            "Line INT NOT NULL," \
            "Kind INT NOT NULL);"
            ,
            "CREATE TABLE BOPDirectQualityDemand(" \
            "EventSequence INT PRIMARY KEY," \
            "DemandSequence INT NOT NULL UNIQUE," \
            "Tick INT NOT NULL," \
            "Line INT NOT NULL);"
            ,
            "CREATE TABLE BOPDirectQualityOutcome(" \
            "EventSequence INT PRIMARY KEY," \
            "FeedbackId INT NOT NULL UNIQUE," \
            "ResolveDemandSequence INT NOT NULL," \
            "Tick INT NOT NULL," \
            "Line INT NOT NULL," \
            "Outcome TEXT NOT NULL);"
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
            ,
            "CREATE TABLE StrideTrainTrace(" \
            "ID INTEGER PRIMARY KEY AUTOINCREMENT," \
            "Tick INT NOT NULL," \
            "Addr INT NOT NULL," \
            "PC INT NOT NULL," \
            "HashPC INT NOT NULL," \
            "QueryHit BOOL NOT NULL," \
            "IsFirstShot BOOL NOT NULL," \
            "Miss BOOL NOT NULL," \
            "IsTrain BOOL NOT NULL," \
            "SITE TEXT);"
            ,
            "CREATE TABLE DespacitoTrainTrace(" \
            "ID INTEGER PRIMARY KEY AUTOINCREMENT," \
            "Tick INT NOT NULL," \
            "vAddr INT NOT NULL," \
            "pAddr INT NOT NULL," \
            "PC INT NOT NULL," \
            "hasPC BOOL NOT NULL," \
            "Miss BOOL NOT NULL," \
            "IsTrain BOOL NOT NULL," \
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


def build_xiangshan_system(args):
    np = args.num_cpus
    assert buildEnv['TARGET_ISA'] == "riscv"

    TestCPUClass = get_xiangshan_cpu_class(args)
    ruby = bool(hasattr(args, 'ruby') and args.ruby)
    num_threads = np * (2 if getattr(args, 'smt', False) else 1)

    test_sys = makeBareMetalXiangshanSystem(
        'timing', SysConfig(mem=args.mem_size), None, np=np, ruby=ruby,
        num_threads=num_threads)

    if hasattr(args, 'enable_trace_mode') and args.enable_trace_mode:
        if bool(getattr(args, 'trace_timing_ptw', False)):
            print("Trace mode: Using FS mode with timing MMU (timing-PTW enabled)")
        else:
            print("Trace mode: Using FS mode with functional TLB to bypass MMU translation issues")
        print("Trace mode: Configuring expanded memory ranges for trace address mapping")
        args.functional_tlb = True
    else:
        print("Checkpoint mode: Using standard FS mode with normal MMU translation")
    test_sys.num_cpus = np
    test_sys.xiangshan_system = True
    test_sys.enable_difftest = args.enable_difftest

    if hasattr(args, 'enable_trace_mode') and args.enable_trace_mode:
        args.difftest_ref_so = None
        test_sys.workload.bootloader = ''
        test_sys.workload.xiangshan_cpt = True
        test_sys.restore_from_gcpt = False
        print("Trace mode: Running without bootloader (no GCPT)")

        if args.mem_type == 'DRAMsim3' and args.dramsim3_ini is None:
            root_dir = os.path.dirname(os.path.dirname(os.path.dirname(__file__)))
            args.dramsim3_ini = os.path.join(root_dir,
                                             'ext/dramsim3/xiangshan_configs/xiangshan_DDR4_8Gb_x8_3200_2ch.ini')

        if bool(getattr(args, 'trace_timing_ptw', False)):
            print("Trace mode: Timing MMU will be applied for timing-PTW")
        else:
            print("Trace mode: FS mode with functional TLB configured to bypass MMU translation issues")
    else:
        config_xiangshan_inputs(args, test_sys)

    return _finish_xiangshan_system(args, test_sys, TestCPUClass, ruby)


def xiangshan_system_init():
    _warn_if_deprecated_xiangshan_entrypoint()
    # Add args
    parser = argparse.ArgumentParser()
    Options.addCommonOptions(parser, configure_xiangshan=True)
    Options.addXiangshanFSOptions(parser)
    Options.addXiangshanTraceOptions(parser)
    parser.add_argument(
        "--btb-tage-upper-bound",
        action="store_true",
        default=False,
        help="Use BTBTAGEUpperBound in kmhv3 instead of the default BTBTAGE",
    )
    parser.add_argument(
        "--disable-l1-direct-compression",
        action="store_false",
        dest="enable_l1_direct_compression",
        default=True,
        help="Disable L1 direct one-stage TLB compression for A/B validation",
    )
    parser.add_argument(
        "--disable-ptw-level-limit",
        action="store_false",
        dest="enable_ptw_level_limit",
        default=True,
        help="Disable PTW level parallelism limits for A/B validation",
    )
    parser.add_argument(
        "--ptw-level0-limit",
        type=int,
        default=6,
        help="PTW level-0 parallelism limit",
    )
    parser.add_argument(
        "--ptw-level1-limit",
        type=int,
        default=1,
        help="PTW level-1 parallelism limit",
    )
    parser.add_argument(
        "--ptw-level2-limit",
        type=int,
        default=1,
        help="PTW level-2 parallelism limit",
    )
    parser.add_argument(
        "--ptw-level3-limit",
        type=int,
        default=1,
        help="PTW level-3 parallelism limit",
    )
    parser.add_argument(
        "--ptw-miss-queue-size",
        type=int,
        default=40,
        help="PTW MissQueue size",
    )
    parser.add_argument(
        "--smtROBDonorEntry",
        type=int,
        default=8,
        help="Minimum ROB entries reserved for a borrowing donor to resume",
    )
    parser.add_argument(
        "--smtROBBaseEntry",
        type=int,
        default=80,
        help="Minimum ROB entries reserved for a borrowing base to resume",
    )
    parser.add_argument(
        "--ROBTotalEntry",
        type=int,
        default=160,
        help="Number of reorder buffer entries",
    )
    parser.add_argument(
        "--standalone-sc",
        action="store_true",
        default=False,
        help="Disable direction TAGE sources in kmhv3 and force MGSC standalone SC prediction",
    )
    parser.add_argument(
        "--solver-problem-ref",
        type=str,
        default="",
        help="Solver problem spec used by the CI parameter solver prototype",
    )
    parser.add_argument(
        "--solver-bind-output",
        type=str,
        default="",
        help="Write solver binding metadata and exit before instantiate",
    )
    parser.add_argument(
        "--solver-overlay",
        type=str,
        default="",
        help="Apply a solver overlay JSON before instantiate",
    )
    parser.add_argument(
        "--rob-walk-policy",
        type=str,
        default="Replay",
        choices=["Rollback", "Replay", "ConstCycle", "NaiveCpt"],
        help="ROB misprediction-recovery walk policy. NaiveCpt enables the "
              "RAT-checkpoint recovery-cost model.",
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
        if args.enable_difftest is None:
            args.enable_difftest = True
    else:
        args.enable_difftest = False
        print("Trace mode: Difftest disabled for trace execution")
    args.enable_riscv_vector = True

    return args
