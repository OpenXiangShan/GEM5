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

#ifndef __MEMORY_PADDR_H__
#define __MEMORY_PADDR_H__

#include <cstdint>

#include <base/types.hh>

namespace gem5
{

class GoldenGloablMem
{
  public:
    // todo: rewrite function names with camel case
    void initGoldenMem(Addr pmem_base, Addr pmem_size, uint8_t *pmem_ptr);
    void goldenMemFinish();

    /* convert the guest physical address in the guest program to host virtual address in difftest */
    void *guestToHost(uint64_t addr);
    /* convert the host virtual address in difftest to guest physical address in the guest program */
    uint64_t hostToGuest(void *addr);

    void updateGoldenMem(uint64_t addr, void *data, uint64_t mask, int len);
    void pmemWriteCheck(uint64_t addr, uint64_t data, int len);
    void pmemWrite(uint64_t addr, uint64_t data, int len);

    void readGoldenMem(uint64_t addr, void *data, uint64_t len);
    uint64_t pmemReadCheck(uint64_t addr, int len);
    uint64_t pmemRead(uint64_t addr, int len);

    bool isSfenceSafe(uint64_t addr, int len);
    bool inPmem(uint64_t addr);

    uint8_t *getGoldenMemPtr() const { return goldenMem; }

  private:
    Addr pmemBase;
    Addr pmemSize;
    uint8_t *goldenMem;
};

} // namespace gem5

#endif // __MEMORY_PADDR_H__
