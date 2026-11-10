# Copyright (c) 2026 Institute of Computing Technology, CAS
# SPDX-License-Identifier: BSD-3-Clause

"""Select cache hierarchy behavior independently of replacement/prediction."""


def add_cache_hierarchy_options(parser):
    parser.add_argument(
        "--cache-hierarchy-policy",
        choices=("legacy", "xiangshan_single_core"),
        default="legacy",
        help=(
            "XiangShan single-core alignment: ordinary writable read grants, "
            "dirty-only L1 data eviction and L2-bypass victim routing"
        ),
    )


def configure_cache_hierarchy(args, system):
    mode = args.cache_hierarchy_policy
    if mode == "legacy":
        return
    if args.num_cpus != 1 or getattr(args, "smt", False):
        raise ValueError("xiangshan_single_core requires one CPU without SMT")
    if getattr(args, "ruby", False) or args.external_memory_system:
        raise ValueError("xiangshan_single_core requires classic caches")
    if not (args.caches and args.l2cache and args.l3cache):
        raise ValueError("xiangshan_single_core requires L1, L2 and L3 caches")
    cpu = system.cpu[0]
    caches = [cpu.icache, cpu.dcache, system.l3]
    for name in ("itb_walker_cache", "dtb_walker_cache"):
        cache = getattr(cpu, name, None)
        if cache is not None:
            caches.append(cache)
    if args.classic_l2:
        caches.extend(system.l2_caches)
    else:
        caches.extend(
            cache_slice.inner_cache
            for wrapper in system.l2_wrappers
            for cache_slice in wrapper.slices
        )
    for cache in caches:
        cache.cache_hierarchy_policy = mode
