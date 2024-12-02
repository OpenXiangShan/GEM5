import argparse
import os

import m5
from m5.defines import buildEnv
from m5.objects import *
from m5.util import addToPath, fatal, warn
from m5.util.fdthelper import *

addToPath('../')


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

    # configure DRAMSim input
    if args.mem_type == 'DRAMsim3' and args.dramsim3_ini is None:
        home = None
        if 'gem5_home' in os.environ:
            home = os.environ['gem5_home']
        if 'GEM5_HOME' in os.environ:
            home = os.environ['GEM5_HOME']
        args.dramsim3_ini = os.path.join(home, 'ext/dramsim3/xiangshan_configs/xiangshan_DDR4_8Gb_x8_3200_2ch.ini')
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
