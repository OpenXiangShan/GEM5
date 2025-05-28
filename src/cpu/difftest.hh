#ifndef __COMMON_H__
#define __COMMON_H__

#include <stdint.h>
#include <vector>

#include <cassert>
#include <cstring>


#include "arch/riscv/types.hh"
#include "base/logging.hh"
#include "cpu/golden_global_mem.hh"

enum
{
    DIFFTEST_TO_DUT, DIFFTEST_TO_REF };
enum { REF_TO_DUT, DUT_TO_REF };
enum { REF_TO_DIFFTEST, DUT_TO_DIFFTEST };

typedef uint64_t rtlreg_t;

typedef uint64_t paddr_t;
typedef uint64_t vaddr_t;

typedef uint16_t ioaddr_t;

#include "nemu_macro.hh"


#define VENUM64 (gem5::RiscvISA::VLEN/64)
#define VENUM32 (gem5::RiscvISA::VLEN/32)
#define VENUM16 (gem5::RiscvISA::VLEN/16)
#define VENUM8 (gem5::RiscvISA::VLEN/8)



struct riscv64_CPU_regfile
{
    union
    {
      uint64_t _64;
    } gpr[32];

    union
    {
      uint64_t _64;
    } fpr[32];

    // shadow CSRs for difftest
    uint64_t mode;
    uint64_t mstatus, sstatus;
    uint64_t mepc, sepc;
    uint64_t mtval, stval;
    uint64_t mtvec, stvec;
    uint64_t mcause, scause;
    uint64_t satp;
    uint64_t mip, mie;
    uint64_t mscratch, sscratch;
    uint64_t mideleg, medeleg;
    uint64_t pc;

    //hypervisor
    uint64_t v; // virtualization mode
    uint64_t mtval2, mtinst, hstatus, hideleg, hedeleg;
    uint64_t hcounteren, htval, htinst, hgatp, vsstatus;
    uint64_t vstvec, vsepc, vscause, vstval, vsatp, vsscratch;

    //vector
    union
    {
      uint64_t _64[VENUM64];
      uint32_t _32[VENUM32];
      uint16_t _16[VENUM16];
      uint8_t  _8[VENUM8];
    } vr[32];

    uint64_t vstart;
    uint64_t vxsat, vxrm, vcsr;
    uint64_t vl, vtype, vlenb;


    uint64_t& operator[](int x) {
        assert(x<64);
        return ((uint64_t*)this)[x];
    }

};

// 0~31: GPRs, 32~63 FPRs
//
// enum
// {
//     DIFFTEST_THIS_PC = 64,
//     DIFFTEST_MSTATUS,
//     DIFFTEST_MCAUSE,
//     DIFFTEST_MEPC,
//     DIFFTEST_SSTATUS,
//     DIFFTEST_SCAUSE,
//     DIFFTEST_SEPC,
//     DIFFTEST_SATP,
//     DIFFTEST_MIP,
//     DIFFTEST_MIE,
//     DIFFTEST_MSCRATCH,
//     DIFFTEST_SSCRATCH,
//     DIFFTEST_MIDELEG,
//     DIFFTEST_MEDELEG,
//     DIFFTEST_MTVAL,
//     DIFFTEST_STVAL,
//     DIFFTEST_MTVEC,
//     DIFFTEST_STVEC,
//     DIFFTEST_MODE,
//     DIFFTEST_NR_REG
// };


struct SyncState
{
    uint64_t lrscValid;
    uint64_t lrscAddr;
};

struct DynamicConfig
{
    bool ignore_illegal_mem_access;
    bool debug_difftest;
};

struct ExecutionGuide {
    // force raise exception
    bool force_raise_exception;
    uint64_t exception_num;
    uint64_t mtval;
    uint64_t stval;
    // need enable h
    uint64_t mtval2;
    uint64_t htval;
    uint64_t vstval;
    // force set jump target
    bool force_set_jump_target;
    uint64_t jump_target;
};

struct DiffState
{
    // Regs and mode for single step difftest
    int commit;
    riscv64_CPU_regfile *nemu_reg;
    riscv64_CPU_regfile *gem5_reg;
    uint32_t this_inst;
    int skip;
    int isRVC;
    uint64_t *wpc;
    uint64_t *wdata;
    uint32_t *wdst;
    int wen;
    uint64_t intrNO;
    uint64_t cause;  // for disambiguate_exec
    int priviledgeMode;
    uint64_t npc;

    // Microarchitucural signal needed to sync status
    struct SyncState sync;

    uint64_t nemu_this_pc;
    uint64_t nemu_commit_inst_pc;
    int cpu_id;
    struct DynamicConfig dynamic_config;
    bool will_handle_intr;

    ExecutionGuide guide;
};

class RefProxy
{
  public:
    // public callable functions
    void (*ref_get_backed_memory)(void *backed_mem, size_t n) = nullptr;
    void (*memcpy_init)(paddr_t nemu_addr, void *dut_buf, size_t n,
                   bool direction) = nullptr;
    void (*memcpy)(paddr_t nemu_addr, void *dut_buf, size_t n,
                   bool direction) = nullptr;
    void (*regcpy)(void *dut, bool direction) = nullptr;
    void (*csrcpy)(void *dut, bool direction) = nullptr;
    void (*uarchstatus_cpy)(void *dut, bool direction) = nullptr;
    int (*store_commit)(uint64_t *saddr, uint64_t *sdata,
                        uint8_t *smask) = nullptr;
    void (*exec)(uint64_t n) = nullptr;
    vaddr_t (*guided_exec)(void *disambiguate_para) = nullptr;
    vaddr_t (*update_config)(void *config) = nullptr;
    void (*raise_intr)(uint64_t no) = nullptr;
    void (*isa_reg_display)() = nullptr;
    void (*query)(void *result_buffer, uint64_t type) = nullptr;
    void (*debug_mem_sync)(paddr_t addr, void *bytes, size_t size) = nullptr;
    void (*sdcard_init)(const char *img_path,
                        const char *sd_cpt_bin_path) = nullptr;
    virtual void initState(int coreid, uint8_t *golden_mem) = 0;

  protected:
    bool multiCore;

    void *handle;
};

class NemuProxy : public RefProxy
{
  public:
    NemuProxy(int coreid, const char *ref_so, bool enable_sdcard_diff, bool enable_mem_dedup, bool multi_core);

    void initState(int coreid, uint8_t *golden_mem) override;
};


class SpikeProxy : public RefProxy
{
  public:
    SpikeProxy(int coreid, const char *ref_so, bool enable_sdcard_diff);

    void initState(int coreid, uint8_t *golden_mem) override { panic("Not implemented\n"); }
};

#define DIFFTEST_WIDTH 8

enum DiffAt
{
    NoneDiff = 0,
    PCDiff,
    NPCDiff,
    InstDiff,
    ValueDiff,
};

void skipPerfCntCsr();

extern std::vector<uint64_t> skipCSRs;

extern uint8_t *pmemStart;
extern uint64_t pmemSize;
extern const char *reg_name[];

#endif
