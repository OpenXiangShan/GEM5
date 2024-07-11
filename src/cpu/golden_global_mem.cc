/***************************************************************************************
 * Copyright (c) 2020-2023 Institute of Computing Technology, Chinese Academy of Sciences
 * Copyright (c) 2020-2021 Peng Cheng Laboratory
 *
 * DiffTest is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 *
 * See the Mulan PSL v2 for more details.
 ***************************************************************************************/

#include "cpu/golden_global_mem.hh"

#include <sys/mman.h>

#include <cstdint>
#include <cstdlib>
#include <ctime>

#include "base/logging.hh"
#include "mem/mem_util.hh"

namespace gem5
{

extern uint8_t *pmemStart;
extern uint64_t pmemSize;

void
GoldenGloablMem::initGoldenMem(Addr pmem_base, Addr pmem_size, uint8_t *pmem_ptr)
{
    goldenMem = pmem_ptr;
    if (goldenMem == (uint8_t *)nullptr) {
        panic("ERROR allocating golden global memory. \n");
    }
    pmemBase = pmem_base;
    pmemSize = pmem_size;
    goldenMemDB.init_db();
    std::vector<std::pair<std::string, DataType>> fields_vec = {
                std::make_pair("dest", UINT64),
                std::make_pair("src", UINT64),
                std::make_pair("addr", UINT64),
                std::make_pair("reuse_time", UINT64)
            };
    goldenMemTrace = goldenMemDB.addAndGetTrace("GMTRACE", fields_vec);
    goldenMemTrace->init_table();
    goldenMemInfo = new GoldenMemInfo[pmemSize / 64];
    memset(goldenMemInfo, 0, sizeof(GoldenMemInfo) * (pmemSize / 64));

    registerExitCallback([this](){ goldenMemFinish(); });

}

void *
GoldenGloablMem::guestToHost(uint64_t addr)
{
    return &goldenMem[addr - pmemBase];
}

void
GoldenGloablMem::goldenMemFinish()
{
    // memory is released in dedupMemManager
    delete []goldenMemInfo;
    goldenMemDB.save_db("golden_mem_db.db");
}

void
GoldenGloablMem::updateGoldenMem(uint64_t addr, void *data, uint64_t mask, int len)
{
    uint8_t *dataArray = (uint8_t *)data;
    for (int i = 0; i < len; i++) {
        if (((mask >> i) & 1) != 0) {
            pmemWriteCheck(addr + i, dataArray[i], 1);
        }
    }
}

void
GoldenGloablMem::updateGoldenMem(uint64_t addr, void *data, const std::vector<bool>& mask, int len)
{
    uint8_t *dataArray = (uint8_t *)data;
    for (int i = 0; i < len; i++) {
        if (mask[i]) {
            pmemWriteCheck(addr + i, dataArray[i], 1);
        }
    }
}

void
GoldenGloablMem::readGoldenMem(uint64_t addr, void *data, uint64_t len)
{
    *(uint64_t *)data = pmemReadCheck(addr, len);
}

void GoldenGloablMem::recordLoad(Addr addr, ThreadID tid) {
    auto gi= goldenMemInfo[(addr - pmemBase) / 64];
    if (gi.modified) {
        Tick reuse_time = curTick() - gi.lastUpdateTick;
        goldenMemTrace->write_record({GoldenMemTrace(tid, gi.lastUpdater, addr, reuse_time)});

    }
}

void GoldenGloablMem::recordStore(Addr addr, ThreadID tid) {
    // if pmem writecheck failed it will panic
    // Maybe record time stored as well?
    // if (addr / 64 != (addr + len) / 64) {
    //     panic("Cross cache line write is not supported! addr: %#lx, mem start: %#lx, mem size: %lx\n",
    //             addr, pmemBase, pmemSize);
    // }

    goldenMemInfo[(addr - pmemBase) / 64] = { curTick(), tid, true, false};

}

void GoldenGloablMem::recordAtomic(Addr addr, ThreadID tid) {
    goldenMemInfo[(addr - pmemBase) / 64] = { curTick(), tid, true, true};
}

bool
GoldenGloablMem::inPmem(uint64_t addr)
{
    return (pmemBase <= addr) && (addr <= pmemBase + pmemSize - 1);
}

uint64_t
GoldenGloablMem::pmemRead(uint64_t addr, int len)
{
    void *p = &goldenMem[addr - pmemBase];
    switch (len) {
        case 1:
            return *(uint8_t *)p;
        case 2:
            return *(uint16_t *)p;
        case 4:
            return *(uint32_t *)p;
        case 8:
            return *(uint64_t *)p;
        default:
            assert(0);
    }
}

void
GoldenGloablMem::pmemWrite(Addr addr, uint64_t data, int len)
{
    void *p = &goldenMem[addr - pmemBase];
    switch (len) {
        case 1:
            *(uint8_t *)p = data;
            return;
        case 2:
            *(uint16_t *)p = data;
            return;
        case 4:
            *(uint32_t *)p = data;
            return;
        case 8:
            *(uint64_t *)p = data;
            return;
        default:
            assert(0);
    }
}

/* Memory accessing interfaces */
uint64_t
GoldenGloablMem::pmemReadCheck(Addr addr, int len)
{
    if (inPmem(addr)) {
        return pmemRead(addr, len);
    } else {
        // ???
        warn("[Hint] read not in pmem, maybe in speculative state! addr: %lx\n", addr);
    }
    return 0;
}

void
GoldenGloablMem::pmemWriteCheck(Addr addr, uint64_t data, int len)
{
    if (inPmem(addr)) {
#ifdef ENABLE_STORE_LOG
        if (goldenmem_store_log_enable)
            pmem_record_store(addr);
#endif  // ENABLE_STORE_LOG
        pmemWrite(addr, data, len);
    } else {
        panic("write not in pmem! addr: %#lx, mem start: %#lx, mem size: %lx\n", addr, pmemBase, pmemSize);
    }
}

}  // namespace gem5
