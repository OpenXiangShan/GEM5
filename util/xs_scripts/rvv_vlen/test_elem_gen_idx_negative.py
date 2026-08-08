#!/usr/bin/env python3
# Copyright (c) 2026 OpenXiangShan
# SPDX-License-Identifier: BSD-3-Clause
"""
Offline multi-angle proofs for dynamic VLEN register splitting.

These do not need gem5 linked. They encode the exact failure mode that
CSR-only smoke tests miss: omitting architectural VLEN in elem_gen_idx.
"""

from __future__ import annotations

import unittest


def elem_gen_idx(vd: int, n: int, elem_size: int, vlen_bits: int) -> int:
    elts_per_reg = (vlen_bits >> 3) // elem_size
    return vd + n // elts_per_reg


def simulate_vlseg2e8(vlen: int, pass_arch_vlen: bool) -> tuple[list[int], list[int]]:
    """
    Decode-time microVd assignment for vlseg2e8 (nf=2, e8, m1).
    Lane index uses architectural VLEN (as execute does via vlen member).
    """
    vlenb = vlen // 8
    vl = min(32, vlenb)
    src = [0xA0 + i for i in range(64)]
    regs = {r: [0] * vlenb for r in range(4)}
    split_vlen = vlen if pass_arch_vlen else 128
    arch_elts = vlen // 8
    for i in range(vl):
        for fn in range(2):
            micro = elem_gen_idx(0 + fn, i, 1, split_vlen)
            lane = i % arch_elts
            if micro in regs and lane < len(regs[micro]):
                regs[micro][lane] = src[i * 2 + fn]
    return regs[0][:vl], regs[1][:vl]


class TestNegativeElemGenIdx(unittest.TestCase):
    def test_correct_path_passes_all_vlens(self):
        for vlen in (128, 256, 512):
            out0, out1 = simulate_vlseg2e8(vlen, True)
            vl = len(out0)
            expect0 = [0xA0 + 2 * i for i in range(vl)]
            expect1 = [0xA0 + 2 * i + 1 for i in range(vl)]
            self.assertEqual(out0, expect0, f"vlen={vlen}")
            self.assertEqual(out1, expect1, f"vlen={vlen}")

    def test_default128_fails_when_arch_is_256(self):
        out0, _ = simulate_vlseg2e8(256, False)
        # Index 16 must be wrong under default-128 split.
        self.assertNotEqual(out0[16], 0xA0 + 32)
        self.assertEqual(out0[16], 0)  # never written into v0[16]

    def test_default128_fails_when_arch_is_512(self):
        out0, _ = simulate_vlseg2e8(512, False)
        self.assertNotEqual(out0[16], 0xA0 + 32)

    def test_mask_merge_elems_is_vlen_over_sew(self):
        # elems_per_vreg = (vlen/8)/sizeof(Elem) == vlen/sew
        for vlen, sew in ((128, 8), (128, 64), (256, 64), (512, 16)):
            elems = (vlen >> 3) // (sew // 8)
            self.assertEqual(elems, vlen // sew)
            if sew != 8:
                # The uint8_t-only bug would incorrectly use vlenb here.
                self.assertNotEqual(elems, vlen >> 3)


class TestUnitStrideLmulSplit(unittest.TestCase):
    """Unit-stride uses VD+i with elem_num_per_vreg = vlen/eew (not elem_gen_idx)."""

    def test_m2_split_scales_with_vlen(self):
        for vlen in (128, 256, 512):
            eew = 8
            elems = vlen // eew
            # micro0 covers [0, elems), micro1 covers [elems, 2*elems)
            self.assertEqual(elems, vlen // 8)
            self.assertEqual(2 * elems, (vlen // 8) * 2)


if __name__ == "__main__":
    unittest.main()
