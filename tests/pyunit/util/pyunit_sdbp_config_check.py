# Copyright (c) 2026 Institute of Computing Technology, CAS
# SPDX-License-Identifier: BSD-3-Clause

import argparse
import contextlib
import importlib.util
import io
import itertools
from pathlib import Path
from types import SimpleNamespace
import sys
import unittest

from m5.objects import Cache


MODULE_PATH = Path(__file__).resolve().parents[3] / "configs/common/SDBP.py"
SPEC = importlib.util.spec_from_file_location("sdbp_config", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class SDBPConfigTest(unittest.TestCase):
    def parser(self):
        parser = argparse.ArgumentParser()
        MODULE.add_sdbp_options(parser)
        return parser

    def test_defaults_and_invalid_guard(self):
        args = self.parser().parse_args([])
        self.assertTrue(args.sdbp_l1_copy_protection)
        self.assertTrue(args.sdbp_sampler_dead_first)
        self.assertFalse(args.sdbp_sampler_demand_pc_only)
        args = self.parser().parse_args(
            [
                "--sdbp-sampler-demand-pc-only",
                "--no-sdbp-sampler-demand-pc-only",
            ]
        )
        self.assertFalse(args.sdbp_sampler_demand_pc_only)
        self.assertEqual(args.sdbp_sampler_protect_youngest, 0)
        self.assertFalse(args.sdbp_enable_bypass)
        with contextlib.redirect_stderr(io.StringIO()):
            for value in ("-1", "1.5"):
                with self.assertRaises(SystemExit):
                    self.parser().parse_args(
                        [f"--sdbp-sampler-protect-youngest={value}"]
                    )

    def test_options_reach_all_cores_and_slices(self):
        for classic, l1, dead, bypass, demand_pc_only in itertools.product(
            (False, True), repeat=5
        ):
            for guard in (0, 1, 2, 3, 8):
                with self.subTest(
                    classic=classic,
                    l1=l1,
                    dead=dead,
                    bypass=bypass,
                    demand_pc_only=demand_pc_only,
                    guard=guard,
                ):
                    flags = [
                        "--l2-replacement-policy=sdbp",
                        f"--sdbp-sampler-protect-youngest={guard}",
                    ]
                    if not l1:
                        flags.append("--no-sdbp-l1-copy-protection")
                    if not dead:
                        flags.append("--no-sdbp-sampler-dead-first")
                    if bypass:
                        flags.append("--sdbp-enable-bypass")
                    if demand_pc_only:
                        flags.append("--sdbp-sampler-demand-pc-only")
                    args = self.parser().parse_args(flags)
                    args.l2cache = args.caches = True
                    args.classic_l2 = classic
                    cpus = [
                        SimpleNamespace(icache=Cache(), dcache=Cache())
                        for _ in range(2)
                    ]
                    groups = [
                        [
                            Cache(size="256KiB", assoc=8)
                            for _ in range(1 if classic else 4)
                        ]
                        for _ in cpus
                    ]
                    system = SimpleNamespace(
                        cpu=cpus,
                        cache_line_size=64,
                        l2_caches=[group[0] for group in groups],
                        l2_wrappers=[
                            SimpleNamespace(
                                slices=[
                                    SimpleNamespace(inner_cache=cache)
                                    for cache in group
                                ]
                            )
                            for group in groups
                        ],
                    )
                    MODULE.configure_sdbp(args, system)
                    for cpu, group in zip(cpus, groups):
                        for cache in group:
                            policy = cache.replacement_policy
                            self.assertEqual(policy.type, "SDBPRP")
                            self.assertEqual(int(policy.num_sets), 512)
                            self.assertEqual(
                                bool(policy.l1_copy_protection), l1
                            )
                            self.assertEqual(
                                bool(policy.sampler_dead_first), dead
                            )
                            self.assertEqual(
                                bool(policy.sampler_demand_pc_only),
                                demand_pc_only,
                            )
                            self.assertEqual(
                                int(policy.sampler_protect_youngest), guard
                            )
                            self.assertEqual(
                                bool(policy.enable_bypass), bypass
                            )
                            self.assertEqual(
                                list(cache.sdbp_l1_caches),
                                [cpu.icache, cpu.dcache] if l1 else [],
                            )


if __name__ in ("__main__", "__m5_main__"):
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(SDBPConfigTest)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    sys.exit(0 if result.wasSuccessful() else 1)
