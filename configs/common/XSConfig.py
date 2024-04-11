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
        if (not args.enable_riscv_vector) and "GCB_REF_SO" in os.environ:
            ref_so = os.environ["GCB_REF_SO"]
        elif args.enable_riscv_vector and "GCBV_REF_SO" in os.environ:
            ref_so = os.environ["GCBV_REF_SO"]
        elif "NEMU_HOME" in os.environ:
            ref_so = os.path.join(os.environ["NEMU_HOME"], "build/riscv64-nemu-interpreter-so")
        else:
            fatal("No valid ref_so file specified for the functional model to "
                  "compare against. Please 1) either specify a valid ref_so file using "
                  "the --difftest-ref-so option;\n"
                  "2) or specify GCB_REF_SO or GCBV_REF_SO that points to the ref_so file;\n"
                  "3) or specify NEMU_HOME that contains build/riscv64-nemu-interpreter-so")
    args.difftest_ref_so = ref_so

    if args.gcpt_restorer is None:
        if args.enable_riscv_vector:
            if "GCBV_RESTORER" in os.environ:
                gcpt_restorer = os.environ["GCBV_RESTORER"]
            else:
                fatal("Plz set $GCBV_RESTORER when model Xiangshan with vector")
        else:
            if "GCB_RESTORER" in os.environ:
                gcpt_restorer = os.environ["GCB_RESTORER"]
            else:
                fatal("Plz set $GCB_RESTORER or pass it through --gcpt-restorer"
                      " when model Xiangshan without vector")
    else:
        gcpt_restorer = args.gcpt_restorer

    if args.num_cpus > 1:
        print("Simulating a multi-core system, demanding a larger GCPT restorer size (2M).")
        sys.gcpt_restorer_size_limit = 2**20
    elif args.enable_riscv_vector:
        print("Simulating a multi-core system, demanding a median GCPT restorer size (0x1000).")
        sys.gcpt_restorer_size_limit = 0x1000
    else:
        print("Simulating a multi-core system, demanding a basic GCPT restorer size (0x700).")
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
            print('Using raw bbl', gcpt_restorer)
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


def config_difftest(cpu_list, args):
    if not args.enable_difftest:
        return
    else:
        assert len(cpu_list) == 1
        cpu_list[0].enable_difftest = True
        cpu_list[0].enable_mem_dedup = True
        cpu_list[0].difftest_ref_so = args.difftest_ref_so
