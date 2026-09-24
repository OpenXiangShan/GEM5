# Copyright (c) 2026 Institute of Computing Technology, CAS
# SPDX-License-Identifier: BSD-3-Clause

"""Explicit LRU/SDBP overrides applied after Kunminghu cache defaults."""

from m5.objects import LRURP, SDBPRP


def add_sdbp_options(parser):
    for level in ("l2", "l3"):
        parser.add_argument(
            f"--{level}-replacement-policy",
            choices=("default", "lru", "sdbp"),
            default="default",
            help=f"Override {level.upper()} replacement after platform defaults",
        )
    parser.add_argument("--sdbp-sampler-num", type=int, default=32)
    parser.add_argument("--sdbp-sampler-assoc", type=int, default=12)
    parser.add_argument("--sdbp-dead-threshold", type=int, default=8)
    parser.add_argument("--sdbp-enable-bypass", action="store_true")
    parser.add_argument(
        "--sdbp-pc-hash-type",
        choices=("xor_fold", "mixed", "low_bits"),
        default="xor_fold",
    )
    parser.add_argument("--sdbp-pc-shift", type=int, default=1)


def configure_sdbp(args, system):
    def configure(cache, policy):
        if policy == "lru":
            cache.replacement_policy = LRURP()
        elif policy == "sdbp":
            cache.replacement_policy = SDBPRP(
                num_sets=int(cache.size)
                // (int(cache.assoc) * int(system.cache_line_size)),
                sampler_num=args.sdbp_sampler_num,
                sampler_assoc=args.sdbp_sampler_assoc,
                dead_threshold=args.sdbp_dead_threshold,
                enable_bypass=args.sdbp_enable_bypass,
                pc_hash_type=args.sdbp_pc_hash_type,
                pc_shift=args.sdbp_pc_shift,
            )

    if args.l2_replacement_policy != "default":
        if not args.l2cache:
            raise ValueError("--l2-replacement-policy requires --l2cache")
        if args.classic_l2:
            caches = system.l2_caches
        else:
            caches = [
                cache_slice.inner_cache
                for wrapper in system.l2_wrappers
                for cache_slice in wrapper.slices
            ]
        for cache in caches:
            configure(cache, args.l2_replacement_policy)
    if args.l3_replacement_policy != "default":
        if not args.l3cache:
            raise ValueError("--l3-replacement-policy requires --l3cache")
        configure(system.l3, args.l3_replacement_policy)
