"""Kunminghu v3 with the first-version Hybrid ROB compression model."""

import os
import sys

from m5.objects import NULL, Root
from m5.util import addToPath, fatal

addToPath('../')
addToPath('../../')

from common import Simulation
from common.xiangshan import (
    build_xiangshan_system,
    configure_xiangshan_linux_workload,
    xiangshan_system_init,
)
from kmhv3 import setKmhV3Params
from util.solver.runtime.integration import maybe_handle_solver_runtime


def setKmhV3HybridParams(args, system):
    setKmhV3Params(args, system)
    if args.smt:
        fatal("Hybrid ROB compression requires numThreads=1")

    for cpu in system.cpu:
        cpu.numThreads = 1
        cpu.valuePred = NULL
        cpu.enable_loadFusion = False
        cpu.enableConstantFolding = False
        cpu.enableMoveElimination = False
        cpu.enableMovImmElimination = False
        # Existing non-load fusion remains enabled by the decode model.
        cpu.RobCompressPolicy = 'hybrid'
        cpu.CROB_instPerGroup = 8
        cpu.renameWidth = 8
        cpu.commitWidth = 8
        cpu.commitInstWidth = 16


if __name__ == '__m5_main__':
    args = xiangshan_system_init()
    assert not args.external_memory_system
    if args.smt:
        fatal("Hybrid ROB compression requires numThreads=1")

    # Keep the surrounding system identical to the standard KMHV3 entry.
    args.bp_type = 'DecoupledBPUWithBTB'
    args.l2_size = '2MB'
    args.l3_size = '32MB'
    args.kmh_align = True
    args.cdp_use_dynamic_degree = False
    args.cdp_accuracy_threshold = 0.05
    args.cdp_use_accuracy_dependent_alignment = False
    args.cdp_use_sv48 = True

    Simulation.setMemClass(args)
    test_sys = build_xiangshan_system(args)
    if (args.raw_cpt and args.generic_rv_cpt and
            os.path.basename(args.generic_rv_cpt) == 'linux.bin'):
        configure_xiangshan_linux_workload(test_sys, args)
    setKmhV3HybridParams(args, test_sys)

    root = Root(full_system=True, system=test_sys)
    if maybe_handle_solver_runtime(root, args):
        sys.exit(0)

    Simulation.run_vanilla(args, root, test_sys, None)
