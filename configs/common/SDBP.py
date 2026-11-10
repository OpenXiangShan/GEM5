# Copyright (c) 2026 Institute of Computing Technology, CAS
# SPDX-License-Identifier: BSD-3-Clause

"""Explicit L2 LRU/SDBP overrides applied after Kunminghu cache defaults."""

import argparse

from m5.objects import LRURP, SDBPRP


def nonnegative_int(value):
    number = int(value)
    if number < 0:
        raise argparse.ArgumentTypeError("must be nonnegative")
    return number


def add_sdbp_options(parser):
    parser.add_argument(
        "--l2-replacement-policy",
        choices=("default", "lru", "sdbp"),
        default="default",
        help="Override L2 replacement after platform defaults",
    )
    parser.add_argument("--sdbp-sampler-num", type=int, default=128)
    parser.add_argument("--sdbp-sampler-assoc", type=int, default=8)
    parser.add_argument(
        "--sdbp-l1-copy-protection",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="Skip L1-resident blocks during L2 dead-victim selection",
    )
    parser.add_argument(
        "--sdbp-sampler-dead-first",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="Prefer dead sampler entries; disable for pure LRU sampling",
    )
    parser.add_argument(
        "--sdbp-sampler-demand-pc-only",
        action=argparse.BooleanOptionalAction,
        default=False,
        help="Only allocate and train the sampler for L2 demand requests with a PC",
    )
    parser.add_argument(
        "--sdbp-sampler-protect-youngest",
        type=nonnegative_int,
        default=0,
        metavar="N",
        help="Skip the N youngest sampler entries during dead-first selection",
    )
    parser.add_argument("--sdbp-dead-threshold", type=int, default=8)
    parser.add_argument(
        "--sdbp-enable-bypass",
        action=argparse.BooleanOptionalAction,
        default=False,
        help="Bypass dead L2 fills and return their L1 victims to L3 (default: off)",
    )
    parser.add_argument(
        "--sdbp-pc-hash-type",
        choices=("xor_fold", "mixed", "low_bits"),
        default="xor_fold",
    )
    parser.add_argument("--sdbp-pc-shift", type=int, default=1)


def configure_sdbp(args, system):
    def configure(cache, policy, cpu):
        if policy == "lru":
            cache.replacement_policy = LRURP()
        elif policy == "sdbp":
            # Query only the owning core's resident L1I/L1D tags.
            cache.sdbp_l1_caches = (
                [cpu.icache, cpu.dcache]
                if args.sdbp_l1_copy_protection
                else []
            )
            cache.replacement_policy = SDBPRP(
                num_sets=int(cache.size)
                // (int(cache.assoc) * int(system.cache_line_size)),
                sampler_num=args.sdbp_sampler_num,
                sampler_assoc=args.sdbp_sampler_assoc,
                l1_copy_protection=args.sdbp_l1_copy_protection,
                sampler_dead_first=args.sdbp_sampler_dead_first,
                sampler_demand_pc_only=args.sdbp_sampler_demand_pc_only,
                sampler_protect_youngest=args.sdbp_sampler_protect_youngest,
                dead_threshold=args.sdbp_dead_threshold,
                enable_bypass=args.sdbp_enable_bypass,
                pc_hash_type=args.sdbp_pc_hash_type,
                pc_shift=args.sdbp_pc_shift,
            )

    if args.l2_replacement_policy != "default":
        if not args.l2cache:
            raise ValueError("--l2-replacement-policy requires --l2cache")
        if (
            args.l2_replacement_policy == "sdbp"
            and args.sdbp_enable_bypass
            and (not args.l3cache or args.no_l3cache)
        ):
            raise ValueError("SDBP L2 bypass requires --l3cache")
        if (
            not args.caches
            and args.l2_replacement_policy == "sdbp"
            and args.sdbp_l1_copy_protection
        ):
            raise ValueError("SDBP L1-copy protection requires --caches")
        if args.classic_l2:
            for cpu, cache in zip(system.cpu, system.l2_caches):
                configure(cache, args.l2_replacement_policy, cpu)
        else:
            for cpu, wrapper in zip(system.cpu, system.l2_wrappers):
                for cache_slice in wrapper.slices:
                    configure(
                        cache_slice.inner_cache,
                        args.l2_replacement_policy,
                        cpu,
                    )
