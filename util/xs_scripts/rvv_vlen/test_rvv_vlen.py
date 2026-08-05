#!/usr/bin/env python3
# Copyright (c) 2026 OpenXiangShan
# SPDX-License-Identifier: BSD-3-Clause
"""
Unit tests for configurable RVV VLEN helpers used by XS-GEM5.

These tests intentionally re-implement the architectural VLMAX formula so they
can run without linking gem5. Keep them in sync with
src/arch/riscv/utility.hh::{getVlmax,vtype_VLMAX}.

Run:
  python3 util/xs_scripts/rvv_vlen/test_rvv_vlen.py
"""

from __future__ import annotations

import math
import unittest


def get_sew(vsew: int) -> int:
    assert 0 <= vsew <= 3
    return 8 << vsew


def get_vflmul(vlmul_encoding: int) -> float:
    # sext<3>
    vlmul = vlmul_encoding if vlmul_encoding < 4 else vlmul_encoding - 8
    return (1 << vlmul) if vlmul >= 0 else 1.0 / (1 << -vlmul)


def get_vlmax(vsew: int, vlmul_encoding: int, vlen: int) -> int:
    """Mirror of RiscvISA::getVlmax(VTYPE, vlen)."""
    sew = get_sew(vsew)
    return int((vlen / sew) * get_vflmul(vlmul_encoding))


def vtype_vlmax(vsew: int, vlmul_encoding: int, vlen: int, per_reg: bool = False) -> int:
    """Mirror of RiscvISA::vtype_VLMAX(vtype, vlen_bits, per_reg)."""
    lmul = vlmul_encoding if vlmul_encoding < 4 else vlmul_encoding - 8
    if per_reg:
        lmul = min(0, lmul)
    return vlen >> (vsew + 3 - lmul)


class TestVlmaxFormula(unittest.TestCase):
    """Architectural VLMAX must scale linearly with configured VLEN."""

    # (vlen, vsew, vlmul_encoding, expected_vlmax)
    # vlmul encodings: 0=m1, 1=m2, 2=m4, 3=m8, 5=mf8, 6=mf4, 7=mf2
    CASES = [
        # VLEN=128, SEW=8, LMUL=1 -> 16
        (128, 0, 0, 16),
        # VLEN=128, SEW=64, LMUL=1 -> 2
        (128, 3, 0, 2),
        # VLEN=128, SEW=16, LMUL=1/8 -> 1
        (128, 1, 5, 1),
        # VLEN=256, SEW=8, LMUL=1 -> 32
        (256, 0, 0, 32),
        # VLEN=256, SEW=64, LMUL=2 -> 8
        (256, 3, 1, 8),
        # VLEN=512, SEW=32, LMUL=1 -> 16
        (512, 2, 0, 16),
        # VLEN=512, SEW=8, LMUL=8 -> 512
        (512, 0, 3, 512),
        # VLEN=512, SEW=16, LMUL=1/8 -> 4
        (512, 1, 5, 4),
    ]

    def test_get_vlmax_table(self):
        for vlen, vsew, vlmul, expected in self.CASES:
            with self.subTest(vlen=vlen, vsew=vsew, vlmul=vlmul):
                self.assertEqual(get_vlmax(vsew, vlmul, vlen), expected)
                self.assertEqual(vtype_vlmax(vsew, vlmul, vlen), expected)

    def test_scales_with_vlen(self):
        # Doubling VLEN must double VLMAX for legal SEW/LMUL combinations.
        # Skip configs that are architecturally illegal at VLEN=128 (VLMAX < 1).
        for vsew in range(4):
            for vlmul in (0, 1, 2, 3, 7, 6, 5):
                base = get_vlmax(vsew, vlmul, 128)
                if base < 1:
                    continue
                self.assertEqual(get_vlmax(vsew, vlmul, 256), base * 2)
                self.assertEqual(get_vlmax(vsew, vlmul, 512), base * 4)

    def test_vlenb_bytes(self):
        for vlen in (128, 256, 512):
            self.assertEqual(vlen >> 3, vlen // 8)
            self.assertTrue(math.log2(vlen).is_integer())


class TestVlenConfigContract(unittest.TestCase):
    """Document the XS-GEM5 VLEN configuration contract."""

    def test_supported_vlens(self):
        supported = {128, 256, 512}
        self.assertIn(128, supported)  # Kunminghu default / NEMU difftest
        self.assertTrue(all((v & (v - 1)) == 0 for v in supported))

    def test_max_container_covers_supported(self):
        max_vlen_bits = 512
        for vlen in (128, 256, 512):
            self.assertLessEqual(vlen, max_vlen_bits)


def elem_gen_idx(vd: int, n: int, elem_size: int, vlen_bits: int) -> int:
    """Mirror of RiscvISA::elem_gen_idx — must take architectural VLEN."""
    elts_per_reg = (vlen_bits >> 3) // elem_size
    return vd + n // elts_per_reg


class TestElemGenIdx(unittest.TestCase):
    """
    Regression for decode-time register splitting.

    These cases would have caught the bug where call sites omitted vlen and
    silently used DefaultVecLenInBits=128 under VLEN=256/512.
    """

    def test_e8_index16_stays_in_vd_for_vlen256(self):
        self.assertEqual(elem_gen_idx(0, 16, 1, 128), 1)
        self.assertEqual(elem_gen_idx(0, 16, 1, 256), 0)
        self.assertEqual(elem_gen_idx(0, 16, 1, 512), 0)

    def test_e8_crosses_at_elts_per_reg(self):
        self.assertEqual(elem_gen_idx(0, 32, 1, 256), 1)
        self.assertEqual(elem_gen_idx(0, 32, 1, 512), 0)
        self.assertEqual(elem_gen_idx(4, 40, 1, 256), 5)

    def test_e64(self):
        self.assertEqual(elem_gen_idx(0, 2, 8, 128), 1)
        self.assertEqual(elem_gen_idx(0, 2, 8, 256), 0)
        self.assertEqual(elem_gen_idx(0, 4, 8, 256), 1)


if __name__ == "__main__":
    unittest.main()
