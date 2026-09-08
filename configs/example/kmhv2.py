import argparse
import sys

import m5
from m5.defines import buildEnv
from m5.objects import *
from m5.util import addToPath, fatal, warn
from m5.util.fdthelper import *

addToPath('../')
addToPath('../../')

from ruby import Ruby

from common.FSConfig import *
from common.SysPaths import *
from common.Benchmarks import *
from common import Simulation
from common.Caches import *
from common.xiangshan import *
from util.solver.runtime.integration import maybe_handle_solver_runtime

if __name__ == '__m5_main__':

    args = xiangshan_system_init()
    # Keep kmhv2 runnable, but align with repo policy (BTB-only).
    args.bp_type = 'DecoupledBPUWithBTB'

    # l1cache prefetcher use stream, stride
    # l2cache prefetcher use pht, bop, cmc
    # disable l1prefetcher store pf train
    # disable l1 berti, l2 cdp
    args.l2_wrapper_hwp_type = "L2CompositeWithWorkerPrefetcher"
    args.kmh_align = True
    assert not args.external_memory_system

    test_mem_mode = 'timing'

    # override cpu class and clock
    if args.xiangshan_ecore:
        FutureClass = None
        args.cpu_clock = '2.4GHz'
    else:
        FutureClass = None

    test_sys = build_xiangshan_system(args)

    root = Root(full_system=True, system=test_sys)
    if maybe_handle_solver_runtime(root, args):
        sys.exit(0)

    Simulation.run_vanilla(args, root, test_sys, FutureClass)
