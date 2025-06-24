#include "cpu/difftest.hh"

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>

#include "arch/riscv/regs/misc.hh"
#include "base/trace.hh"
#include "cpu/nemu_common.hh"
#include "debug/ValueCommit.hh"

const char *reg_name[] = {
    "$0",     "ra",    "sp",       "gp",       "tp",      "t0",
    "t1",     "t2",    "s0",       "s1",       "a0",      "a1",
    "a2",     "a3",    "a4",       "a5",       "a6",      "a7",
    "s2",     "s3",    "s4",       "s5",       "s6",      "s7",
    "s8",     "s9",    "s10",      "s11",      "t3",      "t4",
    "t5",     "t6",
    "ft0",    "ft1",   "ft2",      "ft3",      "ft4",     "ft5",
    "ft6",    "ft7",   "fs0",      "fs1",      "fa0",     "fa1",
    "fa2",    "fa3",   "fa4",      "fa5",      "fa6",     "fa7",
    "fs2",    "fs3",   "fs4",      "fs5",      "fs6",     "fs7",
    "fs8",    "fs9",   "fs10",     "fs11",     "ft8",     "ft9",
    "ft10",   "ft11",
    "mode",   "mstatus","sstatus",  "mepc",    "sepc",    "mtval",
    "stval",  "mtvec",   "stvec",   "mcause",  "scause",  "satp",
    "mip",    "mie",     "mscratch","sscratch","mideleg", "medeleg",
    "this_pc",
    "v0",     "v1",    "v2",       "v3",       "v4",      "v5",
    "v6",     "v7",    "v8",       "v9",       "v10",     "v11",
    "v12",    "v13",   "v14",      "v15",      "v16",     "v17",
    "v18",    "v19",   "v20",      "v21",      "v22",     "v23",
    "v24",    "v25",   "v26",      "v27",      "v28",     "v29",
    "v30",    "v31"
    };

std::vector<uint64_t> skipCSRs;

// CSR op Encoding:
//     #12
// | csrName | 0000 | 0000 | 0000 | 0111 | 0011 |
#define GetCSROPInstCode(csrName) (((uint64_t)csrName << 20) | 0x73UL)

void
skipPerfCntCsr()
{
    skipCSRs.push_back(GetCSROPInstCode(gem5::RiscvISA::CSR_MCYCLE));
    skipCSRs.push_back(GetCSROPInstCode(gem5::RiscvISA::CSR_MINSTRET));

    for (uint64_t counter = gem5::RiscvISA::CSR_MMHPMCOUNTER3;
                  counter <= gem5::RiscvISA::CSR_MMHPMCOUNTER31; counter++) {
        skipCSRs.push_back(GetCSROPInstCode(counter));
    }
    for (uint64_t counter = gem5::RiscvISA::CSR_MHPMCOUNTER03;
                  counter <= gem5::RiscvISA::CSR_MHPMCOUNTER31; counter++) {
        skipCSRs.push_back(GetCSROPInstCode(counter));
    }
    for (uint64_t counter = gem5::RiscvISA::CSR_MMCOUNTINHIBIT;
                  counter <= gem5::RiscvISA::CSR_MHPMEVENT31; counter++) {
        skipCSRs.push_back(GetCSROPInstCode(counter));
    }
    for (uint64_t counter = gem5::RiscvISA::CSR_CYCLE;
                  counter <= gem5::RiscvISA::CSR_HPMCOUNTER31; counter++) {
        skipCSRs.push_back(GetCSROPInstCode(counter));
    }
}

NemuProxy::NemuProxy(int coreid, const char *ref_so, bool enable_sdcard_diff, bool enable_mem_dedup, bool multi_core)
{
    handle = dlmopen(LM_ID_NEWLM, ref_so, RTLD_LAZY | RTLD_DEEPBIND);
    printf("Using %s for difftest\n", ref_so);
    if (!handle) {
        printf("%s\n", dlerror());
        assert(0);
    }

    if (enable_mem_dedup) {
        this->ref_get_backed_memory =
            (void (*)(void *backed_mem, size_t n))dlsym(handle, "difftest_get_backed_memory");
        assert(this->ref_get_backed_memory);
    }

    this->memcpy = (void (*)(paddr_t, void *, size_t, bool))dlsym(
        handle, "difftest_memcpy");
    assert(this->memcpy);

    this->memcpy_init = (void (*)(paddr_t, void *, size_t, bool))dlsym(
        handle, "difftest_memcpy_init");
    if (this->memcpy_init == nullptr) {
        warn("difftest_memcpy_init not found, using difftest_memcpy instead");
        this->memcpy_init = this->memcpy;
    }
    assert(this->memcpy_init);

    regcpy = (void (*)(void *, bool))dlsym(handle, "difftest_regcpy");
    assert(regcpy);

    csrcpy = (void (*)(void *, bool))dlsym(handle, "difftest_csrcpy");
    assert(csrcpy);

    uarchstatus_cpy =
        (void (*)(void *, bool))dlsym(handle, "difftest_uarchstatus_cpy");
    assert(uarchstatus_cpy);

    exec = (void (*)(uint64_t))dlsym(handle, "difftest_exec");
    assert(exec);

    guided_exec = (vaddr_t(*)(void *))dlsym(handle, "difftest_guided_exec");
    assert(guided_exec);

    update_config = (vaddr_t(*)(void *))dlsym(handle, "update_dynamic_config");
    assert(update_config);

    store_commit = (int (*)(uint64_t *, uint64_t *, uint8_t *))dlsym(
        handle, "difftest_store_commit");
    assert(store_commit);

    raise_intr = (void (*)(uint64_t))dlsym(handle, "difftest_raise_intr");
    assert(raise_intr);

    isa_reg_display = (void (*)(void))dlsym(handle, "isa_reg_display");
    assert(isa_reg_display);

    query = (void (*)(void *, uint64_t))dlsym(handle, "difftest_query_ref");
#ifdef ENABLE_RUNHEAD
    assert(query);
#endif

    multiCore = multi_core;

    if (enable_sdcard_diff) {
        sdcard_init = (void (*)(const char *, const char *))dlsym(
            handle, "difftest_sdcard_init");
        assert(sdcard_init);
    }

    skipPerfCntCsr();

    auto nemu_init = (void (*)(void))dlsym(handle, "difftest_init");
    assert(nemu_init);

    nemu_init();
}

void
NemuProxy::initState(int coreid, uint8_t *golden_mem)
{
    if (multiCore) {
        auto nemu_difftest_set_mhartid = (void (*)(int))dlsym(handle, "difftest_set_mhartid");
        warn("Setting mhartid to %d\n", coreid);
        assert(nemu_difftest_set_mhartid);
        nemu_difftest_set_mhartid(coreid);

        auto nemu_difftest_put_gmaddr = (void (*)(uint8_t *ptr))dlsym(handle, "difftest_put_gmaddr");
        warn("Setting gmaddr to %#lx\n", (uint64_t) golden_mem);
        assert(nemu_difftest_put_gmaddr);
        nemu_difftest_put_gmaddr(golden_mem);
    }
}


SpikeProxy::SpikeProxy(int coreid, const char *ref_so, bool enable_sdcard_diff)
{
    handle = dlmopen(LM_ID_NEWLM, ref_so, RTLD_LAZY | RTLD_DEEPBIND);
    printf("Using %s for difftest\n", ref_so);
    if (!handle) {
        printf("%s\n", dlerror());
        assert(0);
    }

    this->memcpy = (void (*)(paddr_t, void *, size_t, bool))dlsym(
        handle, "difftest_memcpy");
    assert(this->memcpy);

    regcpy = (void (*)(void *, bool))dlsym(handle, "difftest_regcpy");
    assert(regcpy);

    csrcpy = (void (*)(void *, bool))dlsym(handle, "difftest_csrcpy");
    assert(csrcpy);

    uarchstatus_cpy =
        (void (*)(void *, bool))dlsym(handle, "difftest_uarchstatus_sync");
    assert(uarchstatus_cpy);

    exec = (void (*)(uint64_t))dlsym(handle, "difftest_exec");
    assert(exec);

    guided_exec = (vaddr_t(*)(void *))dlsym(handle, "difftest_guided_exec");
    assert(guided_exec);

    update_config = (vaddr_t(*)(void *))dlsym(handle, "update_dynamic_config");
    assert(update_config);

    store_commit = (int (*)(uint64_t *, uint64_t *, uint8_t *))dlsym(
        handle, "difftest_store_commit");
    assert(store_commit);

    raise_intr = (void (*)(uint64_t))dlsym(handle, "difftest_raise_intr");
    assert(raise_intr);

    isa_reg_display = (void (*)(void))dlsym(handle, "isa_reg_display");
    assert(isa_reg_display);

    if (coreid > 0) {
        panic("Multi-core difftest on spike is not supported or not tested\n");
    }

    assert(!enable_sdcard_diff);

    skipPerfCntCsr();

    auto nemu_init = (void (*)(void))dlsym(handle, "difftest_init");
    assert(nemu_init);

    nemu_init();
}

