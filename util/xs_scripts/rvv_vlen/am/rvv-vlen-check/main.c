/*
 * Copyright (c) 2026 OpenXiangShan
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Bare-metal AM self-check for XS-GEM5 configurable VLEN.
 *
 * Checks (in order):
 *   1) CSR vlenb / vsetvli VLMAX scale with configured VLEN
 *   2) Functional path: vlseg2e8 + vse8 register-group mapping
 *
 * Why (2) exists: CSR-only smoke tests miss decode-time register splitting
 * bugs in elem_gen_idx() when architectural VLEN != DefaultVecLenInBits.
 * With VLEN=256 and e8, element index 16 must stay in vd (32 elems/reg);
 * the broken default-128 path would write it into vd+1.
 *
 * Build (needs nexus-am + RISC-V GCC):
 *   export AM_HOME=/path/to/nexus-am
 *   make ARCH=riscv64-xs LINUX_GNU_TOOLCHAIN=1
 *
 * Run on XS-GEM5:
 *   ./build/RISCV/gem5.opt configs/example/kmhv3.py \
 *     --raw-cpt --generic-rv-cpt=build/rvv-vlen-check-riscv64-xs.bin \
 *     --rvv-vlen=$VLEN --disable-difftest
 */

#include <klib.h>
#include <stdint.h>

/* Enable vector state (mstatus.VS = Initial). Same recipe as rvv-trigger. */
static inline void enable_vector(void)
{
    asm volatile(
        "lui a0, 0x2\n\t"
        "addiw a0, a0, 512\n\t"
        "csrs mstatus, a0\n\t"
        "csrwi vcsr, 0\n\t"
        :
        :
        : "a0", "memory");
}

static inline uint64_t read_vlenb(void)
{
    uint64_t v;
    asm volatile("csrr %0, vlenb" : "=r"(v));
    return v;
}

/* AVL=0 with rd!=x0 → set vl to VLMAX for the given vtype. */
static inline uint64_t vlmax_e8_m1(void)
{
    uint64_t vl;
    asm volatile("vsetvli %0, zero, e8, m1, ta, ma" : "=r"(vl));
    return vl;
}

static inline uint64_t vlmax_e64_m1(void)
{
    uint64_t vl;
    asm volatile("vsetvli %0, zero, e64, m1, ta, ma" : "=r"(vl));
    return vl;
}

/*
 * Segment load that exercises elem_gen_idx(vd, i, eew/8, vlen).
 *
 * Memory layout for nf=2, e8: [a0,b0,a1,b1,...]
 * After vlseg2e8.v v0, (src) with vl=vl_elems:
 *   v0[i] = a_i = src[2*i]
 *   v1[i] = b_i = src[2*i+1]
 *
 * Critical index: i = vlenb/2  (half of elems-per-reg at e8).
 * Correct VLEN: still in v0/v1 lane i.
 * Broken default-128 when VLEN=256: lands in v1/v2 lane 0.
 */
static int check_vlseg_elem_gen_idx(uint64_t vlenb)
{
    /*
     * vl = min(32, vlenb): for VLEN>=256, index 16 must remain in v0/v1.
     * A decode path that still assumes DefaultVecLenInBits=128 maps index 16
     * into v1/v2 and fails the out0[16]/out1[16] checks below.
     */
    const uint64_t vl_elems = vlenb < 32 ? vlenb : 32;
    uint8_t src[64];
    uint8_t out0[64];
    uint8_t out1[64];
    uint64_t i;

    for (i = 0; i < 64; i++)
        src[i] = (uint8_t)(0xA0 + i);
    for (i = 0; i < 64; i++) {
        out0[i] = 0x5A;
        out1[i] = 0x5A;
    }

    /* Clear destination group so stale bytes cannot masquerade as PASS. */
    asm volatile(
        "vsetvli zero, %0, e8, m1, ta, ma\n\t"
        "vmv.v.i v0, 0\n\t"
        "vmv.v.i v1, 0\n\t"
        "vmv.v.i v2, 0\n\t"
        :
        : "r"(vlenb)
        : "memory");

    asm volatile(
        "vsetvli zero, %1, e8, m1, ta, ma\n\t"
        "vlseg2e8.v v0, (%0)\n\t"
        :
        : "r"(src), "r"(vl_elems)
        : "memory");

    asm volatile(
        "vsetvli zero, %2, e8, m1, ta, ma\n\t"
        "vse8.v v0, (%0)\n\t"
        "vse8.v v1, (%1)\n\t"
        :
        : "r"(out0), "r"(out1), "r"(vl_elems)
        : "memory");

    int ok = 1;
    for (i = 0; i < vl_elems; i++) {
        uint8_t expect_a = src[2 * i];
        uint8_t expect_b = src[2 * i + 1];
        if (out0[i] != expect_a) {
            printf("FAIL: vlseg v0[%lu]=0x%02x expect 0x%02x "
                   "(elem_gen_idx / VLEN split)\n",
                   (unsigned long)i, out0[i], expect_a);
            ok = 0;
            break;
        }
        if (out1[i] != expect_b) {
            printf("FAIL: vlseg v1[%lu]=0x%02x expect 0x%02x "
                   "(elem_gen_idx / VLEN split)\n",
                   (unsigned long)i, out1[i], expect_b);
            ok = 0;
            break;
        }
    }

    if (ok) {
        if (vl_elems > 16) {
            printf("rvv-vlen-check: vlseg PASS (vl=%lu vlenb=%lu idx16 "
                   "v0=0x%02x v1=0x%02x)\n",
                   (unsigned long)vl_elems, (unsigned long)vlenb,
                   out0[16], out1[16]);
        } else {
            printf("rvv-vlen-check: vlseg PASS (vl=%lu vlenb=%lu)\n",
                   (unsigned long)vl_elems, (unsigned long)vlenb);
        }
    }
    return ok;
}

int main(void)
{
    enable_vector();

    uint64_t vlenb = read_vlenb();
    uint64_t vl_e8 = vlmax_e8_m1();
    uint64_t vl_e64 = vlmax_e64_m1();
    uint64_t vlen = vlenb * 8;

    printf("rvv-vlen-check: vlenb=%lu vlen=%lu vlmax_e8_m1=%lu vlmax_e64_m1=%lu\n",
           (unsigned long)vlenb, (unsigned long)vlen,
           (unsigned long)vl_e8, (unsigned long)vl_e64);

    int ok = 1;
    if (vlenb == 0 || (vlenb & (vlenb - 1))) {
        printf("FAIL: vlenb not positive power-of-two\n");
        ok = 0;
    }
    if (vl_e8 != vlenb) {
        printf("FAIL: vlmax(e8,m1)=%lu expected %lu\n",
               (unsigned long)vl_e8, (unsigned long)vlenb);
        ok = 0;
    }
    if (vl_e64 != vlenb / 8) {
        printf("FAIL: vlmax(e64,m1)=%lu expected %lu\n",
               (unsigned long)vl_e64, (unsigned long)(vlenb / 8));
        ok = 0;
    }

    if (!check_vlseg_elem_gen_idx(vlenb))
        ok = 0;

    if (ok) {
        printf("rvv-vlen-check: PASS\n");
        return 0;
    }
    printf("rvv-vlen-check: FAIL\n");
    return 1;
}
