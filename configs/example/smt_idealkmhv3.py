from m5.objects import Root

import os

from m5.util import addToPath

addToPath('../')

from common import Simulation
from common.xiangshan import build_xiangshan_system, xiangshan_system_init
from idealkmhv3 import setKmhV3IdealParams


def setSharedLSQParams(args, system):
    setKmhV3IdealParams(args, system)

    for cpu in system.cpu:
        # Reuse the ideal KMHV3 LSQ-related sizes, but interpret them as a
        # shared SMT-wide pool. For example, LQEntries=128 means both threads
        # compete for a total of 128 load entries instead of 128 each. The
        # same shared-mode accounting applies to SQ/RARQ/RAWQ. Likewise,
        # branchPred.ftq_size is interpreted as a shared SMT-wide FTQ pool.
        # Keep FTQ partitioned by default so one thread cannot monopolize the
        # shared target queue and starve the other thread's frontend.
        cpu.smtLSQMode = 'Shared'
        cpu.smtLSQPolicy = 'Dynamic'
        cpu.smtROBPolicy = 'DynamicBorrowing'
        cpu.branchPred.smtFTQMode = 'Shared'
        cpu.branchPred.smtFTQPolicy = 'Partitioned'


if __name__ == '__m5_main__':
    FutureClass = None

    args = xiangshan_system_init()

    assert not args.external_memory_system

    args.smt = True
    args.bp_type = 'DecoupledBPUWithBTB'
    args.l2_size = '2MB'
    args.l3_size = '32MB'

    if args.dramsim3_ini is None:
        gem5_root = os.path.abspath(os.path.join(os.path.dirname(__file__), '../..'))
        args.dramsim3_ini = os.path.join(
            gem5_root,
            'ext/dramsim3/xiangshan_configs/xiangshan_DDR4_8Gb_x8_3200_8ch.ini'
        )

    Simulation.setMemClass(args)

    test_sys = build_xiangshan_system(args)
    setSharedLSQParams(args, test_sys)

    root = Root(full_system=True, system=test_sys)

    Simulation.run_vanilla(args, root, test_sys, FutureClass)
