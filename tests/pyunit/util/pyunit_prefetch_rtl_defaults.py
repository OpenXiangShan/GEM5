# Copyright (c) 2026 Institute of Computing Technology, CAS
# SPDX-License-Identifier: BSD-3-Clause

import contextlib
import io
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest

from m5.objects import L2CompositeWithWorkerPrefetcher, XSCompositePrefetcher
from m5.objects.Request import PrefetchSourceType
from m5.params import NULL

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "configs"))
from common import PrefetcherConfig as config
from common import CacheConfig as caches


class RtlDefaultDataPrefetchTest(unittest.TestCase):
    def options(self, rtl=True, kmh=False):
        # Deliberately request legacy extras: the explicit RTL profile wins,
        # including over idealkmhv3's unconditional LLDP defaults.
        return SimpleNamespace(
            rtl_default_data_prefetchers=rtl,
            kmh_align=kmh,
            l1_to_l2_pf_hint=True,
            ideal_cache=False,
            pht_pf_level=1,
            l1d_enable_spp=True,
            l1d_enable_cplx=True,
            l1d_enable_lldp=True,
            l2_enable_lldp=True,
            _lldp_training_cpu=NULL,
        )

    def test_coexisting_generators_and_targets_override_legacy_extras(self):
        for kmh in (False, True):
            with self.subTest(kmh=kmh):
                pf = XSCompositePrefetcher()
                config._configure_xs_composite(pf, self.options(kmh=kmh), True)
                for key in ("enable_activepage", "enable_xsstream",
                            "enable_sstride", "enable_pht", "rtl_sms_agt"):
                    self.assertTrue(bool(getattr(pf, key)), key)
                for key in ("enable_berti", "enable_temporal", "enable_bop",
                            "enable_spp", "enable_cplx", "enable_opt",
                            "enable_lldp", "stream_pf_ahead"):
                    self.assertFalse(bool(getattr(pf, key)), key)
                self.assertEqual(int(pf.pht_pf_level), 2)
                self.assertFalse(bool(pf.xsstream.enable_l3_stream_pre))
                self.assertTrue(bool(pf.sstride.use_xs_depth))
                self.assertFalse(bool(pf.sstride.use_redundant_table))
                self.assertEqual(int(pf.sms_agt_stride), 30)
                self.assertEqual(int(pf.sms_agt_threshold), 12)

    def test_both_bop_generators_and_no_cdp_lldp(self):
        for kmh in (False, True):
            with self.subTest(kmh=kmh):
                pf = L2CompositeWithWorkerPrefetcher()
                config._configure_l2_composite(
                    pf, "L2CompositeWithWorkerPrefetcher",
                    self.options(kmh=kmh))
                self.assertTrue(bool(pf.enable_bop))
                for key in ("enable_cdp", "enable_cmc", "enable_lldp",
                            "enable_despacito_stream"):
                    self.assertFalse(bool(getattr(pf, key)), key)
                self.assertTrue(bool(pf.bop_large.use_virtual_addresses))
                self.assertFalse(bool(pf.bop_small.use_virtual_addresses))
                self.assertTrue(
                    bool(pf.bop_small.train_on_physical_addresses))
                self.assertFalse(bool(pf.bop_large.enable_adaptoffset))
                self.assertFalse(bool(pf.bop_small.enable_adaptoffset))

    def test_existing_default_profile_is_preserved(self):
        pf = XSCompositePrefetcher()
        opts = self.options(rtl=False)
        opts.l1d_enable_spp = opts.l1d_enable_cplx = False
        opts.l1d_enable_lldp = False
        config._configure_xs_composite(pf, opts, True)
        self.assertFalse(bool(pf.rtl_sms_agt))
        self.assertTrue(bool(pf.enable_activepage))
        self.assertTrue(bool(pf.enable_berti))
        self.assertTrue(bool(pf.enable_temporal))
        self.assertFalse(bool(pf.enable_xsstream))
        self.assertFalse(bool(pf.enable_sstride))

    def test_incompatible_topology_is_rejected(self):
        opts = self.options()
        opts.classic_l2 = False
        opts.l1d_hwp_type = "MultiPrefetcher"
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                config.create_prefetcher(NULL, "l1d", opts)

    def test_backpressure_preserves_target_level(self):
        for rtl in (False, True):
            for level in ("l1d", "l2"):
                with self.subTest(rtl=rtl, level=level):
                    opts = self.options(rtl=rtl)
                    params = caches._get_cache_opts(NULL, level, opts)
                    if rtl:
                        self.assertFalse(params["prefetch_can_offload"])
                    else:
                        self.assertNotIn("prefetch_can_offload", params)

    def test_profile_rejects_disabled_prefetch_or_cache_levels(self):
        for no_pf, l1, l2 in ((True, True, True), (False, False, True),
                              (False, True, False)):
            opts = self.options()
            opts.no_pf, opts.caches, opts.l2cache = no_pf, l1, l2
            with contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    caches.config_cache(opts, None)

    def test_source_layout_and_old_control_vectors(self):
        self.assertEqual(config.PF_SOURCE_NAMES, PrefetchSourceType.vals)
        self.assertEqual(config.PF_SOURCE_NAMES[1], "SStream")
        self.assertEqual(config.PF_SOURCE_NAMES[16], "SMS_AGT")
        table = config._parse_source_pct_table([100] * 16, "test")
        self.assertEqual(table, [100] * 16 + [-1])
        table = config._parse_source_pct_table(
            {"SMS_AGT": 25, "stream": 50}, "test")
        self.assertEqual(table[16], 25)
        self.assertEqual(table[1], 50)


if __name__ in ("__main__", "__m5_main__"):
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(
        RtlDefaultDataPrefetchTest)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    sys.exit(0 if result.wasSuccessful() else 1)
