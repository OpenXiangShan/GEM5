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
  private:
    TLB *const stb;
    TLB *const ptb;
    TLB *const sharedL2;

    TLB *
    selectL1(const RequestPtr &req, Mode mode) const
    {
        if (mode == Write && stb && !req->isLLSC() && !req->isAtomic())
            return stb;
        return static_cast<TLB *>(getTlb(mode));
    }

    template<class F>
    void forEachL1(F visit)
    {
        for (auto *tlb : {static_cast<TLB *>(itb), static_cast<TLB *>(dtb),
                          stb, ptb}) {
            if (tlb)
                visit(*tlb);
        }
    }

  public:
    PMAChecker *pma;

    MMU(const RiscvMMUParams &p)
      : BaseMMU(p), stb(p.stb), ptb(p.ptb), sharedL2(p.l2_shared),
        pma(p.pma_checker)
    {}

    void demapPage(Addr vaddr, uint64_t asid) override
    {
        forEachL1([&](TLB &tlb) { tlb.demapLocal(vaddr, asid); });
        sharedL2->demapPageL2(vaddr, asid);
    }

    void flushAll() override
    {
        forEachL1([](TLB &tlb) { tlb.flushLocal(); });
        sharedL2->flushAll();
    }

    Fault translateAtomic(const RequestPtr &req, ThreadContext *tc,
                          Mode mode) override
    {
        return selectL1(req, mode)->translateAtomic(req, tc, mode);
    }

    void translateTiming(const RequestPtr &req, ThreadContext *tc,
                         Translation *translation, Mode mode) override
    {
        auto *tlb = selectL1(req, mode);
        if (functional)
            tlb->translateFunctional(req, tc, translation, mode);
        else
            tlb->translateTiming(req, tc, translation, mode);
    }

    Fault translateFunctional(const RequestPtr &req, ThreadContext *tc,
                              Mode mode) override
    {
        return selectL1(req, mode)->translateFunctional(req, tc, mode);
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
        return static_cast<TLB*>(dtb)->getMemPriv(tc, mode);
    }

    Walker *
    getDataWalker()
    {
        return static_cast<TLB*>(dtb)->getWalker();
    }

    void
    takeOverFrom(BaseMMU *old_mmu) override
    {
      MMU *ommu = dynamic_cast<MMU*>(old_mmu);
      BaseMMU::takeOverFrom(ommu);
      pma->takeOverFrom(ommu->pma);

    }

    PMP *
    getPMP()
    {
        return static_cast<TLB*>(dtb)->pmp;
    }

    void
    setOldPriv(ThreadContext *tc) override {
      forEachL1([&](TLB &tlb) { tlb.setOldPriv(tc); });
    }

    void
    useNewPriv(ThreadContext *tc) override {
      forEachL1([&](TLB &tlb) { tlb.useNewPriv(tc); });
    }
};

} // namespace RiscvISA
} // namespace gem5

#endif  // __ARCH_RISCV_MMU_HH__
