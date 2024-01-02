/*
 * Copyright (c) 2001-2005 The Regents of The University of Michigan
 * Copyright (c) 2007 MIPS Technologies, Inc.
 * Copyright (c) 2020 Barkhausen Institut
 * Copyright (c) 2021 Huawei International
 * All rights reserved.
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

#ifndef __ARCH_RISCV_TLB_HH__
#define __ARCH_RISCV_TLB_HH__

#include <list>

#include "arch/generic/tlb.hh"
#include "arch/riscv/isa.hh"
#include "arch/riscv/pagetable.hh"
#include "arch/riscv/pma_checker.hh"
#include "arch/riscv/regs/misc.hh"
#include "arch/riscv/utility.hh"
#include "base/statistics.hh"
#include "mem/request.hh"
#include "params/RiscvTLB.hh"
#include "sim/sim_object.hh"

namespace gem5
{

class ThreadContext;

/* To maintain compatibility with other architectures, we'll
   simply create an ITLB and DTLB that will point to the real TLB */
namespace RiscvISA {

class Walker;

class TLB : public BaseTLB
{
    typedef std::list<TlbEntry *> EntryList;

  protected:
    bool is_dtlb;
    bool is_L1tlb;
    bool isStage2;
    bool isTheSharedL2;
    size_t size;
    size_t sizeBack;
    size_t l2TlbL1Size;
    size_t l2TlbL2Size;
    size_t l2TlbL3Size;
    size_t l2TlbSpSize;
    size_t l2tlbLineSize;
    uint64_t regulationNum;
    std::vector<TlbEntry> tlb;  // our TLB
    TlbEntryTrie trie;          // for quick access
    EntryList freeList;         // free entries
    uint64_t lruSeq;
    bool  hitInSp;
    uint64_t hitPreEntry;
    uint64_t hitPreNum;
    uint64_t RemovePreUnused;
    uint64_t AllPre;
    bool isOpenAutoNextLine;
    uint64_t forwardPreSize;
    bool openForwardPre;
    bool openBackPre;
    bool backPrePrecision;
    bool forwardPrePrecision;
    uint64_t allForwardPre;
    uint64_t removeNoUseForwardPre;
    uint64_t removeNoUseBackPre;
    uint64_t usedBackPre;
    uint64_t allUsed;
    uint64_t forwardUsedPre;
    uint64_t lastVaddr;
    uint64_t lastPc;
    uint64_t traceFlag;


    Walker *walker;

    struct TlbStats : public statistics::Group
    {
        TlbStats(statistics::Group *parent);

        statistics::Scalar readHits;
        statistics::Scalar readMisses;
        statistics::Scalar readAccesses;
        statistics::Scalar writeHits;
        statistics::Scalar writeMisses;
        statistics::Scalar writeAccesses;
        statistics::Scalar readprefetchHits;
        statistics::Scalar writeprefetchHits;
        statistics::Scalar readprefetchAccesses;
        statistics::Scalar writeprefetchAccesses;
        statistics::Scalar readprefetchMisses;
        statistics::Scalar writeprefetchMisses;
        statistics::Scalar writeHitsSquashed;
        statistics::Scalar readHitsSquashed;
        statistics::Scalar squashedInsert;
        statistics::Scalar ALLInsert;
        statistics::Scalar backHits;
        statistics::Scalar usedBackPre;
        statistics::Scalar removeNoUseBackPre;
        statistics::Scalar usedForwardPre;
        statistics::Scalar removeNoUseForwardPre;


        statistics::Scalar writeL2l3TlbMisses;
        statistics::Scalar ReadL2l3TlbMisses;
        statistics::Scalar writeL2Tlbl3Hits;
        statistics::Scalar ReadL2Tlbl3Hits;
        statistics::Scalar squashedInsertL2;
        statistics::Scalar ALLInsertL2;
        statistics::Scalar writeL2l3TlbSquashedHits;
        statistics::Scalar ReadL2l3TlbSquashedHits;

        statistics::Scalar l1tlbRemove;
        statistics::Scalar l1tlbUsedRemove;
        statistics::Scalar l1tlbUnusedRemove;

        statistics::Scalar l2l1tlbRemove;
        statistics::Scalar l2l1tlbUsedRemove;
        statistics::Scalar l2l1tlbUnusedRemove;

        statistics::Scalar l2l2tlbRemove;
        statistics::Scalar l2l2tlbUsedRemove;
        statistics::Scalar l2l2tlbUnusedRemove;

        statistics::Scalar l2l3tlbRemove;
        statistics::Scalar l2l3tlbUsedRemove;
        statistics::Scalar l2l3tlbUnusedRemove;

        statistics::Scalar l2sptlbRemove;
        statistics::Scalar l2sptlbUsedRemove;
        statistics::Scalar l2sptlbUnusedRemove;


        statistics::Scalar hitPreEntry;
        statistics::Scalar hitPreNum;
        statistics::Scalar RemovePreUnused;
        statistics::Scalar AllPre;

        statistics::Formula hits;
        statistics::Formula misses;
        statistics::Formula accesses;
    } stats;

  public:
    PMAChecker *pma;
    PMP *pmp;

  public:
    typedef RiscvTLBParams Params;
    TLB(const Params &p);

    Walker *getWalker();

    void takeOverFrom(BaseTLB *old) override {}

    TlbEntry *insert(Addr vpn, const TlbEntry &entry, bool suqashed_update);
    TlbEntry *insert_forward_pre(Addr vpn, const TlbEntry &entry);
    TlbEntry *insert_back_pre(Addr vpn, const TlbEntry &entry);

    TlbEntry *L2TLB_insert(Addr vpn, const TlbEntry &entry, int level,
                           int choose, int sign,bool squashed_update);
    TlbEntry *L2TLB_insert_in(Addr vpn, const TlbEntry &entry, int choose,
                              EntryList *List, TlbEntryTrie *Trie_l2,
                              int sign,bool squashed_update);
    // TlbEntry *L2TLB_insert_in(Addr vpn,const TlbEntry &entry,int level);


    void flushAll() override;
    void demapPage(Addr vaddr, uint64_t asn) override;
    void demapPageL2(Addr vaddr,uint64_t asn);

    Fault checkPermissions(STATUS status, PrivilegeMode pmode, Addr vaddr,
                           BaseMMU::Mode mode, PTESv39 pte);
    Fault createPagefault(Addr vaddr, BaseMMU::Mode mode);

    PrivilegeMode getMemPriv(ThreadContext *tc, BaseMMU::Mode mode);

    // Checkpointing
    void serialize(CheckpointOut &cp) const override;
    void unserialize(CheckpointIn &cp) override;

    /**
     * Get the table walker port. This is used for
     * migrating port connections during a CPU takeOverFrom()
     * call. For architectures that do not have a table walker,
     * NULL is returned, hence the use of a pointer rather than a
     * reference. For RISC-V this method will always return a valid
     * port pointer.
     *
     * @return A pointer to the walker port
     */
    Port *getTableWalkerPort() override;

    Addr translateWithTLB(Addr vaddr, uint16_t asid, BaseMMU::Mode mode);

    Fault L2tlb_pagefault(Addr vaddr, BaseMMU::Mode mode, const RequestPtr &req, bool is_pre, bool is_back_pre);

    Fault L2tlb_check(PTESv39 pte, int level, STATUS status, PrivilegeMode pmode, Addr vaddr, BaseMMU::Mode mode,
                      const RequestPtr &req, bool is_pre, bool is_back_pre);

    Fault translateAtomic(const RequestPtr &req,
                          ThreadContext *tc, BaseMMU::Mode mode) override;
    void translateTiming(const RequestPtr &req, ThreadContext *tc,
                         BaseMMU::Translation *translation,
                         BaseMMU::Mode mode) override;
    Fault translateFunctional(const RequestPtr &req, ThreadContext *tc,
                              BaseMMU::Mode mode) override;
    Fault finalizePhysical(const RequestPtr &req, ThreadContext *tc,
                           BaseMMU::Mode mode) const override;
    TlbEntry *lookup(Addr vpn, uint16_t asid, BaseMMU::Mode mode, bool hidden,
                     bool sign_used);
    TlbEntry *lookup_forward_pre(Addr vpn, uint64_t asid, bool hidden);
    TlbEntry *lookup_back_pre(Addr vpn, uint64_t asid, bool hidden);
    bool auto_open_nextline();



    std::vector<TlbEntry> tlbL2L1;  // our TLB
    TlbEntryTrie trie_l2l1;               // for next line
    EntryList freeList_l2l1;         // free entries

    std::vector<TlbEntry> tlbL2L2;  // our TLB
    TlbEntryTrie trie_l2l2;               // for next line
    EntryList freeList_l2l2;         // free entries

    std::vector<TlbEntry> tlbL2L3;  // our TLB
    TlbEntryTrie trie_l2l3;               // for next line
    EntryList freeList_l2l3;         // free entries

    std::vector<TlbEntry> tlbL2Sp;  // our TLB
    TlbEntryTrie trie_l2sp;               // for next line
    EntryList freeList_l2sp;         // free entries


    std::vector<TlbEntry> forwardPre;
    TlbEntryTrie trie_forward_pre;
    EntryList freeList_forward_pre;

    std::vector<TlbEntry> backPre;
    TlbEntryTrie trie_back_pre;
    EntryList freeList_back_pre;

  private:
    uint64_t nextSeq() { return ++lruSeq; }

    TlbEntry *lookup_l2tlb(Addr vpn, uint16_t asid, BaseMMU::Mode mode,
                           bool hidden, int f_level, bool sign_used);

    void evictLRU();
    void evict_forward_pre();
    void evict_back_pre();

    void l2TLB_evictLRU(int l2TLBlevel, Addr vaddr);

    void remove(size_t idx);
    void remove_forward_pre(size_t idx);
    void remove_back_pre(size_t idx);
    void l2TLB_remove(size_t idx, int choose);


    Fault translate(const RequestPtr &req, ThreadContext *tc,
                    BaseMMU::Translation *translation, BaseMMU::Mode mode,
                    bool &delayed);
    Fault doTranslate(const RequestPtr &req, ThreadContext *tc,
                      BaseMMU::Translation *translation, BaseMMU::Mode mode,
                      bool &delayed);

};

} // namespace RiscvISA
} // namespace gem5

#endif // __RISCV_MEMORY_HH__
