"""Run the KMHV2 full-system preset with the Matrix backend enabled."""

import sys

from m5.objects import Root, XSDRRIPRP
from m5.util import addToPath

addToPath('../')
addToPath('../../')

from common import Simulation
from common.xiangshan import build_xiangshan_system, xiangshan_system_init
from util.solver.runtime.integration import maybe_handle_solver_runtime


if __name__ == '__m5_main__':
    args = xiangshan_system_init()
    args.matrix = True
    args.bp_type = 'DecoupledBPUWithBTB'
    args.l2_wrapper_hwp_type = 'L2CompositeWithWorkerPrefetcher'
    args.kmh_align = True
    assert not args.external_memory_system

    test_sys = build_xiangshan_system(args)
    l2_caches = (list(test_sys.l2_caches) if args.classic_l2 else
                 [cache_slice.inner_cache
                  for wrapper in test_sys.l2_wrappers
                  for cache_slice in wrapper.slices])
    # Retention filters victim candidates; use one compatible policy for
    # all RMW modes so policy selection does not affect the comparison.
    for cache in l2_caches:
        num_sets = int(cache.size) // (
            int(cache.assoc) * int(test_sys.cache_line_size))
        cache.replacement_policy = XSDRRIPRP(mode=2, num_sets=num_sets)

    root = Root(full_system=True, system=test_sys)
    if maybe_handle_solver_runtime(root, args):
        sys.exit(0)

    Simulation.run_vanilla(args, root, test_sys, None)
