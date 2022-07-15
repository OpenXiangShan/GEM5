#ifndef __DEV_RISCV_NEMU_MMC_HH__
#define __DEV_RISCV_NEMU_MMC_HH__

#include "base/trace.hh"
#include "base/types.hh"
#include "debug/NemuMMC.hh"
#include "dev/io_device.hh"
#include "dev/platform.hh"
#include "dev/riscv/nemu_mmc.hh"
#include "mmc.h"
#include "params/NemuMMC.hh"
#include "sim/system.hh"

// see page 26 of the manual above
#define MEMORY_SIZE (64ull * 1024 * 1024 * 1024)  // 64GB
#define READ_BL_LEN 15
#define BLOCK_LEN (1 << READ_BL_LEN)
#define NR_BLOCK (MEMORY_SIZE / BLOCK_LEN)
#define C_SIZE_MULT 7  // only 3 bits
#define MULT (1 << (C_SIZE_MULT + 2))
#define C_SIZE (NR_BLOCK / MULT - 1)

// This is a simple hardware implementation of linux/drivers/mmc/host/bcm2835.c
// No DMA and IRQ is supported, so the driver must be modified to start PIO
// right after sending the actual read/write commands.

namespace gem5
{
using namespace RiscvISA;

enum
{
    SDCMD,
    SDARG,
    SDTOUT,
    SDCDIV,
    SDRSP0,
    SDRSP1,
    SDRSP2,
    SDRSP3,
    SDHSTS,
    __PAD0,
    __PAD1,
    __PAD2,
    SDVDD,
    SDEDM,
    SDHCFG,
    SDHBCT,
    SDDATA,
    __PAD10,
    __PAD11,
    __PAD12,
    SDHBLC
};

class NemuMMC : public BasicPioDevice
{
  private:
    FILE *img_fp;
    FILE *sdfp;
    uint32_t *sd_reg_base;
    uint32_t blkcnt;
    long blk_addr;
    uint32_t tmp_addr;
    bool write_cmd;
    bool read_ext_csd;

    inline void prepare_rw(int is_write);
    void sdcard_handle_cmd(int cmd);
    void unserialize_sdcard(FILE *sdfp);
    void sdcard_io_handler(uint32_t offset);

  public:
    typedef NemuMMCParams Params;
    NemuMMC(const Params *p);

    Tick read(PacketPtr pkt) override;
    Tick write(PacketPtr pkt) override;
};
}

#endif
