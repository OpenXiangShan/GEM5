#ifndef __COMMON_H__
#define __COMMON_H__

#include <stdint.h>

#include <cassert>
#include <cstring>

#ifndef NUM_CORES
#define NUM_CORES 1
#endif
enum { DIFFTEST_TO_DUT, DIFFTEST_TO_REF };
enum { REF_TO_DUT, DUT_TO_REF };
enum { REF_TO_DIFFTEST, DUT_TO_DIFFTEST };

typedef uint64_t rtlreg_t;

typedef uint64_t paddr_t;
typedef uint64_t vaddr_t;

typedef uint16_t ioaddr_t;

#include "nemu_macro.hh"

// 0~31: GPRs, 32~63 FPRs
enum
{
    DIFFTEST_THIS_PC = 64,
    DIFFTEST_MSTATUS,
    DIFFTEST_MCAUSE,
    DIFFTEST_MEPC,
    DIFFTEST_SSTATUS,
    DIFFTEST_SCAUSE,
    DIFFTEST_SEPC,
    DIFFTEST_SATP,
    DIFFTEST_MIP,
    DIFFTEST_MIE,
    DIFFTEST_MSCRATCH,
    DIFFTEST_SSCRATCH,
    DIFFTEST_MIDELEG,
    DIFFTEST_MEDELEG,
    DIFFTEST_MTVAL,
    DIFFTEST_STVAL,
    DIFFTEST_MTVEC,
    DIFFTEST_STVEC,
    DIFFTEST_MODE,
    DIFFTEST_NR_REG
};


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

struct DiffState
{
    // Regs and mode for single step difftest
    int commit;
    uint64_t *nemu_reg;
    uint64_t *gem5_reg;
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
};

class RefProxy
{
  public:
    // public callable functions
    void (*memcpy)(paddr_t nemu_addr, void *dut_buf, size_t n,
                   bool direction) = NULL;
    void (*regcpy)(void *dut, bool direction) = NULL;
    void (*csrcpy)(void *dut, bool direction) = NULL;
    void (*uarchstatus_cpy)(void *dut, bool direction) = NULL;
    int (*store_commit)(uint64_t *saddr, uint64_t *sdata,
                        uint8_t *smask) = NULL;
    void (*exec)(uint64_t n) = NULL;
    vaddr_t (*guided_exec)(void *disambiguate_para) = NULL;
    vaddr_t (*update_config)(void *config) = NULL;
    void (*raise_intr)(uint64_t no) = NULL;
    void (*isa_reg_display)() = NULL;
    void (*query)(void *result_buffer, uint64_t type) = NULL;
    void (*debug_mem_sync)(paddr_t addr, void *bytes, size_t size) = NULL;
    void (*sdcard_init)(const char *img_path,
                        const char *sd_cpt_bin_path) = NULL;
};

class NemuProxy : public RefProxy
{
  public:
    NemuProxy(int coreid, const char *ref_so);

  private:
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

extern uint8_t *pmemStart;
extern uint64_t pmemSize;
extern const char *reg_name[];

#endif
