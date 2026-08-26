/*
 * Copyright (c) 2026 OpenXiangShan
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Bare-metal AM self-check for XS-GEM5 configurable VLEN.
 *
 * Multi-angle checks (CSR-only is NOT enough):
 *   A) CSR vlenb / vsetvli VLMAX
 *   B) vlseg2e8   — elem_gen_idx segment load split
 *   C) vsseg2e8   — elem_gen_idx segment store split
 *   D) vlse8      — elem_gen_idx strided load
 *   E) vluxei8    — elem_gen_idx indexed load
 *   F) vle8 m2    — unit-stride LMUL path (elem_num_per_vreg = vlen/eew)
 *   G) vl1re8     — whole-register load sized to architectural vlenb
 *   H) vslidedown — slide uses elem_gen_idx for vs2
 *   I) vmseq m2   — VMaskMergeMicroInst (vlen/sew elems per micro-op)
 *
 * Build:
 *   export AM_HOME=/path/to/nexus-am
 *   make ARCH=riscv64-xs LINUX_GNU_TOOLCHAIN=1
 */

#include <klib.h>
#include <stdint.h>

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

static void clear_vregs(uint64_t vlenb)
{
    asm volatile(
        "vsetvli zero, %0, e8, m1, ta, ma\n\t"
        "vmv.v.i v0, 0\n\t"
        "vmv.v.i v1, 0\n\t"
        "vmv.v.i v2, 0\n\t"
        "vmv.v.i v3, 0\n\t"
        "vmv.v.i v4, 0\n\t"
        "vmv.v.i v5, 0\n\t"
        "vmv.v.i v6, 0\n\t"
        "vmv.v.i v7, 0\n\t"
        "vmv.v.i v8, 0\n\t"
        "vmv.v.i v9, 0\n\t"
        "vmv.v.i v10, 0\n\t"
        "vmv.v.i v11, 0\n\t"
        :
        : "r"(vlenb)
        : "memory");
}

/* B) Segment load — primary regression for elem_gen_idx(..., vlen). */
static int check_vlseg(uint64_t vlenb)
{
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

    clear_vregs(vlenb);
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

    for (i = 0; i < vl_elems; i++) {
        if (out0[i] != src[2 * i] || out1[i] != src[2 * i + 1]) {
            printf("FAIL: vlseg i=%lu\n", (unsigned long)i);
            return 0;
        }
    }
    printf("rvv-vlen-check: vlseg PASS (vl=%lu)\n", (unsigned long)vl_elems);
    return 1;
}

/* C) Segment store — same split on the store side (microVs3). */
static int check_vsseg(uint64_t vlenb)
{
    const uint64_t vl_elems = vlenb < 32 ? vlenb : 32;
    uint8_t a[64];
    uint8_t b[64];
    uint8_t dst[64];
    uint64_t i;

    for (i = 0; i < 64; i++) {
        a[i] = (uint8_t)(0x20 + i);
        b[i] = (uint8_t)(0x30 + i);
        dst[i] = 0x5A;
    }

    clear_vregs(vlenb);
    asm volatile(
        "vsetvli zero, %2, e8, m1, ta, ma\n\t"
        "vle8.v v0, (%0)\n\t"
        "vle8.v v1, (%1)\n\t"
        "vsseg2e8.v v0, (%3)\n\t"
        :
        : "r"(a), "r"(b), "r"(vl_elems), "r"(dst)
        : "memory");

    for (i = 0; i < vl_elems; i++) {
        if (dst[2 * i] != a[i] || dst[2 * i + 1] != b[i]) {
            printf("FAIL: vsseg i=%lu dst=0x%02x/0x%02x expect 0x%02x/0x%02x\n",
                   (unsigned long)i, dst[2 * i], dst[2 * i + 1], a[i], b[i]);
            return 0;
        }
    }
    printf("rvv-vlen-check: vsseg PASS (vl=%lu)\n", (unsigned long)vl_elems);
    return 1;
}

/* D) Strided load. */
static int check_vlse(uint64_t vlenb)
{
    const uint64_t vl_elems = vlenb < 32 ? vlenb : 32;
    const uint64_t stride = 2;
    uint8_t src[128];
    uint8_t out[64];
    uint64_t i;

    for (i = 0; i < 128; i++)
        src[i] = (uint8_t)(0x10 + (i & 0xff));
    for (i = 0; i < 64; i++)
        out[i] = 0x5A;

    clear_vregs(vlenb);
    asm volatile(
        "vsetvli zero, %2, e8, m1, ta, ma\n\t"
        "vlse8.v v0, (%0), %1\n\t"
        :
        : "r"(src), "r"(stride), "r"(vl_elems)
        : "memory");
    asm volatile(
        "vsetvli zero, %1, e8, m1, ta, ma\n\t"
        "vse8.v v0, (%0)\n\t"
        :
        : "r"(out), "r"(vl_elems)
        : "memory");

    for (i = 0; i < vl_elems; i++) {
        if (out[i] != src[i * stride]) {
            printf("FAIL: vlse i=%lu\n", (unsigned long)i);
            return 0;
        }
    }
    printf("rvv-vlen-check: vlse PASS (vl=%lu)\n", (unsigned long)vl_elems);
    return 1;
}

/*
 * E) Indexed load — elem_gen_idx on both data and index register groups.
 * Why: CodeRabbit flagged this family; CSR/vlseg alone does not cover it.
 */
static int check_vluxei(uint64_t vlenb)
{
    const uint64_t vl_elems = vlenb < 32 ? vlenb : 32;
    uint8_t mem[128];
    uint8_t idx[64];
    uint8_t out[64];
    uint64_t i;

    for (i = 0; i < 128; i++)
        mem[i] = (uint8_t)(0x50 + i);
    /* Gather odd bytes: idx[i] = 2*i+1 */
    for (i = 0; i < vl_elems; i++)
        idx[i] = (uint8_t)(2 * i + 1);
    for (i = 0; i < 64; i++)
        out[i] = 0x5A;

    clear_vregs(vlenb);
    asm volatile(
        "vsetvli zero, %1, e8, m1, ta, ma\n\t"
        "vle8.v v2, (%0)\n\t"
        :
        : "r"(idx), "r"(vl_elems)
        : "memory");
    asm volatile(
        "vsetvli zero, %1, e8, m1, ta, ma\n\t"
        "vluxei8.v v0, (%0), v2\n\t"
        :
        : "r"(mem), "r"(vl_elems)
        : "memory");
    asm volatile(
        "vsetvli zero, %1, e8, m1, ta, ma\n\t"
        "vse8.v v0, (%0)\n\t"
        :
        : "r"(out), "r"(vl_elems)
        : "memory");

    for (i = 0; i < vl_elems; i++) {
        uint8_t expect = mem[idx[i]];
        if (out[i] != expect) {
            printf("FAIL: vluxei i=%lu got=0x%02x expect=0x%02x\n",
                   (unsigned long)i, out[i], expect);
            return 0;
        }
    }
    printf("rvv-vlen-check: vluxei PASS (vl=%lu)\n", (unsigned long)vl_elems);
    return 1;
}

/* F) Unit-stride LMUL=2. */
static int check_vle_m2(uint64_t vlenb)
{
    const uint64_t vl_elems = vlenb * 2;
    uint8_t src[128];
    uint8_t out0[64];
    uint8_t out1[64];
    uint64_t i;

    if (vl_elems > 128) {
        printf("FAIL: vle_m2 buffer too small\n");
        return 0;
    }
    for (i = 0; i < 128; i++)
        src[i] = (uint8_t)(0x40 + i);
    for (i = 0; i < 64; i++) {
        out0[i] = 0x5A;
        out1[i] = 0x5A;
    }

    clear_vregs(vlenb);
    asm volatile(
        "vsetvli zero, %1, e8, m2, ta, ma\n\t"
        "vle8.v v0, (%0)\n\t"
        :
        : "r"(src), "r"(vl_elems)
        : "memory");
    asm volatile(
        "vsetvli zero, %2, e8, m1, ta, ma\n\t"
        "vse8.v v0, (%0)\n\t"
        "vse8.v v1, (%1)\n\t"
        :
        : "r"(out0), "r"(out1), "r"(vlenb)
        : "memory");

    for (i = 0; i < vlenb; i++) {
        if (out0[i] != src[i] || out1[i] != src[vlenb + i]) {
            printf("FAIL: vle_m2 i=%lu\n", (unsigned long)i);
            return 0;
        }
    }
    printf("rvv-vlen-check: vle_m2 PASS (vl=%lu)\n", (unsigned long)vl_elems);
    return 1;
}

/* G) Whole-register load — architectural vlenb, not MaxVLEN. */
static int check_vl1re(uint64_t vlenb)
{
    uint8_t src[80];
    uint8_t out[80];
    uint64_t i;

    for (i = 0; i < 80; i++) {
        src[i] = (uint8_t)(0x80 + i);
        out[i] = 0x5A;
    }

    clear_vregs(vlenb);
    asm volatile("vl1re8.v v0, (%0)\n\t" : : "r"(src) : "memory");
    asm volatile(
        "vsetvli zero, %1, e8, m1, ta, ma\n\t"
        "vse8.v v0, (%0)\n\t"
        :
        : "r"(out), "r"(vlenb)
        : "memory");

    for (i = 0; i < vlenb; i++) {
        if (out[i] != src[i]) {
            printf("FAIL: vl1re i=%lu\n", (unsigned long)i);
            return 0;
        }
    }
    for (i = vlenb; i < 64 && i < 80; i++) {
        if (out[i] != 0x5A) {
            printf("FAIL: vl1re wrote past vlenb at %lu\n", (unsigned long)i);
            return 0;
        }
    }
    printf("rvv-vlen-check: vl1re PASS (vlenb=%lu)\n", (unsigned long)vlenb);
    return 1;
}

/*
 * H) vslidedown — constructor uses elem_gen_idx(VS2+i, off, sew/8, vlen).
 *
 * RVV vslidedown.vi offset=1 (e8,m1): for each active i < vl
 *   vd[i] = (i+1 < VLMAX) ? vs2[i+1] : 0
 *
 * Why load at VLMAX first: with vl < VLMAX, slide's last active lane
 * reads vs2[vl]. If that lane was only a load tail under ta, gem5 may
 * legally hold 0xff there — that is not a slide bug. Load the full
 * architectural register, then shrink vl for the slide so VLEN=512
 * still exercises vl < VLMAX (expect src[vl], not 0 / 0xff).
 */
static int check_vslide(uint64_t vlenb)
{
    const uint64_t vlmax = vlenb; /* e8, m1 */
    const uint64_t vl_elems = vlmax < 32 ? vlmax : 32;
    uint8_t src[64];
    uint8_t out[64];
    uint64_t i;

    for (i = 0; i < 64; i++)
        src[i] = (uint8_t)(0x70 + i);
    for (i = 0; i < 64; i++)
        out[i] = 0x5A;

    clear_vregs(vlenb);
    /* Fill vs2 over VLMAX so slide source indices are defined. */
    asm volatile(
        "vsetvli zero, %1, e8, m1, ta, ma\n\t"
        "vle8.v v2, (%0)\n\t"
        :
        : "r"(src), "r"(vlmax)
        : "memory");
    /* Slide (and store) at vl_elems — may be < VLMAX on VLEN=512. */
    asm volatile(
        "vsetvli zero, %1, e8, m1, ta, ma\n\t"
        "vslidedown.vi v0, v2, 1\n\t"
        "vse8.v v0, (%0)\n\t"
        :
        : "r"(out), "r"(vl_elems)
        : "memory");

    for (i = 0; i < vl_elems; i++) {
        /* Spec: dest[i] = (i+OFFSET < VLMAX) ? src[i+OFFSET] : 0 */
        const uint8_t expect =
            (i + 1 < vlmax) ? src[i + 1] : (uint8_t)0;
        if (out[i] != expect) {
            printf("FAIL: vslide i=%lu got=0x%02x expect=0x%02x "
                   "(vl=%lu vlmax=%lu)\n",
                   (unsigned long)i, out[i], expect,
                   (unsigned long)vl_elems, (unsigned long)vlmax);
            return 0;
        }
    }
    printf("rvv-vlen-check: vslide PASS (vl=%lu vlmax=%lu)\n",
           (unsigned long)vl_elems, (unsigned long)vlmax);
    return 1;
}

/*
 * I) Mask-producing op with LMUL=2 — forces VMaskMergeMicroInst.
 * Use e64 so elems_per_vreg = vlen/64 (SEW-aware), not vlenb.
 * Pattern: equal on even indices -> mask bits 1010...
 */
static int check_vmseq_m2(uint64_t vlenb)
{
    const uint64_t elems_per_reg = vlenb / 8; /* e64 */
    const uint64_t vl_elems = elems_per_reg * 2; /* m2 */
    uint64_t a[16];
    uint64_t b[16];
    uint8_t mask_bytes[8];
    uint64_t i;

    if (vl_elems > 16) {
        printf("FAIL: vmseq buffer too small for vlenb=%lu\n",
               (unsigned long)vlenb);
        return 0;
    }

    for (i = 0; i < vl_elems; i++) {
        a[i] = i;
        b[i] = (i & 1) ? 0xdeadull : i; /* equal only on even */
    }
    for (i = 0; i < 8; i++)
        mask_bytes[i] = 0x5A;

    clear_vregs(vlenb);
    asm volatile(
        "vsetvli zero, %2, e64, m2, ta, ma\n\t"
        "vle64.v v8, (%0)\n\t"
        "vle64.v v10, (%1)\n\t"
        "vmseq.vv v0, v8, v10\n\t"
        :
        : "r"(a), "r"(b), "r"(vl_elems)
        : "memory");
    asm volatile(
        "vsetvli zero, %1, e64, m2, ta, ma\n\t"
        "vsm.v v0, (%0)\n\t"
        :
        : "r"(mask_bytes), "r"(vl_elems)
        : "memory");

    for (i = 0; i < vl_elems; i++) {
        unsigned bit = (mask_bytes[i / 8] >> (i % 8)) & 1u;
        unsigned expect = (i & 1) ? 0u : 1u;
        if (bit != expect) {
            printf("FAIL: vmseq_m2 i=%lu bit=%u expect=%u "
                   "(mask merge / SEW span)\n",
                   (unsigned long)i, bit, expect);
            return 0;
        }
    }
    printf("rvv-vlen-check: vmseq_m2 PASS (vl=%lu e64)\n",
           (unsigned long)vl_elems);
    return 1;
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

    if (!check_vlseg(vlenb))
        ok = 0;
    if (!check_vsseg(vlenb))
        ok = 0;
    if (!check_vlse(vlenb))
        ok = 0;
    if (!check_vluxei(vlenb))
        ok = 0;
    if (!check_vle_m2(vlenb))
        ok = 0;
    if (!check_vl1re(vlenb))
        ok = 0;
    if (!check_vslide(vlenb))
        ok = 0;
    if (!check_vmseq_m2(vlenb))
        ok = 0;

    if (ok) {
        printf("rvv-vlen-check: PASS\n");
        return 0;
    }
    printf("rvv-vlen-check: FAIL\n");
    return 1;
}
