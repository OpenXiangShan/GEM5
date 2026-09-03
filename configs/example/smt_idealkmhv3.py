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
        cpu.StoreQueueMultiple = 1 # Do not support Virtual-SQ in SMT
        cpu.smtLSQMode = 'Shared'
        cpu.smtLSQPolicy = 'Dynamic'
        cpu.smtROBPolicy = 'DynamicBorrowing'
        cpu.branchPred.smtFTQMode = 'Shared'
        cpu.branchPred.smtFTQPolicy = 'Partitioned'


def setDualFrontendProbeParams(system):
    for cpu in system.cpu:
        # Upper-bound probe: predict and start fetching one FTQ target from
        # each SMT thread in the same cycle. Each target creates two I-cache
        # line requests, so four tag read ports are required to avoid
        # introducing an artificial thread bias in the cache model.
        cpu.branchPred.smtNumPredictingThreads = 2
        cpu.smtNumFetchTargetThreads = 2
        cpu.smtNumPreDispatchThreads = 2
        # Keep the dual-thread pre-dispatch probe closer to an RTL-oriented
        # 10-wide implementation: each thread can contribute up to five
        # instructions per cycle. Wider upper-bound probes can override both
        # widths from the command line.
        cpu.decodeWidth = 5
        cpu.renameWidth = 5
        cpu.icache.tag_load_read_ports = 4

        # Keep early-predictor training on the existing resolve/commit path.
        # Their per-thread ahead state is preserved while the shared tables
        # are treated as ideal dual-read resources for this upper-bound probe.
        cpu.branchPred.ubtb.usingS3Pred = False
        cpu.branchPred.abtb.enabled = True
        cpu.branchPred.abtb.usingS3Pred = False
        cpu.branchPred.microtage.enabled = True
        cpu.branchPred.microtage.usingS3Pred = False


def setTidPartitionedPredictorParams(system):
    for cpu in system.cpu:
        # RTL-oriented capacity model: large predictor tables are split into
        # two disjoint tid-owned halves. Prediction remains ideal dual-ported,
        # and resolve/commit training keeps the existing timing model.
        for predictor in (
            cpu.branchPred.abtb,
            cpu.branchPred.microtage,
            cpu.branchPred.mbtb,
            cpu.branchPred.tage,
            cpu.branchPred.ittage,
            cpu.branchPred.mgsc,
        ):
            predictor.smtTidPartitioned = True

        # The RTL implementation can afford to duplicate the small uBTB.
        # Double the physical model before splitting it so each thread keeps
        # the original 32-entry capacity.
        cpu.branchPred.ubtb.numEntries = 64
        cpu.branchPred.ubtb.smtTidPartitioned = True


if __name__ == '__m5_main__':
    FutureClass = None

    args = xiangshan_system_init()

    assert not args.external_memory_system

    args.smt = True
    if args.enable_dynamic_pf is None:
        args.enable_dynamic_pf = True
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
    setDualFrontendProbeParams(test_sys)
    setTidPartitionedPredictorParams(test_sys)

    root = Root(full_system=True, system=test_sys)

    Simulation.run_vanilla(args, root, test_sys, FutureClass)
