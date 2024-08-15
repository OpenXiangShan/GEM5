#include "dev/riscv/nemu_mmc.hh"

#include "mem/packet.hh"
#include "mem/packet_access.hh"
#include "nemu_mmc.hh"
#include "sim/system.hh"

namespace gem5
{
using namespace RiscvISA;

NemuMMC::NemuMMC(const NemuMMCParams *p)
    : BasicPioDevice(*p, p->pio_size)
    , blkcnt(0)
    , blk_addr(0)
    , tmp_addr(0)
    , write_cmd(false)
    , read_ext_csd(false)
{
    assert(C_SIZE < (1 << 12));
    sd_reg_base = (uint32_t *)malloc(0x80);
    img_fp = fopen(p->img_path.c_str(), "rb");
    sdfp = fopen(p->cpt_bin_path.c_str(), "rb");
    if (sdfp) {
        unserialize_sdcard(sdfp);
        fclose(sdfp);
    }
}

inline void
NemuMMC::prepare_rw(int is_write)
{
    blk_addr = sd_reg_base[SDARG];
    tmp_addr = 0;
    if (img_fp)
        fseek(img_fp, blk_addr << 9, SEEK_SET);
    write_cmd = is_write;
}

void
NemuMMC::sdcard_handle_cmd(int cmd)
{
    switch (cmd) {
        case MMC_GO_IDLE_STATE:
            break;
        case MMC_SEND_OP_COND:
            sd_reg_base[SDRSP0] = 0x80ff8000;
            break;
        case MMC_ALL_SEND_CID:
            sd_reg_base[SDRSP0] = 0x00000001;
            sd_reg_base[SDRSP1] = 0x00000000;
            sd_reg_base[SDRSP2] = 0x00000000;
            sd_reg_base[SDRSP3] = 0x15000000;
            break;
        case 52:  // ???
            break;
        case MMC_SEND_CSD:
            sd_reg_base[SDRSP0] = 0x92404001;
            sd_reg_base[SDRSP1] = 0x124b97e3 | ((C_SIZE & 0x3) << 30);
            sd_reg_base[SDRSP2] =
                0x0f508000 | (C_SIZE >> 2) | (READ_BL_LEN << 16);
            sd_reg_base[SDRSP3] = 0x9026012a;
            break;
        case MMC_SEND_EXT_CSD:
            read_ext_csd = true;
            tmp_addr = 0;
            break;
        case MMC_SLEEP_AWAKE:
            break;
        case MMC_APP_CMD:
            break;
        case MMC_SET_RELATIVE_ADDR:
            break;
        case MMC_SELECT_CARD:
            break;
        case MMC_SET_BLOCK_COUNT:
            blkcnt = sd_reg_base[SDARG] & 0xffff;
            break;
        case MMC_READ_MULTIPLE_BLOCK:
            prepare_rw(false);
            break;
        case MMC_WRITE_MULTIPLE_BLOCK:
            prepare_rw(true);
            break;
        case MMC_SEND_STATUS:
            sd_reg_base[SDRSP0] = 0;
            sd_reg_base[SDRSP1] = 0;
            sd_reg_base[SDRSP2] = 0;
            sd_reg_base[SDRSP3] = 0;
            break;
        case MMC_STOP_TRANSMISSION:
            break;
        default:
            panic("unhandled command = %d", cmd);
    }
}

void
NemuMMC::sdcard_io_handler(uint32_t offset)
{
    assert(img_fp);
    int idx = offset / 4;
    switch (idx) {
        case SDCMD:
            sdcard_handle_cmd(sd_reg_base[SDCMD] & 0x3f);
            break;
        case SDARG:
        case SDRSP0:
        case SDRSP1:
        case SDRSP2:
        case SDRSP3:
            break;
        case SDDATA:
            if (read_ext_csd) {
                // See section 8.1 JEDEC Standard JED84-A441
                uint32_t data;
                switch (tmp_addr) {
                    case 192:
                        data = 2;
                        break;  // EXT_CSD_REV
                    case 212:
                        data = MEMORY_SIZE / 512;
                        break;
                    default:
                        data = 0;
                }
                sd_reg_base[SDDATA] = data;
                if (tmp_addr == 512 - 4)
                    read_ext_csd = false;
            } else if (img_fp) {
                __attribute__((unused)) int ret;
                if (!write_cmd) {
                    ret = fread(&sd_reg_base[SDDATA], 4, 1, img_fp);
                } else {
                    ret = fwrite(&sd_reg_base[SDDATA], 4, 1, img_fp);
                }
            }
            tmp_addr += 4;
            break;
        default:
            panic("unhandle offset = %d", offset);
    }
}

void
NemuMMC::unserialize_sdcard(FILE *sdfp)
{
    __attribute__((unused)) int ret;
    ret = fread(sd_reg_base, 4, 0x80 / 4, sdfp);
    ret = fread(&tmp_addr, 4, 1, sdfp);
    ret = fread(&write_cmd, 1, 1, sdfp);
    ret = fread(&read_ext_csd, 1, 1, sdfp);
    uint64_t pos;
    ret = fread(&pos, 8, 1, sdfp);
    ret = fseek(img_fp, pos, SEEK_SET);
}

Tick
NemuMMC::read(PacketPtr pkt)
{
    assert(pkt->getSize() == 4);
    Addr offset = pkt->getAddr() - pioAddr;
    // handler before read
    sdcard_io_handler(offset);
    int idx = offset / 4;
    DPRINTF(NemuMMC, "offset = 0x%x(idx = %d), is_write = %d, data = 0x%x\n",
            offset, idx, 0, sd_reg_base[idx]);
    uint32_t ret_val = sd_reg_base[idx];
    pkt->setLE(ret_val);
    pkt->makeAtomicResponse();
    return pioDelay;
}

Tick
NemuMMC::write(PacketPtr pkt)
{
    assert(pkt->getSize() == 4);
    Addr offset = pkt->getAddr() - pioAddr;
    uint32_t write_val = pkt->getRaw<uint32_t>();
    int idx = offset / 4;
    sd_reg_base[idx] = write_val;
    // handler after write
    sdcard_io_handler(offset);
    DPRINTF(NemuMMC, "offset = 0x%x(idx = %d), is_write = %d, data = 0x%x\n",
            offset, idx, 1, sd_reg_base[idx]);
    pkt->makeAtomicResponse();
    return pioDelay;
}

gem5::NemuMMC *
NemuMMCParams::create() const
{
    return new NemuMMC(this);
}
}
