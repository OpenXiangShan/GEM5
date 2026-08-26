#!/usr/bin/env python3
# Copyright (c) 2026 OpenXiangShan
# SPDX-License-Identifier: BSD-3-Clause
"""
Config-layer tests for XS-GEM5 RVV VLEN parameters.

Covers the same contract as upstream RiscvISA.py validators, plus XS-specific
limits (128/256/512). Also imports the adapted upstream resource matrix.

Run:
  python3 util/xs_scripts/rvv_vlen/test_rvv_vlen.py
  python3 util/xs_scripts/rvv_vlen/test_rvv_vlen_config.py
  python3 util/xs_scripts/rvv_vlen/test_matrix.py
  python3 configs/example/xiangshan_rvv_vlen_smoke.py --standalone --rvv-vlen=256
"""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent
while ROOT != ROOT.parent and not (ROOT / "SConstruct").exists():
    ROOT = ROOT.parent
assert (ROOT / "SConstruct").exists(), f"Cannot locate GEM5 root from {__file__}"
sys.path.insert(0, str(ROOT / "util" / "xs_scripts" / "rvv_vlen"))
sys.path.insert(0, str(ROOT / "configs" / "example"))
sys.path.insert(0, str(ROOT / "tests" / "gem5" / "rvv_vlen"))


def _load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(mod)
    return mod


class TestRiscvIsaParamContract(unittest.TestCase):
    def test_power_of_two_and_range(self):
        smoke = _load(
            "xiangshan_rvv_vlen_smoke",
            ROOT / "configs/example/xiangshan_rvv_vlen_smoke.py",
        )
        for vlen in (128, 256, 512):
            smoke.validate_params(vlen, 64)
        with self.assertRaises(SystemExit):
            smoke.validate_params(192, 64)  # not power-of-two / unsupported
        with self.assertRaises(SystemExit):
            smoke.validate_params(128, 128)  # ELEN too large for XS choices
        with self.assertRaises(SystemExit):
            smoke.validate_params(128, 256)  # VLEN < ELEN

    def test_standalone_smoke_exit_zero(self):
        smoke = _load(
            "xiangshan_rvv_vlen_smoke",
            ROOT / "configs/example/xiangshan_rvv_vlen_smoke.py",
        )
        self.assertEqual(smoke.standalone_check(128, 64), 0)
        self.assertEqual(smoke.standalone_check(256, 64), 0)
        self.assertEqual(smoke.standalone_check(512, 64), 0)


class TestUpstreamMatrixPort(unittest.TestCase):
    def test_matrix_size_and_resources(self):
        matrix_mod = _load(
            "test_matrix",
            ROOT / "util/xs_scripts/rvv_vlen/test_matrix.py",
        )
        rows = matrix_mod.build_matrix()
        self.assertEqual(len(matrix_mod.UPSTREAM_RESOURCES), 12)
        self.assertEqual(matrix_mod.XS_VLENS, [128, 256, 512])
        self.assertEqual(len(rows), 12 * 3)
        self.assertTrue(
            all(r["vlen"] in (128, 256, 512) for r in rows)
        )
        # Upstream SE scanned up to 16384; XS must not include those.
        self.assertFalse(any(r["vlen"] > 512 for r in rows))

    def test_pass_regex_mentions_resource(self):
        matrix_mod = _load(
            "test_matrix",
            ROOT / "util/xs_scripts/rvv_vlen/test_matrix.py",
        )
        for resource in matrix_mod.UPSTREAM_RESOURCES:
            pat = matrix_mod.expected_pass_pattern(resource)
            # re.escape turns '-' into '\-'; accept either form.
            self.assertTrue(
                resource in pat or resource.replace("-", r"\-") in pat,
                msg=f"{resource} not found in {pat}",
            )


if __name__ == "__main__":
    unittest.main()
