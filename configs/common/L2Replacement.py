# Copyright (c) 2026 XiangShan
# SPDX-License-Identifier: BSD-3-Clause

from m5.objects import KPCReplState, KPCRP, LRURP, XSDRRIPRP
from m5.util import fatal


def configure_l2_replacement(args, system):
    """Apply the selected policy after the KMHV3 cache geometry is final."""
    choice = args.l2_replacement
    if choice == "default":
        return
    if not args.l2cache:
        fatal("--l2-replacement requires an L2 cache")
    line_size = int(system.cache_line_size)
    for core in range(args.num_cpus):
        if args.classic_l2:
            caches = [system.l2_caches[core]]
        else:
            caches = [s.inner_cache for s in system.l2_wrappers[core].slices]
        ways = int(caches[0].assoc)
        sets = int(caches[0].size) // (ways * line_size)
        if choice == "kpc":
            state = KPCReplState(
                num_sets=sets * len(caches),
                num_ways=ways,
                block_size=line_size,
                sampled_sets=args.kpc_sampled_sets,
                hysteresis_bits=args.kpc_hysteresis_bits,
            )
            # Parent the shared state before any policy can adopt it.
            setattr(system, f"kpc_repl_state{core}", state)
        for index, cache in enumerate(caches):
            if (int(cache.assoc) != ways or
                    int(cache.size) != sets * ways * line_size):
                fatal("L2 replacement comparison requires equal slice geometry")
            if choice == "lru":
                policy = LRURP()
            elif choice == "xs-drrip":
                policy = XSDRRIPRP(mode=2, num_sets=sets, num_ways=ways)
            else:
                policy = KPCRP(
                    state=state,
                    num_slices=len(caches),
                    slice_index=index,
                    promote_prefetch_hits=args.kpc_promote_prefetch_hits,
                )
            cache.replacement_policy = policy
