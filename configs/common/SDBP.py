# Copyright (c) 2026 Institute of Computing Technology, CAS
# SPDX-License-Identifier: BSD-3-Clause

"""Explicit L2 LRU/SDBP overrides applied after Kunminghu cache defaults."""

import argparse

from m5.objects import LRURP, SDBPRP


def add_sdbp_options(parser):
    parser.add_argument(
        "--l2-replacement-policy",
        choices=("default", "lru", "sdbp"),
        default="default",
        help="Override L2 replacement after platform defaults",
    )
    parser.add_argument("--sdbp-sampler-num", type=int, default=128)
    parser.add_argument("--sdbp-sampler-assoc", type=int, default=6)
    parser.add_argument(
        "--sdbp-sampler-protect-youngest-two",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="Protect sampler MRU and second MRU during dead selection (default: on)",
    )
    parser.add_argument("--sdbp-dead-threshold", type=int, default=8)
    parser.add_argument(
        "--sdbp-enable-bypass",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="Bypass dead L2 fills and return their L1 victims to L3 (default: on)",
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
            # L1 must expect ownership before L2's refill-time prediction.
            cpu.dcache.sdbp_bypass_writable = args.sdbp_enable_bypass
            # Query only the owning core's resident L1I/L1D tags.
            cache.sdbp_l1_caches = [cpu.icache, cpu.dcache]
            cache.replacement_policy = SDBPRP(
                num_sets=int(cache.size)
                // (int(cache.assoc) * int(system.cache_line_size)),
                sampler_num=args.sdbp_sampler_num,
                sampler_assoc=args.sdbp_sampler_assoc,
                sampler_protect_youngest_two=args.sdbp_sampler_protect_youngest_two,
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
        if not args.caches and args.l2_replacement_policy == "sdbp":
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
