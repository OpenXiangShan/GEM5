/*
 * Copyright (c) 2020 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __ARCH_RISCV_MMU_HH__
#define __ARCH_RISCV_MMU_HH__

#include "arch/generic/mmu.hh"
#include "arch/riscv/isa.hh"
#include "arch/riscv/page_size.hh"
#include "arch/riscv/pma_checker.hh"
#include "arch/riscv/tlb.hh"

#include "params/RiscvMMU.hh"

namespace gem5
{

namespace RiscvISA {

class MMU : public BaseMMU
{
  public:
    PMAChecker *pma;
    // Extra store-side root. BaseMMU only owns architectural itb/dtb roots,
    // so RISC-V must explicitly route and maintain this TLB when enabled.
    TLB *stb;
    // Static simulation mode. False preserves the original shared dtb;
    // true gives write translations their own stb state.
    const bool enableStoreTlb;

    MMU(const RiscvMMUParams &p)
      : BaseMMU(p), pma(p.pma_checker), stb(p.stb),
        enableStoreTlb(p.enable_store_tlb)
    {}

    // Select the data L1 TLB in O(1). The default shared mode sends reads and
    // writes to dtb; split mode sends writes to stb. Execute accesses bypass
    // this helper and are routed to itb by callers.
    BaseTLB *dataTlb(BaseMMU::Mode mode) const
    {
        return enableStoreTlb && mode == BaseMMU::Write ? stb : dtb;
    }

    // Keep all translation entry points on the same mode-to-TLB mapping.
    Fault translateAtomic(const RequestPtr &req, ThreadContext *tc,
                          BaseMMU::Mode mode) override
    {
        return (mode == BaseMMU::Execute ? itb : dataTlb(mode))
            ->translateAtomic(req, tc, mode);
    }

    void translateTiming(const RequestPtr &req, ThreadContext *tc,
                         BaseMMU::Translation *translation,
                         BaseMMU::Mode mode) override
    {
        BaseTLB *tlb = mode == BaseMMU::Execute ? itb : dataTlb(mode);
        if (functional)
            tlb->translateFunctional(req, tc, translation, mode);
        else
            tlb->translateTiming(req, tc, translation, mode);
    }

    Fault translateFunctional(const RequestPtr &req, ThreadContext *tc,
                              BaseMMU::Mode mode) override
    {
        return (mode == BaseMMU::Execute ? itb : dataTlb(mode))
            ->translateFunctional(req, tc, mode);
    }

    Fault finalizePhysical(const RequestPtr &req, ThreadContext *tc,
                           BaseMMU::Mode mode) const override
    {
        return (mode == BaseMMU::Execute ? itb : dataTlb(mode))
            ->finalizePhysical(req, tc, mode);
    }

    void flushAll() override
    {
        BaseMMU::flushAll();
        // BaseMMU traverses only its architectural dtb/itb roots. The active
        // store-only root must therefore be explicitly flushed in split mode.
        if (enableStoreTlb)
            stb->flushAll();
    }

    void demapPage(Addr vaddr, uint64_t asn) override
    {
        // Generic MMU invalidates itb/dtb; stb is an additional root.
        BaseMMU::demapPage(vaddr, asn);
        if (enableStoreTlb)
            stb->demapPage(vaddr, asn);
    }

    TranslationGenPtr
    translateFunctional(Addr start, Addr size, ThreadContext *tc,
            Mode mode, Request::Flags flags) override
    {
        return TranslationGenPtr(new MMUTranslationGen(
                PageBytes, start, size, tc, this, mode, flags));
    }

    PrivilegeMode
    getMemPriv(ThreadContext *tc, BaseMMU::Mode mode)
    {
        return static_cast<TLB*>(dataTlb(mode))->getMemPriv(tc, mode);
    }

    Walker *
    getDataWalker(BaseMMU::Mode mode = BaseMMU::Read)
    {
        // Functional page walks must use the walker belonging to the data
        // TLB selected by mode, especially for write translations.
        return static_cast<TLB*>(dataTlb(mode))->getWalker();
    }

    void
    takeOverFrom(BaseMMU *old_mmu) override
    {
      MMU *ommu = dynamic_cast<MMU*>(old_mmu);
      BaseMMU::takeOverFrom(ommu);
      pma->takeOverFrom(ommu->pma);
      // The dormant stb has no architectural state to preserve in shared
      // mode. In split mode, transfer its entries and walker state as well.
      if (enableStoreTlb)
          stb->takeOverFrom(ommu->stb);

    }

    PMP *
    getPMP()
    {
        return static_cast<TLB*>(dtb)->pmp;
    }

    void
    setOldPriv(ThreadContext *tc) override {
      static_cast<TLB*>(dtb)->setOldPriv(tc);
      if (enableStoreTlb)
          static_cast<TLB*>(stb)->setOldPriv(tc);
      static_cast<TLB*>(itb)->setOldPriv(tc);
    }

    void
    useNewPriv(ThreadContext *tc) override {
      static_cast<TLB*>(dtb)->useNewPriv(tc);
      if (enableStoreTlb)
          static_cast<TLB*>(stb)->useNewPriv(tc);
      static_cast<TLB*>(itb)->useNewPriv(tc);
    }
};

} // namespace RiscvISA
} // namespace gem5

#endif  // __ARCH_RISCV_MMU_HH__
