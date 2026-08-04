/*
 * Copyright (c) 2026 OpenXiangShan
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Bare-metal AM self-check for XS-GEM5 configurable VLEN.
 *
 * Opens mstatus.VS, then reads:
 *   - CSR vlenb  (expect VLEN/8)
 *   - VLMAX via vsetvli x0,e8,m1  (expect VLEN/8)
 *   - VLMAX via vsetvli x0,e64,m1 (expect VLEN/64)
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

    if (ok) {
        printf("rvv-vlen-check: PASS\n");
        return 0;
    }
    printf("rvv-vlen-check: FAIL\n");
    return 1;
}
