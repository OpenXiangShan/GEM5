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

#include "arch/riscv/tlb.hh"

#include <cstdint>
#include <string>
#include <vector>

#include "arch/riscv/faults.hh"
#include "arch/riscv/mmu.hh"
#include "arch/riscv/page_size.hh"
#include "arch/riscv/pagetable.hh"
#include "arch/riscv/pagetable_walker.hh"
#include "arch/riscv/pma_checker.hh"
#include "arch/riscv/pmp.hh"
#include "arch/riscv/pra_constants.hh"
#include "arch/riscv/utility.hh"
#include "base/inifile.hh"
#include "base/str.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "cpu/thread_context.hh"
#include "debug/TLB.hh"
#include "debug/TLBGPre.hh"
#include "debug/TLBVerbose.hh"
#include "debug/TLBVerbose3.hh"
#include "debug/TLBVerbosel2.hh"
#include "debug/TLBtrace.hh"
#include "debug/autoNextline.hh"
#include "mem/page_table.hh"
#include "mem/request.hh"
#include "params/RiscvTLB.hh"
#include "regs/misc.hh"
#include "sim/full_system.hh"
#include "sim/process.hh"
#include "sim/system.hh"

namespace gem5
{

using namespace RiscvISA;

///////////////////////////////////////////////////////////////////////
//
//  RISC-V TLB
//

static Addr
buildKey(Addr vpn, uint16_t asid, uint8_t translateMode)
{
    return (static_cast<Addr>(asid) << 48) | (static_cast<Addr>(translateMode & 0x3) << 46) |
           ((vpn >> PGSHFT) & (((uint64_t)1 << 46) - 1));
}

TLB::TLB(const Params &p) :
    BaseTLB(p), is_dtlb(p.is_dtlb),is_L1tlb(p.is_L1tlb),isStage2(p.is_stage2),
    isTheSharedL2(p.is_the_sharedL2),size(p.size),sizeBack(32),
    l2TlbL3Size(p.l2tlb_l3_size),
    l2TlbL2Size(p.l2tlb_l2_size),l2TlbL1Size(p.l2tlb_l1_size),
    l2TlbL0Size(p.l2tlb_l0_size),l2TlbSpSize(p.l2tlb_sp_size),
    L2TLB_L1_MASK(0),L2TLB_L0_MASK(0),
    regulationNum(p.regulation_num),
    tlb(size),lruSeq(0),hitInSp(false),
    hitPreEntry(0),hitPreNum(0),
    RemovePreUnused(0),AllPre(0),
    isOpenAutoNextLine(p.is_open_nextline),
    forwardPreSize(p.forward_pre_size),openForwardPre(p.open_forward_pre),
    openBackPre(p.open_back_pre),
    backPrePrecision(p.initial_back_pre_precision_value),
    forwardPrePrecision(p.initial_forward_pre_precision_value),
    controlNum(0),
    allForwardPre(0),removeNoUseForwardPre(0),removeNoUseBackPre(0),
    usedBackPre(0),test_num(0),allUsed(0),forwardUsedPre(0),
    lastVaddr(0),lastPc(0), traceFlag(false),
    stats(this), pma(p.pma_checker),
    pmp(p.pmp),
    tlbL2L3(l2TlbL3Size *l2tlbLineSize),tlbL2L2(l2TlbL2Size *l2tlbLineSize),
    tlbL2L1(l2TlbL1Size *l2tlbLineSize),tlbL2L0(l2TlbL0Size *l2tlbLineSize),
    tlbL2Sp(l2TlbSpSize *l2tlbLineSize),
    forwardPre(forwardPreSize),backPre(32)
{
    L2TLB_L1_MASK = (((uint64_t)1) << static_cast<int>(std::log2(l2TlbL1Size / L2L1LRU_NUM))) - 1;
    L2TLB_L0_MASK = (((uint64_t)1) << static_cast<int>(std::log2(l2TlbL0Size / L2L0LRU_NUM))) - 1;

    if (is_L1tlb) {
        DPRINTF(TLBVerbose, "tlb11\n");
        for (size_t x = 0; x < size; x++) {
            tlb[x].trieHandle = nullptr;
            freeList.push_back(&tlb[x]);
        }
        walker = p.walker;
        walker->setTLB(this);
        TLB *l2tlb;
        if (isStage2) {
            l2tlb = this;
        } else {
            l2tlb = static_cast<TLB *>(nextLevel());
        }
        walker->setL2TLB(l2tlb);
        DPRINTF(TLBVerbose, "tlb11 tlb_size %d size() %d\n", size, tlb.size());

    }
    if (isStage2 || isTheSharedL2) {
        DPRINTF(TLBVerbose, "tlbL2\n");

        configL2Tlb(&freeListL2L3,&trieL2L3,tlbL2L3,l2TlbL3Size,false);
        configL2Tlb(&freeListL2L2,&trieL2L2,tlbL2L2,l2TlbL2Size,false);
        configL2Tlb(&freeListL2L1,&trieL2L1,tlbL2L1,l2TlbL1Size,false);
        configL2Tlb(&freeListL2L0,&trieL2L0,tlbL2L0,l2TlbL0Size,false);
        configL2Tlb(&freeListL2sp,&trieL2sp,tlbL2Sp,l2TlbSpSize,true);

        for (size_t x_g = 0; x_g < forwardPreSize; x_g++) {
            forwardPre[x_g].trieHandle = nullptr;
            freeListForwardPre.push_back(&forwardPre[x_g]);
        }
        for (size_t x_f = 0; x_f < 32; x_f++) {
            backPre[x_f].trieHandle = nullptr;
            freeListBackPre.push_back(&backPre[x_f]);
        }
        DPRINTF(TLBVerbose, "l2l3.size() %d l2l2.size() %d l2l1.size() %d l2l0.size() %d l2sp.size() %d\n",
                tlbL2L3.size(), tlbL2L2.size(), tlbL2L1.size(), tlbL2L0.size(), tlbL2Sp.size());
        DPRINTF(TLBVerbose,
                "tlbl2 size l2tlb_l3_size %d l2tlb_l2_size %d l2tlb_l1_size "
                "%d l2tlb_l0_size %d l2tlb_sp_size %d\n",
                l2TlbL3Size, l2TlbL2Size, l2TlbL1Size, l2TlbL0Size, l2TlbSpSize);
    }
}

Walker *
TLB::getWalker()
{
    return walker;
}
void
TLB::setPTWmode(bool _enable_sv48){
    walker->openSv48 = _enable_sv48;
}
void
TLB::configL2Tlb(EntryList *List_choose, TlbEntryTrie *Trie_l2_choose, std::vector<TlbEntry> &l2Tlb_choose,
                 size_t size, bool sp)
{
    int push_times = 1;
    if (sp) {
        push_times = 3;
    }

    for (size_t x_count = 0; x_count < size * l2tlbLineSize; x_count++) {
        l2Tlb_choose[x_count].trieHandle = nullptr;
        List_choose->push_back(&l2Tlb_choose[x_count]);
    }

    for (int push_time = 0; push_time < push_times; push_time++) {
        l2Tlb.push_back(l2Tlb_choose.data());
        l2TlbSize.push_back(size);
        l2Trie.push_back(Trie_l2_choose);
        l2Freelist.push_back(List_choose);
    }
}
void
TLB::evictLRU()
{
    // Find the entry with the lowest (and hence least recently updated)
    // sequence number.

    size_t lru = 0;
    for (size_t i = 1; i < size; i++) {
        if (tlb[i].lruSeq < tlb[lru].lruSeq)
            lru = i;
    }

    remove(lru);
}
void
TLB::evictForwardPre()
{
    size_t lru = 0;
    for (size_t i = 1; i < forwardPreSize; i++) {
        if (forwardPre[i].lruSeq < forwardPre[lru].lruSeq) {
            lru = i;
        }
    }
    removeForwardPre(lru);
}

void
TLB::evictBackPre()
{
    size_t lru = 0;
    for (size_t i = 1; i < sizeBack; i++) {
        if (backPre[i].lruSeq < backPre[lru].lruSeq) {
            lru = i;
        }
    }
    removeBackPre(lru);
}

void
TLB::l2TLBEvictLRU(int l2TLBlevel, Addr vaddr)
{
    size_t lru;
    size_t i;
    Addr l1_index;
    Addr l0_index;
    l1_index = (vaddr >> (PageShift + 1 * LEVEL_BITS + L2TLB_BLK_OFFSET)) & (L2TLB_L1_MASK);
    l0_index = (vaddr >> (PageShift + 0 * LEVEL_BITS + L2TLB_BLK_OFFSET)) & (L2TLB_L0_MASK);
    int l2_index_num = 0;
    int l1_index_num = 0;
    int l0_index_num = 0;
    DPRINTF(TLB, "l2tlb_evictLRU tlb_l2l1_size %d\n", tlbL2L1.size());
    DPRINTF(TLB, "l2TLBEvictLRU level %d vaddr %#x\n", l2TLBlevel, vaddr);

    if (l2TLBlevel == L_L2L3) {
        lru = 0;
        for (i = l2tlbLineSize; i < l2TlbL3Size * l2tlbLineSize;i += l2tlbLineSize){
            if (tlbL2L3[i].lruSeq < tlbL2L3[lru].lruSeq){
                lru = i;
            }
        }
        l2TLBRemove(lru, L_L2L3);
        DPRINTF(TLB, "removed\n");
    } else if (l2TLBlevel == L_L2L2) {
        lru = 0;
        for (i = l2tlbLineSize; i < l2TlbL2Size * l2tlbLineSize;i += l2tlbLineSize){
            if (tlbL2L2[i].lruSeq < tlbL2L2[lru].lruSeq){
                lru = i;
            }
        }
        l2TLBRemove(lru, L_L2L2);
        DPRINTF(TLB, "removed\n");
    } else if (l2TLBlevel == L_L2L1) {
        lru = 0;
        for (i = 0; i < l2TlbL1Size * l2tlbLineSize; i += l2tlbLineSize) {
            if ((tlbL2L1[i].index == l1_index) && (tlbL2L1[i].trieHandle != nullptr)) {
                DPRINTF(TLBVerbose, "vaddr %#x index %#x\n", tlbL2L1[i].vaddr, l1_index);
                if (l1_index_num == 0) {
                    lru = i;
                } else if (tlbL2L1[i].lruSeq < tlbL2L1[lru].lruSeq) {
                    lru = i;
                }
                l1_index_num++;
            }
        }
        if (l1_index_num == L2L1LRU_NUM) {
            l2TLBRemove(lru, L_L2L1);
            DPRINTF(TLB, "removed\n");
        } else if (l1_index_num > 2) {
            panic("wrong in l2l1 tlb l1_index_num %d\n",l1_index_num);
        } else if (freeListL2L1.empty())
            panic("TLB::l2TLBEvictLRU freeListL2L1 should not be empty.");
        else
            DPRINTF(TLB, "still have entries.\n");
    } else if (l2TLBlevel == L_L2L0) {
        lru = 0;
        for (i = 0; i < l2TlbL0Size * l2tlbLineSize; i = i + l2tlbLineSize) {
            if ((tlbL2L0[i].index == l0_index) && (tlbL2L0[i].trieHandle != nullptr)) {
                if (l0_index_num == 0) {
                    lru = i;
                } else if (tlbL2L0[i].lruSeq < tlbL2L0[lru].lruSeq) {
                    lru = i;
                }
                l0_index_num++;
            }
            else if ((tlbL2L0[i].index == l0_index) && (tlbL2L0[i].trieHandle == nullptr)) {
                DPRINTF(TLB, "trieHandle nullptr. i: %d\n", i);
            }
        }

        if (l0_index_num == L2L0LRU_NUM){
            l2TLBRemove(lru, L_L2L0);
            DPRINTF(TLB, "removed\n");
        } else if (l0_index_num > L2L0LRU_NUM){
            panic("wrong in l2l0 tlb l0_index_num %d\n",l0_index_num);
        } else if (freeListL2L0.empty())
            panic("TLB::l2TLBEvictLRU freeListL2L0 should not be empty."
                  " l0_index_num: %d , tlbL2L0.size: %d.", l0_index_num, tlbL2L0.size());
        else
            DPRINTF(TLB, "still have entries.\n");
    } else if ((l2TLBlevel == L_L2sp1) || (l2TLBlevel == L_L2sp2) || (l2TLBlevel == L_L2sp3)) {
        lru =0;
        for (i = l2tlbLineSize; i < l2TlbSpSize * l2tlbLineSize; i = i + l2tlbLineSize) {
            if (tlbL2Sp[i].lruSeq < tlbL2Sp[lru].lruSeq) {
                lru = i;
            }
        }
        l2TLBRemove(lru, L_L2sp3);
        DPRINTF(TLB, "removed\n");
    }
}

TlbEntry *
TLB::lookup(Addr vpn, uint16_t asid, BaseMMU::Mode mode, bool hidden,
            bool sign_used,uint8_t translateMode)
{
    TlbEntry *entry = trie.lookup(buildKey(vpn, asid, translateMode));

    if (!hidden) {
        if (entry)
            entry->lruSeq = nextSeq();

        if (mode == BaseMMU::Write)
            stats.writeAccesses++;
        else
            stats.readAccesses++;

        if (!entry) {
            if (mode == BaseMMU::Write)
                stats.writeMisses++;
            else
                stats.readMisses++;
        }
        else {
            if (mode == BaseMMU::Write)
                stats.writeHits++;
            else
                stats.readHits++;
        }

        if (entry) {
            if (entry->isSquashed) {
                if (mode == BaseMMU::Write)
                    stats.writeHitsSquashed++;
                else
                    stats.readHitsSquashed++;
            }
        }

        DPRINTF(TLBVerbose, "lookup(vpn=%#x, asid=%#x): %s ppn %#x\n",
                vpn, asid, entry ? "hit" : "miss", entry ? entry->paddr : 0);
    }
    if (sign_used) {
        if (entry) {
            entry->used = true;
        }
    }

    return entry;
}
TlbEntry *
TLB::lookupForwardPre(Addr vpn, uint64_t asid, bool hidden)
{
    TlbEntry *entry = trieForwardPre.lookup(buildKey(vpn, asid, 0));
    if (!hidden) {
        if (entry) {
            entry->lruSeq = nextSeq();
            entry->used = true;
        }
    }
    return entry;
}

TlbEntry *
TLB::lookupBackPre(Addr vpn, uint64_t asid, bool hidden)
{
    TlbEntry *entry = trieBackPre.lookup(buildKey(vpn, asid, 0));
    if (!hidden) {
        if (entry) {
            entry->lruSeq = nextSeq();
            entry->used = true;
            stats.backHits++;
        }
    }
    return entry;
}

bool
TLB::autoOpenNextline()
{
    TLB *l2tlb;

    if (isStage2) {
        l2tlb = this;
    } else {
        l2tlb = static_cast<TLB *>(nextLevel());
    }

    int pre_num_c = l2tlb->AllPre;
    int removePreUnused_c = l2tlb->RemovePreUnused;

    bool auto_nextline = true;
    double precision = (double)((pre_num_c - removePreUnused_c) / (pre_num_c + 1));
    if (isOpenAutoNextLine) {
        if (removePreUnused_c > regulationNum) {
            if (precision < nextlinePrecision) {
                DPRINTF(autoNextline, "pre_num %d removePreUnused %d precision %f\n", pre_num_c, removePreUnused_c,
                        precision);
                auto_nextline = false;
            }
        }
    }
    return auto_nextline;
}
void
TLB::updateL2TLBSeq(TlbEntryTrie *Trie_l2, Addr vpn, Addr step, uint16_t asid, uint8_t translateMode)
{
    for (int i = 0; i < l2tlbLineSize; i++) {
        TlbEntry *m_entry = (*Trie_l2).lookup(buildKey(vpn + step * i, asid, translateMode));
        if (m_entry == nullptr) {
            DPRINTF(TLB, "l2sp1 vaddr basic %#x vaddr %#x \n", vpn, vpn + step * i);
            panic("l2 TLB link num is empty\n");
        }
        m_entry->lruSeq = nextSeq();
    }
}
TlbEntry *
TLB::lookupL2TLB(Addr vpn, uint16_t asid, BaseMMU::Mode mode, bool hidden, int f_level, bool sign_used,
                 uint8_t translateMode)
{
    Addr tem;
    // f_vpnl2l? : vpn[3] vpn[2] vpn[1] vpn[0] offset
    // f_vpnl2l3 : vpn[3]   0      0      0       0
    // f_vpnl2l2 : vpn[3] vpn[2]   0      0       0
    // f_vpnl2l1 : vpn[3] vpn[2] vpn[1]   0       0
    // f_vpnl2l0 : vpn[3] vpn[2] vpn[1] vpn[0]    0
    tem = vpn >> PageShift;
    Addr f_vpnl2l3 = (tem >> 3 * LEVEL_BITS) << (PageShift + 3 * LEVEL_BITS);
    Addr f_vpnl2l2 = (tem >> 2 * LEVEL_BITS) << (PageShift + 2 * LEVEL_BITS);
    Addr f_vpnl2l1 = (tem >> 1 * LEVEL_BITS) << (PageShift + 1 * LEVEL_BITS);
    Addr f_vpnl2l0 = (tem >> 0 * LEVEL_BITS) << (PageShift + 0 * LEVEL_BITS);
    DPRINTF(TLB, "f_vpnl2l3 %#x f_vpnl2l2 %#x f_vpnl2l1 %#x f_vpnl2l0 %#x vpn %#x\n",
            f_vpnl2l3, f_vpnl2l2, f_vpnl2l1, f_vpnl2l0, vpn);

    tem = vpn >> (PageShift + L2TLB_BLK_OFFSET);
    Addr vpnl2l3 = (tem >> 3 * LEVEL_BITS) << (PageShift + 3 * LEVEL_BITS + L2TLB_BLK_OFFSET);
    Addr vpnl2l2 = (tem >> 2 * LEVEL_BITS) << (PageShift + 2 * LEVEL_BITS + L2TLB_BLK_OFFSET);
    Addr vpnl2l1 = (tem >> 1 * LEVEL_BITS) << (PageShift + 1 * LEVEL_BITS + L2TLB_BLK_OFFSET);
    Addr vpnl2l0 = (tem >> 0 * LEVEL_BITS) << (PageShift + 0 * LEVEL_BITS + L2TLB_BLK_OFFSET);

    Addr vpnl2sp3, vpnl2sp2, vpnl2sp1;

    vpnl2sp3 = vpnl2l3, vpnl2sp2 = vpnl2l2, vpnl2sp1 = vpnl2l1;

    Addr step;
    int i;

    TlbEntry *entry_l2 = nullptr;

    if (is_L1tlb && !isStage2)
        panic("wrong in tlb config\n");

    if (f_level == L_L2L3) {
        DPRINTF(TLB, "look up l2tlb in l2l3\n");
        TlbEntry *entry_l2l3 = trieL2L3.lookup(buildKey(f_vpnl2l3, asid, translateMode));
        entry_l2 = entry_l2l3;
        step = 0x1ll << (PageShift + 3 * LEVEL_BITS);
        if ((!hidden) && (entry_l2l3))
            updateL2TLBSeq(&trieL2L3, vpnl2l3, step, asid, translateMode);
    }
    if (f_level == L_L2L2) {
        DPRINTF(TLB, "look up l2tlb in l2l2 key %#x\n", buildKey(f_vpnl2l2, asid, translateMode));
        TlbEntry *entry_l2l2 = trieL2L2.lookup(buildKey(f_vpnl2l2, asid, translateMode));
        entry_l2 = entry_l2l2;
        step = 0x1ll << (PageShift + 2 * LEVEL_BITS);
        if ((!hidden) && (entry_l2l2))
            updateL2TLBSeq(&trieL2L2, vpnl2l2, step, asid, translateMode);
    }
    if (f_level == L_L2L1) {
        DPRINTF(TLB, "look up l2tlb in l2l1\n");
        TlbEntry *entry_l2l1 = trieL2L1.lookup(buildKey(f_vpnl2l1, asid, translateMode));
        entry_l2 = entry_l2l1;
        step = 0x1ll << (PageShift + 1 * LEVEL_BITS);
        if ((!hidden) && (entry_l2l1))
            updateL2TLBSeq(&trieL2L1, vpnl2l1, step, asid, translateMode);
    }
    if (f_level == L_L2L0) {
        DPRINTF(TLB, "look up l2tlb in l2l0\n");
        TlbEntry *entry_l2l0 = trieL2L0.lookup(buildKey(vpn, asid, translateMode));
        entry_l2 = entry_l2l0;
        step = 0x1000;
        bool write_sign = false;
        if (entry_l2l0) {
            if (sign_used) {
                if (entry_l2l0->isPre && (!entry_l2l0->preSign)) {
                    write_sign = true;
                    stats.hitPreEntry++;
                    hitPreEntry++;
                }
                if (entry_l2l0->isPre) {
                    stats.hitPreNum++;
                    hitPreNum++;
                }
            }
            for (i = 0; i < l2tlbLineSize; i++) {
                TlbEntry *m_entry_l2l0 = trieL2L0.lookup(buildKey((vpnl2l0 + step * i), asid, translateMode));
                if (m_entry_l2l0 == nullptr) {
                    DPRINTF(TLB, "l2l0 vaddr basic %#x vaddr %#x\n", vpnl2l0, vpnl2l0 + step * i);
                    panic("l2l0 TLB link num is empty\n");
                }
                if (!hidden)
                    m_entry_l2l0->lruSeq = nextSeq();
                if (write_sign)
                    m_entry_l2l0->preSign = true;
            }
            if (!hidden) {
                if (mode == BaseMMU::Write) {
                    stats.writeL2Tlbl0Hits++;
                } else {
                    stats.ReadL2Tlbl0Hits++;
                }
                if (entry_l2l0->isSquashed) {
                    if (mode == BaseMMU::Write) {
                        stats.writeL2l0TlbSquashedHits++;
                    } else {
                        stats.ReadL2l0TlbSquashedHits++;
                    }
                }
            }
        }
    }
    if (f_level == L_L2sp3) {
        DPRINTF(TLB, "look up l2tlb in l2sp3\n");
        TlbEntry *entry_l2sp3 = trieL2sp.lookup(buildKey(f_vpnl2l3, asid, translateMode));
        entry_l2 = entry_l2sp3;
        step = 0x1ll << (PageShift + 3 * LEVEL_BITS);
        if (entry_l2sp3) {
            if (entry_l2sp3->level == L2L2CheckLevel) {
                DPRINTF(TLB, "hit in sp but sp2 , return\n");
                return nullptr;
            }
            if (entry_l2sp3->level == L2L1CheckLevel) {
                DPRINTF(TLB, "hit in sp but sp1 , return\n");
                return nullptr;
            }
            if (!hidden)
                updateL2TLBSeq(&trieL2sp, vpnl2sp3, step, asid, translateMode);
        }
    }
    if (f_level == L_L2sp2) {
        DPRINTF(TLB, "look up l2tlb in l2sp2\n");
        TlbEntry *entry_l2sp2 = trieL2sp.lookup(buildKey(f_vpnl2l2, asid, translateMode));
        entry_l2 = entry_l2sp2;
        step = 0x1ll << (PageShift + 2 * LEVEL_BITS);
        if (entry_l2sp2) {
            if (entry_l2sp2->level == L2L3CheckLevel) {
                DPRINTF(TLB, "hit in sp but sp3 , return\n");
                return nullptr;
            }
            if (entry_l2sp2->level == L2L1CheckLevel) {
                DPRINTF(TLB, "hit in sp but sp1 , return\n");
                return nullptr;
            }
            if (!hidden)
                updateL2TLBSeq(&trieL2sp, vpnl2sp2, step, asid, translateMode);
        }
    }
    if (f_level == L_L2sp1) {
        DPRINTF(TLB, "look up l2tlb in l2sp1\n");
        TlbEntry *entry_l2sp1 = trieL2sp.lookup(buildKey(f_vpnl2l1, asid, translateMode));
        entry_l2 = entry_l2sp1;
        step = 0x1ll << (PageShift + LEVEL_BITS);
        if (entry_l2sp1) {
            if (entry_l2sp1->level == L2L3CheckLevel) {
                DPRINTF(TLB, "hit in sp but sp3 , return\n");
                return nullptr;
            }
            if (entry_l2sp1->level == L2L2CheckLevel) {
                DPRINTF(TLB, "hit in sp but sp2 , return\n");
                return nullptr;
            }
            if (!hidden)
                updateL2TLBSeq(&trieL2sp, vpnl2sp1, step, asid, translateMode);
        }
    }

    if (sign_used) {
        if (entry_l2)
            entry_l2->used = true;
    }

    return entry_l2;
}

TlbEntry *
TLB::insert(Addr vpn, const TlbEntry &entry,bool squashed_update,uint8_t translateMode)
{
    DPRINTF(TLBGPre, "insert(vpn=%#x, asid=%#x): ppn=%#x pte=%#x size=%#x\n",
            vpn, translateMode == gstage ? entry.vmid : entry.asid, entry.paddr, entry.pte, entry.size());

    // If somebody beat us to it, just use that existing entry.
    TlbEntry *newEntry = nullptr;
    if (translateMode == gstage)
        newEntry = lookup(vpn, entry.vmid, BaseMMU::Read, true, false, translateMode);
    else
        newEntry = lookup(vpn, entry.asid, BaseMMU::Read, true, false, translateMode);

    if (squashed_update) {
        if (newEntry) {
            if (newEntry->isSquashed) {
                return newEntry;
            }
            // update isSquashed flag
            newEntry->isSquashed = entry.isSquashed;
            stats.squashedInsert++;

        } else {
            DPRINTF(TLBVerbosel2, "update isSquashed flag but no entry\n");
        }
        return newEntry;
    }
    if (newEntry) {
        // update PTE flags (maybe we set the dirty/writable flag)
        newEntry->pte = entry.pte;
        Addr newEntryAddr = buildKey(newEntry->vaddr, newEntry->asid, translateMode);
        Addr vpnAddr = buildKey(entry.vaddr, entry.asid, translateMode);
        if (newEntry->vaddr != vpn) {
            DPRINTF(TLBGPre, "tlb in newEntryAddr %#x vpnAddr %#x\n", newEntryAddr, vpnAddr);
            DPRINTF(TLBGPre, "l1 tlb insert(vpn=%#x, vpn2 %#x asid=%#x): ppn=%#x pte=%#x size=%#x\n", vpn, entry.vaddr,
                    entry.asid, entry.paddr, entry.pte, entry.size());
            DPRINTF(TLBGPre, "l1 newentry(vpn=%#x, vpn2 %#x asid=%#x): ppn=%#x pte=%#x size=%#x \n", vpn,
                    newEntry->vaddr, newEntry->asid, newEntry->paddr, newEntry->pte, newEntry->size());
            DPRINTF(TLBGPre, "l1 newEntry->vaddr %#x vpn %#x \n", newEntry->vaddr, vpn);
        }

        return newEntry;
    }

    if (freeList.empty())
        evictLRU();

    newEntry = freeList.front();
    freeList.pop_front();

    Addr key = buildKey(vpn, entry.asid, translateMode);
    if (translateMode == gstage)
        key = buildKey(vpn, entry.vmid, translateMode);
    *newEntry = entry;
    newEntry->lruSeq = nextSeq();
    newEntry->vaddr = vpn;
    newEntry->trieHandle = trie.insert(key, TlbEntryTrie::MaxBits - entry.logBytes + PGSHFT, newEntry);
    DPRINTF(TLBVerbosel2, "trie insert key %#x logbytes %#x paddr %#x\n", key,
            entry.logBytes, newEntry->paddr);
    // stats all insert number
    stats.ALLInsert++;
    allUsed++;
    return newEntry;
}

TlbEntry *
TLB::insertForwardPre(Addr vpn, const TlbEntry &entry)
{
    TlbEntry *newEntry = lookupForwardPre(vpn, entry.asid, true);
    if (newEntry)
        return newEntry;
    if (freeListForwardPre.empty()) {
        evictForwardPre();
    }
    newEntry = freeListForwardPre.front();
    freeListForwardPre.pop_front();

    Addr key = buildKey(vpn, entry.asid, 0);
    *newEntry = entry;
    newEntry->lruSeq = nextSeq();
    newEntry->vaddr = vpn;
    newEntry->used = false;
    newEntry->trieHandle = trieForwardPre.insert(key, TlbEntryTrie::MaxBits - entry.logBytes + PGSHFT, newEntry);
    allForwardPre++;
    return newEntry;
}

TlbEntry *
TLB::insertBackPre(Addr vpn, const TlbEntry &entry)
{
    TlbEntry *newEntry = lookupBackPre(vpn, entry.asid, true);
    if (newEntry)
        return newEntry;
    if (freeListBackPre.empty()) {
        evictBackPre();
    }
    newEntry = freeListBackPre.front();
    freeListBackPre.pop_front();

    Addr key = buildKey(vpn, entry.asid, 0);
    *newEntry = entry;
    newEntry->lruSeq = nextSeq();
    newEntry->vaddr = vpn;
    newEntry->used = false;
    newEntry->trieHandle = trieBackPre.insert(key, TlbEntryTrie::MaxBits - entry.logBytes + PGSHFT, newEntry);
    return newEntry;
}

TlbEntry *
TLB::L2TLBInsertIn(Addr vpn, const TlbEntry &entry, int choose, EntryList *List, TlbEntryTrie *Trie_l2, int sign,
                   bool squashed_update, uint8_t translateMode)
{
    if (!List || !Trie_l2)
        panic("L2TLBInsertIn: List or Trie_l2 should not be 0\n");

    DPRINTF(TLB,
            "l2tlb insert(vpn=%#x, entry.vaddr %#x asid=%#x): ppn=%#x pte=%#x "
            "size=%#x choose %d\n",
            vpn, entry.vaddr, translateMode == gstage ? entry.vmid : entry.asid,
            entry.paddr, entry.pte, entry.size(), choose);
    TlbEntry *newEntry;
    Addr key;
    if (translateMode == gstage)
        newEntry = lookupL2TLB(vpn, entry.vmid, BaseMMU::Read, true, choose, false, translateMode);
    else
        newEntry = lookupL2TLB(vpn, entry.asid, BaseMMU::Read, true, choose, false, translateMode);

    Addr step = 0;
    if ((choose == L_L2L3) || (choose == L_L2sp3)) {
        step = 0x1ll << (PageShift + 3 * LEVEL_BITS);
    } else if ((choose == L_L2L2) || (choose == L_L2sp2)) {
        step = 0x1ll << (PageShift + 2 * LEVEL_BITS);
    } else if ((choose == L_L2L1) || (choose == L_L2sp1)) {
        step = 0x1ll << (PageShift + 1 * LEVEL_BITS);
    } else if (choose == L_L2L0) {
        step = 0x1ll << (PageShift + 0 * LEVEL_BITS);
    }

    if (squashed_update) {
        if (newEntry) {
            if (newEntry->isSquashed) {
                return newEntry;
            }
            newEntry->isSquashed = true;
            stats.squashedInsertL2++;
            for (int i = 1; i < l2tlbLineSize; i++) {
                if (translateMode == gstage) {
                    newEntry =
                        lookupL2TLB(vpn + step * i, entry.vmid, BaseMMU::Read, true, choose, false, translateMode);
                } else {
                    newEntry =
                        lookupL2TLB(vpn + step * i, entry.asid, BaseMMU::Read, true, choose, false, translateMode);
                }
                stats.squashedInsertL2++;
                if (newEntry) {
                    newEntry->isSquashed = true;
                }
            }
        }
        return newEntry;
    }
    if (newEntry) {
        newEntry->pte = entry.pte;
        if (newEntry->vaddr != vpn) {
            Addr newEntryAddr = ((buildKey(newEntry->vaddr, newEntry->asid, translateMode) >> 12) << 12);
            Addr vpnAddr = ((buildKey(entry.vaddr, entry.asid, translateMode) >> 12) << 12);
            Addr vpngpaddr = ((buildKey(entry.gpaddr, entry.vmid, translateMode) >> 12) << 12);
            warn("vaddr newEntry vaddr %lx vpn %lx key newEntry vaddr %lx vpn %lx vpngpaddr %lx\n", newEntry->vaddr,
                 vpn, newEntryAddr, vpnAddr, vpngpaddr);

            DPRINTF(TLBVerbosel2, "newEntryAddr %#x vpnAddr %#x\n", newEntryAddr, vpnAddr);
            DPRINTF(TLBVerbosel2, "l2tlb insert(vpn=%#x, vpn2 %#x asid=%#x): ppn=%#x pte=%#x size=%#x level %d\n", vpn,
                    entry.vaddr, entry.asid, entry.paddr, entry.pte, entry.size(), choose);
            DPRINTF(TLBVerbosel2, "newentry(vpn=%#x, vpn2 %#x asid=%#x): ppn=%#x pte=%#x size=%#x level %d\n", vpn,
                    newEntry->vaddr, newEntry->asid, newEntry->paddr, newEntry->pte, newEntry->size(), choose);
            DPRINTF(TLBVerbosel2, "newEntry->vaddr %#x vpn %#x choose %d\n", newEntry->vaddr, vpn, choose);
            if ((newEntryAddr != vpnAddr) && (newEntryAddr != vpngpaddr))
                panic("tlb match but key is wrong\n");
        }
        return newEntry;
    }
    DPRINTF(TLB, "not hit in l2 tlb\n");
    if ((choose == L_L2L1 || choose == L_L2L0) && (sign == 0)) {
        DPRINTF(TLB, "choose %d sign %d\n", choose, sign);
        l2TLBEvictLRU(choose, vpn);
    } else {
        if ((*List).empty())
            l2TLBEvictLRU(choose, vpn);
        else
            DPRINTF(TLB, "eviction is not needed, tlb still have entries.\n");
    }

    if ((*List).empty())
        panic("TLB::L2TLBInsertIn freeList should not be empty.");

    newEntry = (*List).front();
    (*List).pop_front();

    key = buildKey(vpn, entry.asid, translateMode);
    if (translateMode == gstage)
        key = buildKey(vpn, entry.vmid, translateMode);

    if (newEntry == nullptr)
        panic("TLB::L2TLBInsertIn newEntry should not be nullptr.");

    *newEntry = entry;
    newEntry->lruSeq = nextSeq();
    newEntry->vaddr = vpn;
    if (entry.paddr == 0) {
        DPRINTF(TLB, " l2tlb num is outside vaddr %#x paddr %#x \n",
                entry.vaddr, entry.paddr);
    }

    newEntry->trieHandle = (*Trie_l2).insert(
        key, TlbEntryTrie::MaxBits - entry.logBytes + PGSHFT, newEntry);

    DPRINTF(TLB, "l2tlb trie insert key %#x logbytes %#x len %#x\n", key,
            entry.logBytes,TlbEntryTrie::MaxBits - entry.logBytes + PGSHFT);
    stats.ALLInsertL2++;
    if (choose == L_L2L0)
        allUsed++;
    if (entry.isPre) {
        stats.AllPre++;
        AllPre++;
    }

    return newEntry;

}

TlbEntry *
TLB::L2TLBInsert(Addr vpn, const TlbEntry &entry, int level, int choose, int sign, bool squashed_update,
                 uint8_t translateMode)
{
    TLB *l2tlb;
    if (isStage2) {
        l2tlb = this;
    } else {
        l2tlb = static_cast<TLB *>(nextLevel());
    }

    TlbEntry *newEntry = nullptr;
    DPRINTF(TLB, "choose %d vpn %#x entry->vaddr %#x\n", choose, vpn, entry.vaddr);
    newEntry = l2tlb->L2TLBInsertIn(vpn, entry, choose, l2tlb->l2Freelist[choose - 1], l2tlb->l2Trie[choose - 1], sign,
                                    squashed_update, translateMode);

    if (!squashed_update) {
        assert(newEntry != nullptr);
    }

    return newEntry;
}

void
TLB::demapPage(Addr vpn, uint64_t asid)
{
    DPRINTF(TLBGPre, "flush(vpn=%#x, asid=%#x)\n", vpn, asid);
    asid &= 0xFFFF;

    size_t i;

    TLB *l2tlb;
    if (isStage2) {
        l2tlb = this;
    } else {
        l2tlb = static_cast<TLB *>(nextLevel());
    }

    if ((l2tlb == nullptr) && (!isStage2))
        panic("l2tlb is fault\n");

    if (vpn == 0 && asid == 0) {
        flushAll();
        if (!isStage2) {
            l2tlb->flushAll();
        }

    }

    else {
        DPRINTF(TLB, "flush(vpn=%#x, asid=%#x)\n", vpn, asid);
        DPRINTF(TLB, "l1tlb flush(vpn=%#x, asid=%#x)\n", vpn, asid);
        if (vpn != 0 && asid != 0) {
            for (uint8_t i = 0; i < 4; i++) {
                TlbEntry *newEntry = lookup(vpn, asid, BaseMMU::Read, true, false, i);
                if (newEntry)
                    remove(newEntry - tlb.data());
                l2tlb->demapPageL2(vpn, asid);
            }
        } else {
            for (i = 0; i < size; i++) {
                if (tlb[i].trieHandle) {
                    Addr mask = ~(tlb[i].size() - 1);
                    if ((vpn == 0 || (vpn & mask) == (tlb[i].vaddr & mask)) && (asid == 0 || tlb[i].asid == asid))
                        remove(i);
                }
                if (tlb[i].trieHandle) {
                    Addr mask = ~(tlb[i].size() - 1);
                    if ((vpn == 0 || (vpn & mask) == (tlb[i].gpaddr & mask)) && (asid == 0 || tlb[i].vmid == asid))
                        remove(i);
                }
            }
            l2tlb->demapPageL2(vpn, asid);
        }
    }
}

void
TLB::demapPageL2(Addr vpn, uint64_t asid)
{
    asid &= 0xFFFF;
    std::vector<Addr> vpn_vec;
    Addr vpnl2l3 = (vpn >> (PageShift + 3 * LEVEL_BITS + L2TLB_BLK_OFFSET))
                   << (PageShift + 3 * LEVEL_BITS + L2TLB_BLK_OFFSET);
    Addr vpnl2l2 = (vpn >> (PageShift + 2 * LEVEL_BITS + L2TLB_BLK_OFFSET))
                   << (PageShift + 2 * LEVEL_BITS + L2TLB_BLK_OFFSET);
    Addr vpnl2l1 = (vpn >> (PageShift + LEVEL_BITS + L2TLB_BLK_OFFSET)) << (PageShift + LEVEL_BITS + L2TLB_BLK_OFFSET);
    Addr vpnl2l0 = (vpn >> (PageShift + L2TLB_BLK_OFFSET)) << (PageShift + L2TLB_BLK_OFFSET);
    Addr vpnl2sp3 = (vpn >> (PageShift + 3 * LEVEL_BITS + L2TLB_BLK_OFFSET))
                    << (PageShift + 3 * LEVEL_BITS + L2TLB_BLK_OFFSET);
    Addr vpnl2sp2 = (vpn >> (PageShift + 2 * LEVEL_BITS + L2TLB_BLK_OFFSET))
                    << (PageShift + 2 * LEVEL_BITS + L2TLB_BLK_OFFSET);
    Addr vpnl2sp1 = (vpn >> (PageShift + LEVEL_BITS + L2TLB_BLK_OFFSET))
                    << (PageShift + LEVEL_BITS + L2TLB_BLK_OFFSET);
    vpn_vec.push_back(vpnl2l3);
    vpn_vec.push_back(vpnl2l2);
    vpn_vec.push_back(vpnl2l1);
    vpn_vec.push_back(vpnl2l0);
    vpn_vec.push_back(vpnl2sp3);
    vpn_vec.push_back(vpnl2sp2);
    vpn_vec.push_back(vpnl2sp1);
    int i;

    DPRINTF(TLB, "l2 flush(vpn=%#x, asid=%#x)\n", vpn, asid);
    DPRINTF(TLBVerbose3, "l2tlb flush(vpn=%#x, asid=%#x)\n", vpn, asid);

    TlbEntry *l2_newEntry[L_L2SUM]  = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    TlbEntry *l2_newEntry1[L_L2SUM] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    TlbEntry *l2_newEntry2[L_L2SUM] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};

    for (int ii = 1; ii < L_L2SUM; ii++) {
        l2_newEntry[ii] = lookupL2TLB(vpn_vec[ii - 1], asid, BaseMMU::Read, true, ii, false, direct);
    }
    for (int ii = 1; ii < L_L2SUM; ii++) {
        l2_newEntry1[ii] = lookupL2TLB(vpn_vec[ii - 1], asid, BaseMMU::Read, true, ii, true, gstage);
    }
    for (int ii = 1; ii < L_L2SUM; ii++) {
        l2_newEntry2[ii] = lookupL2TLB(vpn_vec[ii - 1], asid, BaseMMU::Read, true, ii, true, vsstage);
    }



    if (vpn != 0 && asid != 0) {
        if (isStage2 || isTheSharedL2) {
            for (i = 1; i < L_L2SUM; i++) {
                int tlb_i = 0;
                if (i - 1 > L_L2L0)
                    tlb_i = L_L2L0;
                else
                    tlb_i = i - 1;
                if (l2_newEntry[i]) {
                    TlbEntry *m_newEntry = lookupL2TLB(vpn_vec[i - 1], asid, BaseMMU::Read, true, i, false, direct);
                    assert(m_newEntry != nullptr);
                    l2TLBRemove(m_newEntry - l2Tlb[tlb_i], i);
                }
                if (l2_newEntry1[i]) {
                    TlbEntry *m_newEntry = lookupL2TLB(vpn_vec[i - 1], asid, BaseMMU::Read, true, i, true, gstage);
                    assert(m_newEntry != nullptr);
                    l2TLBRemove(m_newEntry - l2Tlb[tlb_i], i);
                }
                if (l2_newEntry2[i]) {
                    TlbEntry *m_newEntry = lookupL2TLB(vpn_vec[i - 1], asid, BaseMMU::Read, true, i, true, vsstage);
                    assert(m_newEntry != nullptr);
                    l2TLBRemove(m_newEntry - l2Tlb[tlb_i], i);
                }
            }
        }
    } else {
        if (isStage2 || isTheSharedL2) {
            for (int i_type = 0; i_type < L2PageTypeNum; i_type++) {
                for (i = 0; i < l2TlbSize[i_type] * l2tlbLineSize; i = i + l2tlbLineSize) {
                    if ((l2Tlb[i_type] + i)->trieHandle) {
                        l2TLBRemove(i, i_type + 1);
                    }
                }
            }
        }
        for (int i_type = 0; i_type < L2PageTypeNum; i_type++) {
            for (i = 0; i < l2TlbSize[i_type] * l2tlbLineSize; i = i + l2tlbLineSize) {
                Addr mask = ~((l2Tlb[i_type] + i)->size() - 1);
                if ((l2Tlb[i_type] + i)->trieHandle) {
                    if ((vpn_vec[i_type] == 0 || (vpn_vec[i_type] & mask) == ((l2Tlb[i_type] + i)->vaddr & mask)) &&
                        (asid == 0 || (l2Tlb[i_type] + i)->asid == asid)) {
                        l2TLBRemove(i, i_type + 1);
                    }
                }
                if ((l2Tlb[i_type] + i)->trieHandle) {
                    if ((vpn_vec[i_type] == 0 ||
                         (vpn_vec[i_type] & mask) == ((l2Tlb[i_type] + i)->gpaddr & mask)) &&
                        (asid == 0 || (l2Tlb[i_type] + i)->vmid == asid)) {
                        l2TLBRemove(i, i_type + 1);
                    }
                }
            }
        }
    }
}

void
TLB::flushAll()
{
    size_t i;
    if (is_L1tlb) {
        for (i = 0; i < size; i++) {
            if (tlb[i].trieHandle)
                remove(i);
        }
    }
    if (isStage2 || isTheSharedL2) {
        for (int i_type = 0; i_type < L2PageTypeNum; i_type++) {
            for (i = 0; i < l2TlbSize[i_type] * l2tlbLineSize; i = i + l2tlbLineSize) {
                if ((l2Tlb[i_type] + i)->trieHandle) {
                    l2TLBRemove(i, i_type + 1);
                }
            }
        }
    }
}

void
TLB::remove(size_t idx)
{
    assert(tlb[idx].trieHandle);
    if (tlb[idx].used) {
        stats.l1tlbUsedRemove++;
    } else {
        stats.l1tlbUnusedRemove++;
    }
    trie.remove(tlb[idx].trieHandle);
    tlb[idx].trieHandle = nullptr;
    freeList.push_back(&tlb[idx]);
    stats.l1tlbRemove++;
}

void
TLB::removeForwardPre(size_t idx)
{
    assert(forwardPre[idx].trieHandle);
    if (!forwardPre[idx].used) {
        removeNoUseForwardPre++;
        stats.removeNoUseForwardPre++;
    } else {
        forwardUsedPre++;
        stats.usedForwardPre++;
    }
    trieForwardPre.remove(forwardPre[idx].trieHandle);
    forwardPre[idx].trieHandle = nullptr;
    freeListForwardPre.push_back(&forwardPre[idx]);
}

void
TLB::removeBackPre(size_t idx)
{
    assert(backPre[idx].trieHandle);
    if (!backPre[idx].used) {
        removeNoUseBackPre++;
        stats.removeNoUseBackPre++;
    } else {
        usedBackPre++;
        stats.usedBackPre++;
    }
    trieBackPre.remove(backPre[idx].trieHandle);
    backPre[idx].trieHandle = nullptr;
    freeListBackPre.push_back(&backPre[idx]);
}
void
TLB::l2tlbRemoveIn(EntryList *List, TlbEntryTrie *Trie_l2, std::vector<TlbEntry> &tlb, size_t idx, int choose)
{
    DPRINTF(TLB, "remove tlb %d idx %d\n", choose, idx);
    DPRINTF(TLB, "remove tlb (vpn=%#x, asid=%#x): ppn=%#x pte=%#x size=%#x\n", tlb[idx].vaddr, tlb[idx].asid,
            tlb[idx].paddr, tlb[idx].pte, tlb[idx].size());
    assert(tlb[idx].trieHandle);
    (*Trie_l2).remove(tlb[idx].trieHandle);
    tlb[idx].trieHandle = nullptr;
    (*List).push_back(&tlb[idx]);
}

void
TLB::l2TLBRemove(size_t idx, int choose)
{
    stats.l2tlbRemove[choose]++;
    if ((l2Tlb[choose - 1] + idx)->used){
        stats.l2tlbUsedRemove[choose]++;
    } else{
        stats.l2tlbUnusedRemove[choose]++;
    }
    for (int i = 0; i < l2tlbLineSize; i++) {
        DPRINTF(TLB, "remove l2_tlb level %d idx %d idx+i %d\n", choose - 1, idx, idx + i);
        DPRINTF(TLB, "remove tlb %d idx %d\n", choose, idx);
        DPRINTF(TLB, "remove tlb (vpn=%#x, asid=%#x): ppn=%#x pte=%#x size=%#x\n",
                (l2Tlb[choose - 1] + idx + i)->vaddr, (l2Tlb[choose - 1] + idx + i)->asid,
                (l2Tlb[choose - 1] + idx + i)->paddr, (l2Tlb[choose - 1] + idx + i)->pte,
                (l2Tlb[choose - 1] + idx + i)->size());
        assert((l2Tlb[choose - 1] + idx + i)->trieHandle);
        (*l2Trie[choose - 1]).remove((l2Tlb[choose - 1] + idx + i)->trieHandle);
        (l2Tlb[choose - 1] + idx + i)->trieHandle = nullptr;
        (*l2Freelist[choose - 1]).push_back((l2Tlb[choose - 1] + idx + i));
    }

}

Fault
TLB::checkPermissions(STATUS status, PrivilegeMode pmode, Addr vaddr,
                      BaseMMU::Mode mode, PTE pte,Addr gpaddr,bool G)
{
    Fault fault = NoFault;

    if (mode == BaseMMU::Read && !pte.r) {
        fault = createPagefault(vaddr, gpaddr, mode, G);
    } else if (mode == BaseMMU::Write && !pte.w) {
        fault = createPagefault(vaddr, gpaddr, mode, G);
    } else if (mode == BaseMMU::Execute && !pte.x) {
        fault = createPagefault(vaddr, gpaddr, mode, G);
    }

    if (fault == NoFault) {
        if (pmode == PrivilegeMode::PRV_U && !pte.u) {
            fault = createPagefault(vaddr, gpaddr, mode, G);
        } else if (pmode == PrivilegeMode::PRV_S && pte.u && (status.sum == 0)) {
            fault = createPagefault(vaddr, gpaddr, mode, G);
        }
    }

    return fault;
}

std::pair<bool, Fault>
TLB::checkGPermissions(STATUS status,Addr vaddr,Addr gpaddr,BaseMMU::Mode mode, PTE pte,bool h_inst){
    bool continuePtw = false;
    if (pte.v && !pte.r && !pte.w && !pte.x) {
        return std::make_pair(true,NoFault);
    } else if (!pte.v || (!pte.r && pte.w)) {
        return std::make_pair(continuePtw,createPagefault(vaddr, gpaddr, mode, true));
    } else if (!pte.u) {
        return std::make_pair(continuePtw,createPagefault(vaddr, gpaddr, mode, true));
    } else if (((mode == BaseMMU::Execute) || (h_inst)) && (!pte.x)) {
        return std::make_pair(continuePtw,createPagefault(vaddr, gpaddr, mode, true));
    } else if ((mode == BaseMMU::Read) && (!pte.r && !(status.mxr && pte.x))) {
        return std::make_pair(continuePtw,createPagefault(vaddr, gpaddr, mode, true));
    } else if ((mode == BaseMMU::Write) && !(pte.r && pte.w)) {
        return std::make_pair(continuePtw,createPagefault(vaddr, gpaddr, mode, true));
    }
    return std::make_pair(continuePtw,NoFault);
}

Fault
TLB::createPagefault(Addr vaddr, Addr gPaddr,BaseMMU::Mode mode,bool G)
{
    ExceptionCode code;
    if (G) {
        if (mode == BaseMMU::Read) {
            code = ExceptionCode::LOAD_G_PAGE;
        } else if (mode == BaseMMU::Write) {
            code = ExceptionCode::STORE_G_PAGE;
        } else {
            code = ExceptionCode::INST_G_PAGE;
        }
    } else {
        if (mode == BaseMMU::Read) {
            code = ExceptionCode::LOAD_PAGE;
        } else if (mode == BaseMMU::Write) {
            code = ExceptionCode::STORE_PAGE;
        } else {
            code = ExceptionCode::INST_PAGE;
        }
    }

    DPRINTF(TLB, "Create page fault #%i on %#lx\n", code, vaddr);
    return std::make_shared<AddressFault>(vaddr, gPaddr, code);
}

Addr
TLB::translateWithTLB(Addr vaddr, uint16_t asid, BaseMMU::Mode mode, uint8_t translateMode)
{
    TlbEntry *e = lookup(vaddr, asid, mode, false, false, translateMode);
    DPRINTF(TLB, "translateWithTLB vaddr %#x \n", vaddr);
    assert(e != nullptr);
    DPRINTF(TLBGPre, "translateWithTLB vaddr %#x paddr %#x\n", vaddr,
            e->paddr << PageShift | (vaddr & mask(e->logBytes)));
    return (e->paddr << PageShift) | (vaddr & mask(e->logBytes));
}
Fault
TLB::L2TLBPagefault(Addr vaddr, BaseMMU::Mode mode, const RequestPtr &req, bool isPre, bool is_back_pre)
{
    if (req->isInstFetch()) {
        Addr page_l2_start = (vaddr >> 12) << 12;
        DPRINTF(TLBVerbosel2, "vaddr %#x,req_pc %#x,page_l2_start %#x\n",
                vaddr, req->getPC(), page_l2_start);
        if (req->getPC() < page_l2_start) {
            DPRINTF(TLBVerbosel2, "vaddr %#x,req_pc %#x,page_l2_start %#x\n",
                    vaddr, req->getPC(), page_l2_start);
            return createPagefault(page_l2_start, 0, mode, false);
        }
        return createPagefault(req->getPC(), 0, mode, false);
    } else {
        DPRINTF(TLBVerbosel2, "vaddr 2 %#x,req_pc %#x,get vaddr %#x\n", vaddr, req->getPC(), req->getVaddr());
        if (is_back_pre)
            return createPagefault(req->getBackPreVaddr(), 0, mode, false);
        else if (isPre)
            return createPagefault(req->getForwardPreVaddr(), 0, mode, false);
        else
            return createPagefault(req->getVaddr(), 0, mode, false);
    }
}

Fault
TLB::L2TLBCheck(PTE pte, int level, STATUS status, PrivilegeMode pmode, Addr vaddr, BaseMMU::Mode mode,
                const RequestPtr &req, bool isPre, bool is_back_pre)
{
    Fault fault = NoFault;
    hitInSp = false;
    DPRINTF(TLB, "L2TLBCheck level: %d , pte:%#x, pte.v:%d, pte.r:%d, pte.w:%d, ppn0:%#x, ppn1:%#x, ppn2:%#x\n",
            level, pte, pte.v, pte.r, pte.w, pte.ppn0, pte.ppn1, pte.ppn2);
    if (isPre) {
        DPRINTF(TLBGPre, "l2tlb_check paddr %#x vaddr %#x pte %#x\n", pte.ppn,
                vaddr, pte);
    }

    if (!pte.v || (!pte.r && pte.w)) {
        hitInSp = true;
        fault = L2TLBPagefault(vaddr, mode, req, isPre, is_back_pre);

    } else {
        if (pte.r || pte.x) {
            hitInSp = true;
            fault = checkPermissions(status, pmode, vaddr, mode, pte, 0, false);
            if (fault == NoFault) {
                if (level >= L2L1CheckLevel && pte.ppn0 != 0) {
                    fault = L2TLBPagefault(vaddr, mode, req, isPre, is_back_pre);
                } else if (level >= L2L2CheckLevel && pte.ppn1 != 0) {
                    fault = L2TLBPagefault(vaddr, mode, req, isPre, is_back_pre);
                } else if (level == L2L3CheckLevel && pte.ppn2 != 0) {
                    fault = L2TLBPagefault(vaddr, mode, req, isPre, is_back_pre);
                }
            }

            if (fault == NoFault) {
                if (!pte.a) {
                    fault = L2TLBPagefault(vaddr, mode, req, isPre, is_back_pre);
                }
                if (!pte.d && mode == BaseMMU::Write) {
                    fault = L2TLBPagefault(vaddr, mode, req, isPre, is_back_pre);
                }
            }

        } else {
            level--;
            if (level < 0) {
                hitInSp = true;
                fault = L2TLBPagefault(vaddr, mode, req, isPre, is_back_pre);
            } else {
                hitInSp = false;
            }
        }
    }
    DPRINTF(TLB, "tlb check final\n");
    if (fault == NoFault)
        DPRINTF(TLB, "the result is nofault\n");
    else
        DPRINTF(TLB, "the result is fault for some reason\n");
    if (fault != NoFault)
    {
        DPRINTF(TLBVerbose3, "hit in l2 vaddr is %#x\n", vaddr);
    }
    return fault;
}
bool
TLB::checkPrePrecision(uint64_t &removeNoUsePre, uint64_t &usedPre)
{
    bool prePrecision = false;
    if ((removeNoUsePre + removeNoUsePre) > preHitOnHitLNum) {
        if ((((double)(removeNoUsePre + 1) / (removeNoUsePre + usedPre))) < preHitOnHitPrecision) {
            prePrecision = true;
        } else {
            prePrecision = false;
        }
        removeNoUsePre = 0;
        usedPre = 0;
    }
    return prePrecision;
}
void
TLB::sendPreHitOnHitRequest(TlbEntry *e_pre_1, TlbEntry *e_pre_2, const RequestPtr &req, Addr pre_block, uint16_t asid,
                            bool forward, int check_level, STATUS status, PrivilegeMode pmode, BaseMMU::Mode mode,
                            ThreadContext *tc, BaseMMU::Translation *translation)
{
    TlbEntry pre_entry;
    TlbEntry *e_pre;
    double pre_precision = 0;
    TLB *l2tlb;
    if (isStage2) {
        l2tlb = this;
    } else {
        l2tlb = static_cast<TLB *>(nextLevel());
    }
    assert(l2tlb != nullptr);
    pre_entry.vaddr = pre_block;
    pre_entry.asid = asid;
    pre_entry.logBytes = PageShift;
    pre_entry.used = false;
    if (forward) {
        req->setForwardPreVaddr(pre_block);
        l2tlb->insertForwardPre(pre_block, pre_entry);
        pre_precision = forwardPrePrecision;
    } else {
        req->setBackPreVaddr(pre_block);
        l2tlb->insertBackPre(pre_block, pre_entry);
        pre_precision = backPrePrecision;
    }

    if (e_pre_1)
        e_pre = e_pre_1;
    else
        e_pre = e_pre_2;
    Fault pre_fault = L2TLBCheck(e_pre->pte, check_level, status, pmode, pre_block, mode, req, forward, !forward);
    if ((pre_fault == NoFault) && (!hitInSp) && pre_precision) {
        DPRINTF(TLBGPre, "pre_vaddr %#x\n", pre_block);
        walker->start(e_pre->pte.ppn, tc, translation, req, mode, forward, !forward, check_level - 1, true,
                      e_pre->asid);
    }
}
std::pair<bool, Fault>
TLB::L2TLBSendRequest(Fault fault, TlbEntry *e_l2tlb, const RequestPtr &req, ThreadContext *tc,
                      BaseMMU::Translation *translation, BaseMMU::Mode mode, Addr vaddr, bool &delayed, int level)
{
    Addr paddr;
    TlbEntry *e_l2tlbVsstage = nullptr;
    TlbEntry *e_l2tlbGstage = nullptr;

    if (hitInSp) {
        if (fault == NoFault) {
            paddr = e_l2tlb->paddr << PageShift | (vaddr & mask(e_l2tlb->logBytes));
            walker->doL2TLBHitSchedule(req, tc, translation, mode, paddr, e_l2tlb, e_l2tlbVsstage, e_l2tlbGstage, 1);
            delayed = true;
            return std::make_pair(true, fault);
        }
    } else {
        fault = walker->start(e_l2tlb->pte.ppn, tc, translation, req, mode, false, false, level, true, e_l2tlb->asid);
        if (translation != nullptr || fault != NoFault) {
            delayed = true;
            return std::make_pair(true, fault);
        }
    }
    return std::make_pair(false, fault);
}

std::pair<int,Fault>
TLB::checkHL1Tlb(const RequestPtr &req, ThreadContext *tc,
                 BaseMMU::Translation *translation, BaseMMU::Mode mode)
{
    SATP vsatp = tc->readMiscReg(MISCREG_VSATP);
    HGATP hgatp = tc->readMiscReg(MISCREG_HGATP);
    Addr vaddr = VADDR_SEXT(hgatp.mode, req->getVaddr());
    STATUS status = tc->readMiscReg(MISCREG_STATUS);
    Fault fault = NoFault;
    PrivilegeMode pmode = getMemPriv(tc, mode);
    bool continuePtw =false;
    Addr ppn = 0;
    uint64_t pte = 0;
    TlbEntry *e[L_L2SUM] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    int vs_top_level, g_top_level;
    int hit_type = H_L1miss;
    TlbEntry *e_l2tlb = nullptr;
    TlbEntry *e_l2tlbVsstage = nullptr;
    TlbEntry *e_l2tlbGstage = nullptr;
    if (vsatp.mode != 0)
        e[0] = lookup(vaddr, vsatp.asid, mode, false, true, allstage);
    else
        e[0] = lookup(vaddr, hgatp.vmid, mode, false, true, gstage);

    vs_top_level = PTW_TOP_LEVEL(vsatp.mode);
    g_top_level = PTW_TOP_LEVEL(hgatp.mode);

    if (e[0]) {
        hit_type = h_l1AllstageHit;
        DPRINTF(TLB, "l1tlb hit in Allstage\n");
        assert(hgatp.vmid == e[0]->vmid);
        if (vsatp.mode != 0) {
            if ((mode == BaseMMU::Write && !e[0]->pteVS.d) || (!e[0]->pteVS.a))
                fault = createPagefault(vaddr, 0, mode, false);
            if (fault != NoFault) {
                return std::make_pair(hit_type, fault);
            }
            fault = checkPermissions(status, pmode, vaddr, mode, e[0]->pteVS, 0, false);
            if (fault != NoFault) {
                return std::make_pair(hit_type, fault);
            }
        }
        Addr fault_gpaddr = ((e[0]->gpaddr >> 12) << 12) | (vaddr & 0xfff);

        std::pair(continuePtw,fault) = checkGPermissions(status,vaddr,fault_gpaddr,mode,e[0]->pte,req->get_h_inst());
        if (fault != NoFault) {
            return std::make_pair(hit_type, fault);
        }

        Addr paddr = e[0]->paddr << PageShift | (vaddr & mask(e[0]->logBytes));
        if (e[0]->level > 0) {
            paddr = (paddr >> (PageShift + e[0]->level * 9)) << (PageShift + e[0]->level * 9) |
                    (vaddr & mask(e[0]->logBytes));
        }
        req->setPaddr(paddr);
        return std::make_pair(hit_type,NoFault);
    } else {
        Addr pgBase = vsatp.ppn << PageShift;
        Addr gPaddr = 0;

        Addr paddrBase = 0;
        Addr pg_mask = 0;


        e[0] = lookup(vaddr, vsatp.asid, mode, false, true, vsstage);
        if (e[0]){
            req->setPte(e[0]->pte);
            hit_type = h_l1VSstageHit;
            gPaddr = e[0]->pte.ppn <<12;
            DPRINTF(TLB, "l1tlb hit in VSstage: level %d, ppn %#x\n", e[0]->level, e[0]->pte.ppn);
            if (e[0]->level >0){
                pg_mask =  (1ULL << (12 + 9 * e[0]->level)) - 1;
                gPaddr = ((e[0]->pte.ppn << 12) & ~pg_mask) | (vaddr & pg_mask & ~PGMASK);
            }
            gPaddr = gPaddr | (vaddr & PGMASK);
            if (mode == BaseMMU::Write && !e[0]->pte.d) {
                fault = createPagefault(vaddr, 0, mode, false);
                //return fault;
                return std::make_pair(hit_type,fault);
            } else {
                fault = checkPermissions(status, pmode, vaddr, mode, e[0]->pte, 0, false);
                if (fault != NoFault) {
                    return std::make_pair(hit_type, fault);
                }
            }
            req->setPaddr(gPaddr);
            ppn = e[0]->pte.ppn;
            pte = e[0]->pte;

            DPRINTFR(TLB, "\tpass check, try to lookup for Gstage pte\n");

            e[0] = lookup(gPaddr, hgatp.vmid, mode, false, true, gstage);
            if (e[0]) {
                hit_type = h_l1GstageHit;
                DPRINTF(TLB, "l1tlb hit in Gstage: level %d, ppn %#x\n", e[0]->level, e[0]->pte.ppn);
                std::pair(continuePtw, fault) =
                    checkGPermissions(status, vaddr, gPaddr, mode, e[0]->pte, req->get_h_inst());
                if (e[0]->level >0){
                    pg_mask = (1ULL << (12 + 9 * e[0]->level)) - 1;
                    pgBase = ((e[0]->pte.ppn << 12) & ~pg_mask) | (gPaddr & pg_mask & ~PGMASK);
                } else {
                    pgBase = e[0]->pte.ppn << 12;
                }
                gPaddr = pgBase |(gPaddr & PGMASK);


                if (fault != NoFault) {
                    return std::make_pair(hit_type, fault);
                }


                walker->doL2TLBHitSchedule(req, tc, translation, mode, gPaddr, e_l2tlb, e_l2tlbVsstage, e_l2tlbGstage,
                                           3);
                return std::make_pair(hit_type,NoFault);
            } else {
                DPRINTF(TLB, "l1tlb miss in Gstage, set TwoStagePageTableWalk\n");
                req->setTwoPtwWalk(true, 0, g_top_level, ppn, true);
                req->setgPaddr(gPaddr);
                return std::make_pair(hit_type, NoFault);
            }
        } else {
            DPRINTF(TLB, "l1tlb miss in VSstage, set TwoStagePageTableWalk\n");
            req->setTwoPtwWalk(false, vs_top_level, g_top_level, ppn, false);
            req->setgPaddr(gPaddr);
            return std::make_pair(hit_type, NoFault);
        }
    }
}

std::pair<int, Fault>
TLB::checkHL2Tlb(const RequestPtr &req, ThreadContext *tc, BaseMMU::Translation *translation, BaseMMU::Mode mode,
                 int l1tlbtype)
{
    SATP vsatp = tc->readMiscReg(MISCREG_VSATP);
    HGATP hgatp = tc->readMiscReg(MISCREG_HGATP);
    Addr vaddr = VADDR_SEXT(hgatp.mode, req->getVaddr());
    STATUS status = tc->readMiscReg(MISCREG_STATUS);
    STATUS vstatus = tc->readMiscReg(MISCREG_VSSTATUS);
    Fault fault = NoFault;
    PrivilegeMode pmode = getMemPriv(tc, mode);
    bool continuePtw = false;
    TlbEntry *e[L_L2SUM] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    int hit_type = H_L1miss;
    int hit_level;
    int hit_flag = 0;
    Addr gPaddr = 0;
    Addr pg_mask = 0;
    int level;
    int twoStageLevel;
    bool finishgva = false;
    hitInSp = false;

    Addr pgBase = vsatp.ppn << PageShift;

    TLB *l2tlb;
    if (isStage2) {
        l2tlb = this;
    } else {
        l2tlb = static_cast<TLB *>(nextLevel());
    }

    assert(l2tlb != nullptr);
    // first check
    TlbEntry *e_l2tlb = nullptr;
    TlbEntry *e_l2tlbVsstage = nullptr;
    TlbEntry *e_l2tlbGstage = nullptr;

    if ((!e[0]) && (l1tlbtype == h_l1VSstageHit)) {
        hit_level = PTW_TOP_LEVEL(vsatp.mode);
        DPRINTF(TLB, "lookup for l2tlb at Gstage\n");
        for (int i_e = 1; i_e < L_L2SUM; i_e++) {
            if ((hgatp.mode == AddrXlateMode::SV39) && (i_e == L_L2L3 || i_e == L_L2sp3))
                continue;
            e[i_e] = l2tlb->lookupL2TLB(req->getgPaddr(), hgatp.vmid, mode, false, i_e, true, gstage);
            if (e[i_e]) {
                if (e[i_e]->level < hit_level) {
                    e[0] = e[i_e];
                    hit_level = e[i_e]->level;
                    hit_flag = i_e;
                }
            }
        }
        if (e[0]) {
            e_l2tlbGstage = e[0];
            gPaddr = req->getgPaddr();
            std::pair<bool, Fault> result =
                checkGPermissions(status, vaddr, gPaddr, mode, e[0]->pte, req->get_h_inst());
            continuePtw = result.first;
            fault = result.second;
            DPRINTF(TLB, "l2tlb hit in Gstage: "
                    "level %d, hit_flag %d, ppn %#x\n", hit_level, hit_flag, e[0]->pte.ppn);
            if (fault != NoFault) {
                return std::make_pair(hit_type, fault);
            } else if (continuePtw) {
                hit_type = h_l2GstageHitContinue;
                twoStageLevel = hit_level;
                hit_level--;
                if (hit_level < 0) {
                    assert(0);
                }
                DPRINTFR(TLB, "\tneed continue to PTW(h_l2GstageHitContinue)\n");
                req->setTwoPtwWalk(true, 0, e[0]->level-1, e[0]->pte.ppn, true);
                req->setgPaddr(gPaddr);
                return std::make_pair(hit_type, fault);
            } else {
                hit_type = h_l2GstageHitEnd;
                DPRINTFR(TLB, "\tfind the leaf pte(h_l2GstageHitEnd)\n");
                if (e[0]->level > 0) {
                    pg_mask = (1ULL << (12 + 9 * e[0]->level)) - 1;
                    pgBase = ((e[0]->pte.ppn << 12) & ~pg_mask) | (gPaddr & pg_mask & ~PGMASK);
                }
                else {
                    pgBase = e[0]->pte.ppn << 12;
                }
                gPaddr = pgBase | (gPaddr & PGMASK);
                walker->doL2TLBHitSchedule(req, tc, translation, mode, gPaddr, e_l2tlb, e_l2tlbVsstage, e_l2tlbGstage,
                                           4);
                return std::make_pair(hit_type, NoFault);
            }
        } else {
            hit_type = h_l2VSstageHitEnd;
            DPRINTF(TLB, "l2tlb miss in Gstage (h_l2VSstageHitEnd)\n");
            return std::make_pair(hit_type, NoFault);
        }
    }

    if (!e[0]) {
        DPRINTF(TLB, "l1tlb miss, lookup l2tlb at VSstage.\n");
        e[0] = lookup(vaddr, vsatp.asid, mode, false, true, vsstage);
        hit_level = PTW_TOP_LEVEL(vsatp.mode);
        if (!e[0]) {
            for (int i_e = 1; i_e < L_L2SUM; i_e++) {
                if ((vsatp.mode == AddrXlateMode::SV39) && (i_e == L_L2L3 || i_e == L_L2sp3))
                    continue;
                e[i_e] = l2tlb->lookupL2TLB(vaddr, vsatp.asid, mode, false, i_e, true, vsstage);
                if (e[i_e]) {
                    if (e[i_e]->level < hit_level) {
                        e[0] = e[i_e];
                        hit_level = e[i_e]->level;
                        hit_flag = i_e;
                    }
                }
            }
        }
        if (e[0]) {
            DPRINTF(TLB, "l2 tlb hit in VSstage (h_l2VSstageHitContinue).\n");
            e_l2tlbVsstage = e[0];
            hit_type = h_l2VSstageHitContinue;
            level = e[0]->level;
            fault = L2TLBCheck(e[0]->pte, e[0]->level, vstatus, pmode, vaddr, mode, req, false, false);
            finishgva = hitInSp;
            req->setPte(e[0]->pte);
            uint64_t hit_vaddr = e[0]->vaddr;
            if (fault != NoFault) {
                DPRINTF(TLB, "l2 tlb pte check with fault, return. (h_l2VSstageHitContinue).\n");
                return std::make_pair(hit_type, fault);
            } else {
                DPRINTF(TLB, "l2 tlb pte pass check: level %d, isLeaf? %d, ppn %#x. (h_l2VSstageHitEnd).\n",
                        level, hitInSp, e[0]->pte.ppn);
                hit_type = h_l2VSstageHitEnd;
                gPaddr = e[0]->pte.ppn << 12;
                if (finishgva) {
                    if (e[0]->level > 0) {
                        pg_mask = (1ULL << (12 + 9 * e[0]->level)) - 1;
                        gPaddr = ((e[0]->pte.ppn << 12) & ~pg_mask) | (vaddr & pg_mask & ~PGMASK);
                    }
                    gPaddr = gPaddr | (vaddr & PGMASK);
                } else {
                    level--;
                    Addr shift = (PageShift + LEVEL_BITS * level);
                    Addr idx_f = (vaddr >> shift) & LEVEL_MASK;
                    Addr idx = (idx_f >> L2TLB_BLK_OFFSET) << L2TLB_BLK_OFFSET;
                    gPaddr = (e[0]->pte.ppn << PageShift) + (idx_f * l2tlbLineSize);
                }

                DPRINTFR(TLB, "\tlookup (gPaddr: %#x) in l1tlb again for Gstage.\n", gPaddr);
                e[0] = nullptr;
                e[0] = lookup(gPaddr, hgatp.vmid, mode, false, true, gstage);
                if (!e[0]) {
                    DPRINTF(TLB, "l1tlb miss, lookup (gPaddr: %#x) l2tlb for Gstage.\n", gPaddr);
                    hit_level = PTW_TOP_LEVEL(hgatp.mode);
                    for (int i_e = 1; i_e < L_L2SUM; i_e++) {
                        if ((hgatp.mode == AddrXlateMode::SV39) && (i_e == L_L2L3 || i_e == L_L2sp3))
                            continue;
                        e[i_e] = l2tlb->lookupL2TLB(gPaddr, hgatp.vmid, mode, false, i_e, true, gstage);
                        if (e[i_e]) {
                            if (e[i_e]->level < hit_level) {
                                e[0] = e[i_e];
                                hit_level = e[i_e]->level;
                                hit_flag = i_e;
                                DPRINTF(TLB, "l2tlb hit (gPaddr: %#x) for Gstage: hit_flag %d hit_level %d.\n",
                                        gPaddr, hit_flag, hit_level);
                            }
                        }
                    }
                }
                if (e[0]) {
                    e_l2tlbGstage = e[0];
                    twoStageLevel = e[0]->level;
                    req->setgPaddr(gPaddr);
                    auto check_res = checkGPermissions(status, vaddr, gPaddr, mode, e[0]->pte, req->get_h_inst());
                    continuePtw = check_res.first;
                    fault = check_res.second;
                    DPRINTF(TLB, "found pte for gPaddr: %#x in Gstage -> "
                            "level:%d, ppn:%#x, pte.(v:%d, r:%d, w:%d, x:%d).\n",
                            gPaddr, e[0]->level, e[0]->pte.ppn, e[0]->pte.v, e[0]->pte.r, e[0]->pte.w, e[0]->pte.x);
                    if (fault != NoFault) {
                        return std::make_pair(hit_type, fault);
                    } else if (continuePtw) {
                        DPRINTFR(TLB, "\tneed to continue PageTableWalk, setTwoStagePtW. "
                                 "(h_l2GstageHitContinue)\n");
                        hit_type = h_l2GstageHitContinue;
                        req->setTwoPtwWalk(true, level, twoStageLevel-1, e[0]->pte.ppn, hitInSp);
                        req->setgPaddr(gPaddr);
                        return std::make_pair(hit_type, fault);
                    } else {
                        DPRINTFR(TLB, "\tHit the leaf page. ");
                        hit_type = h_l2GstageHitEnd;
                        uint64_t gpaddr_past = gPaddr;
                        if (finishgva) {
                            if (e[0]->level > 0) {
                                pg_mask = (1ULL << (12 + 9 * e[0]->level)) - 1;
                                pgBase = ((e[0]->pte.ppn << 12) & ~pg_mask) | (gPaddr & pg_mask & ~PGMASK);
                            } else {
                                pgBase = (e[0]->pte.ppn << 12);
                            }
                            gPaddr = pgBase | (gPaddr & PGMASK);
                            DPRINTFR(TLB, "GVA finish, got HPA %#x, "
                                     "schedule l2tlb hit event. (h_l2GstageHitEnd)\n", gPaddr);
                            walker->doL2TLBHitSchedule(req, tc, translation, mode, gPaddr, e_l2tlb, e_l2tlbVsstage,
                                                       e_l2tlbGstage, 4);
                        } else {
                            DPRINTFR(TLB, "GVA not finish, "
                                     "still need to setTwoStagePTW. (h_l2VSstageHitContinue)\n");
                            uint64_t ppppn = e[0]->pte.ppn;
                            hit_type = h_l2VSstageHitContinue;
                            req->setTwoPtwWalk(false, level, e[0]->level, e[0]->pte.ppn, hitInSp);
                        }
                        return std::make_pair(hit_type, NoFault);
                    }

                } else {
                    DPRINTF(TLB, "l1tlb and l2tlb all miss, for Gstage "
                            "(gPaddr: %#x, h_l2VSstageHitEnd).\n", gPaddr);
                    hit_type = h_l2VSstageHitEnd;
                    twoStageLevel = PTW_TOP_LEVEL(vsatp.mode);
                    // req->setTwoPtwWalk(false,level,twoStageLevel,0,hitInSp);
                    req->setTwoPtwWalk(true, level, twoStageLevel, 0, hitInSp);
                    req->setgPaddr(gPaddr);
                    if ((gPaddr & ~H_VADDR_MASK(vsatp.mode)) != 0) {
                        // this is a excep
                        fault = createPagefault(vaddr, gPaddr, mode, false);
                        return std::make_pair(hit_type, fault);
                    }
                    return std::make_pair(hit_type, NoFault);
                }
            }

        } else {
            DPRINTF(TLB, "l2 tlb miss at VSstage (H_L1miss), set TwoStagePTW.\n");
            hit_type = H_L1miss;
            twoStageLevel = PTW_TOP_LEVEL(vsatp.mode);
            level = PTW_TOP_LEVEL(vsatp.mode);
            req->setTwoPtwWalk(false, level, twoStageLevel, 0, hitInSp);
            req->setgPaddr(gPaddr);
            return std::make_pair(hit_type,NoFault);
        }
    }
    return std::make_pair(hit_type,NoFault);
}

Fault
TLB::doTwoStageTranslate(const RequestPtr &req, ThreadContext *tc,
                 BaseMMU::Translation *translation, BaseMMU::Mode mode,
                 bool &delayed)
{
    SATP vsatp = tc->readMiscReg(MISCREG_VSATP);
    HGATP hgatp = tc->readMiscReg(MISCREG_HGATP);
    Addr vaddr = VADDR_SEXT(hgatp.mode, req->getVaddr());
    int virt = tc->readMiscReg(MISCREG_VIRMODE);
    STATUS status = tc->readMiscReg(MISCREG_STATUS);
    HSTATUS hstatus = tc->readMiscReg(MISCREG_HSTATUS);
    int two_stage_pmode = (int)getMemPriv(tc, mode);
    Fault fault = NoFault;
    PrivilegeMode pmode = getMemPriv(tc, mode);
    bool continuePtw = false;
    int l1tlbtype = H_L1miss;


    TLB *l2tlb;
    if (isStage2) {
        l2tlb = this;
    } else {
        l2tlb = static_cast<TLB *>(nextLevel());
    }

    assert(l2tlb != nullptr);

    if (mode != BaseMMU::Execute) {
        if (status.mprv) {
            two_stage_pmode = status.mpp;
            virt = status.mpv && (two_stage_pmode != PrivilegeMode::PRV_M);
        }

        if (req->get_h_inst()) {
            virt = 1;
            two_stage_pmode = (PrivilegeMode)(RegVal)hstatus.spvp;
        }
    }

    if (virt != 0) {
        if (vsatp.mode == 0) {
            req->setVsatp0Mode(true);
            req->setTwoStageState(true, virt, two_stage_pmode);
            if ((vaddr & ~H_VADDR_MASK(hgatp.mode)) != 0 ){
                return createPagefault(vaddr,vaddr,mode,true);
            }
        } else {
            req->setVsatp0Mode(false);
            req->setTwoStageState(true, virt, two_stage_pmode);
        }
        std::pair<int, Fault> result = checkHL1Tlb(req, tc, translation, mode);
        l1tlbtype = result.first;
        fault = result.second;

        if (fault != NoFault) {
            return fault;
        } else if ((l1tlbtype == h_l1VSstageHit) || (l1tlbtype == H_L1miss)) {
            std::pair<int, Fault> result = checkHL2Tlb(req, tc, translation, mode, l1tlbtype);
            if (result.second != NoFault) {
                return result.second;
            }

            if ((result.first == h_l2GstageHitContinue) || (result.first == h_l2VSstageHitEnd) ||
                (result.first == H_L1miss) || (result.first == h_l2VSstageHitContinue)) {
                Addr shift = PageShift + LEVEL_BITS * req->get_level();
                Addr idx_f = (vaddr >> shift) & LEVEL_MASK;
                Addr gpaddr_check = (vsatp.ppn << PageShift) + (idx_f * sizeof(PTE));
                if ((req->get_level() != PTW_TOP_LEVEL(hgatp.mode)) && (req->getgPaddr() != 0)) {
                    gpaddr_check = req->getgPaddr();
                }
                if (req->get_vsatp_0_mode()) {
                    gpaddr_check = vaddr;
                }
                if ((gpaddr_check & ~H_VADDR_MASK(hgatp.mode)) != 0) {
                    // this is a excep
                    ExceptionCode code;
                    if (mode == BaseMMU::Read) {
                        code = ExceptionCode::LOAD_ACCESS;
                    } else if (mode == BaseMMU::Write) {
                        code = ExceptionCode::STORE_ACCESS;
                    } else {
                        code = ExceptionCode::INST_ACCESS;
                    }
                    return std::make_shared<AddressFault>(req->getVaddr(), 0, code);
                }
                DPRINTFR(TLB, "doTwoStageTranslate: walker start\n");
                fault = walker->start(0, tc, translation, req, mode, false, false, 0, false, 0);
                if (translation != nullptr || fault != NoFault) {
                    delayed = true;
                    return fault;
                }
                return fault;
            } else if (result.first == h_l2GstageHitEnd) {
                delayed = true;
                return fault;
            } else {
                req->getPaddr();
            }
        } else if (l1tlbtype == h_l1GstageHit) {
            delayed = true;
            return fault;
        } else {
            req->getPaddr();
        }
    }
    return fault;
}
Fault
TLB::doTranslate(const RequestPtr &req, ThreadContext *tc,
                 BaseMMU::Translation *translation, BaseMMU::Mode mode,
                 bool &delayed)
{
    delayed = false;
    SATP satp = tc->readMiscReg(MISCREG_SATP);
    Addr vaddr = VADDR_SEXT(satp.mode, req->getVaddr());
    Addr vaddr_trace = (vaddr >> (PageShift + L2TLB_BLK_OFFSET)) << (PageShift + L2TLB_BLK_OFFSET);
    if (((vaddr_trace != lastVaddr) || (req->getPC() != lastPc)) &&
        is_dtlb) {
        traceFlag = true;
        lastVaddr = vaddr_trace;
        lastPc = req->getPC();
    } else {
        traceFlag = false;
    }

    TlbEntry *e[L_L2SUM] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    TlbEntry *forward_pre[L_L2SUM] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    TlbEntry *back_pre[L_L2SUM] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    e[0] = lookup(vaddr, satp.asid, mode, false, true, direct);
    Addr paddr = 0;
    Fault fault = NoFault;
    Fault fault_return = NoFault;
    Fault forward_pre_fault;
    Fault back_pre_fault;
    STATUS status = tc->readMiscReg(MISCREG_STATUS);
    PrivilegeMode pmode = getMemPriv(tc, mode);
    DPRINTF(TLB, "doTranslate: satp_mode is %d\n", satp.mode);

    TLB *l2tlb;
    if (isStage2) {
        l2tlb = this;
    } else {
        l2tlb = static_cast<TLB *>(nextLevel());
    }

    assert(l2tlb != nullptr);

    uint64_t remove_unused_forward_pre = l2tlb->removeNoUseForwardPre;
    uint64_t all_forward_pre_num = l2tlb->allForwardPre;
    uint64_t all_used_num = l2tlb->allUsed / l2tlbLineSize;
    uint64_t all_used_forward_pre_num = l2tlb->forwardUsedPre;
    auto precision = (double)(all_forward_pre_num - remove_unused_forward_pre) / (all_forward_pre_num + 1);

    auto recall = (double)all_used_forward_pre_num / (all_used_num + 1);
    RequestPtr pre_req = req;


    Addr forward_pre_vaddr = vaddr + (l2tlbLineSize << PageShift);
    Addr forward_pre_block = (forward_pre_vaddr >> (PageShift + L2TLB_BLK_OFFSET)) << (PageShift + L2TLB_BLK_OFFSET);
    Addr vaddr_block = (vaddr >> (PageShift + L2TLB_BLK_OFFSET)) << (PageShift + L2TLB_BLK_OFFSET);
    Addr back_pre_vaddr = vaddr - (l2tlbLineSize << PageShift);
    Addr back_pre_block = (back_pre_vaddr >> (PageShift + L2TLB_BLK_OFFSET)) << (PageShift + L2TLB_BLK_OFFSET);

    l2tlb->lookupForwardPre(vaddr_block, satp.asid, false);
    TlbEntry *pre_forward = l2tlb->lookupForwardPre(forward_pre_block, satp.asid, true);

    l2tlb->lookupBackPre(vaddr_block, satp.asid, false);
    TlbEntry *pre_back = l2tlb->lookupBackPre(back_pre_block, satp.asid, true);
    backPrePrecision = checkPrePrecision(l2tlb->removeNoUseBackPre, l2tlb->usedBackPre);
    forwardPrePrecision = checkPrePrecision(l2tlb->removeNoUseForwardPre, l2tlb->forwardUsedPre);
    if (walker->openSv48) {
        fault = walker->start(0, tc, translation, req, mode, false, false, 3, false, 0);

        if (translation != nullptr || fault != NoFault) {
            // This gets ignored in atomic mode.
            delayed = true;
            return fault;
        } else {
            panic("sv48 goes wrong\n");
        }
    }

    for (int i_e = 1; i_e < L_L2SUM; i_e++) {
        if ((satp.mode == AddrXlateMode::SV39) && (i_e == L_L2L3 || i_e == L_L2sp3))
            continue;
        if (!e[0])
            e[i_e] = l2tlb->lookupL2TLB(vaddr, satp.asid, mode, false, i_e, true, direct);

        forward_pre[i_e] = l2tlb->lookupL2TLB(forward_pre_block, satp.asid, mode, true, i_e, true, direct);
        back_pre[i_e] = l2tlb->lookupL2TLB(back_pre_block, satp.asid, mode, true, i_e, true, direct);
    }
    bool return_flag = false;

    if (!e[0]) {  // look up l2tlb
        if (e[L_L2L0] && e[L_L2L0]->pte.v) {  // if hit in l2l0 (leaf 4KB page)
            DPRINTF(TLBVerbosel2, "hit in l2TLB l0\n");
            fault = L2TLBCheck(e[L_L2L0]->pte, L2L0CheckLevel, status, pmode, vaddr, mode, req, false, false);
            if (hitInSp) {
                e[0] = e[L_L2L0];
                if (fault == NoFault) {
                    paddr = e[0]->paddr << PageShift | (vaddr & mask(e[0]->logBytes));
                    DPRINTF(TLBVerbosel2, "vaddr %#x,paddr %#x,pc %#x\n", vaddr, paddr, req->getPC());
                    TlbEntry *e_l2tlbVsstage = nullptr;
                    TlbEntry *e_l2tlbGstage = nullptr;
                    walker->doL2TLBHitSchedule(req, tc, translation, mode, paddr,
                                               e[L_L2L0], e_l2tlbVsstage, e_l2tlbGstage, 1);
                    DPRINTF(TLBVerbosel2, "finish Schedule\n");
                    delayed = true;
                    if ((forward_pre_block != vaddr_block) && (!forward_pre[L_L2L0])
                        && openForwardPre && (!pre_forward)) {
                        if (forward_pre[L_L2L1] || forward_pre[L_L2sp1]) {
                            sendPreHitOnHitRequest(forward_pre[L_L2sp1], forward_pre[L_L2L1], req, forward_pre_block,
                                                   satp.asid, true, L2L1CheckLevel, status, pmode, mode, tc,
                                                   translation);
                        } else {
                            if (forward_pre[L_L2L2] || forward_pre[L_L2sp2]) {
                                sendPreHitOnHitRequest(forward_pre[L_L2sp2], forward_pre[L_L2L2], req,
                                                       forward_pre_block, satp.asid, true, L2L2CheckLevel,
                                                       status, pmode, mode, tc, translation);
                            } else {
                                if (satp.mode == AddrXlateMode::SV48
                                    && (forward_pre[L_L2L3] || forward_pre[L_L2sp3])) {
                                    sendPreHitOnHitRequest(forward_pre[L_L2sp3], forward_pre[L_L2L3], req,
                                                           forward_pre_block, satp.asid, true, L2L3CheckLevel,
                                                           status, pmode, mode, tc, translation);
                                }
                            }
                        }
                    }
                    if ((back_pre_block != vaddr_block) && (!back_pre[L_L2L0]) && openBackPre && (!pre_back)) {
                        if (back_pre[L_L2L1] || back_pre[L_L2sp1]) {
                            sendPreHitOnHitRequest(back_pre[L_L2sp1], back_pre[L_L2L1], req, back_pre_block, satp.asid,
                                                   false, L2L1CheckLevel, status, pmode, mode, tc, translation);
                        }
                    }
                    if (traceFlag)
                        DPRINTF(TLBtrace, "tlb hit in vaddr %#x pc %#x\n", vaddr_trace, req->getPC());
                    return fault;
                }
            } else {
                panic("wrong in L2TLB\n");
            }

        } else if (e[L_L2sp1] && e[L_L2sp1]->pte.v) {  // hit in sp1
            DPRINTF(TLBVerbosel2, "hit in l2 tlb sp1\n");
            fault = L2TLBCheck(e[L_L2sp1]->pte, L2L1CheckLevel, status, pmode, vaddr, mode, req, false, false);
            if (hitInSp)
                e[0] = e[L_L2sp1];
            auto [return_flag, fault_return] =
                L2TLBSendRequest(fault, e[L_L2sp1], req, tc, translation, mode, vaddr, delayed, L2L1CheckLevel - 1);
            if (return_flag)
                return fault_return;
        } else if (e[L_L2sp2] && e[L_L2sp2]->pte.v) {  // hit in sp2
            DPRINTF(TLBVerbosel2, "hit in l2 tlb sp2\n");
            fault = L2TLBCheck(e[L_L2sp2]->pte, L2L2CheckLevel, status, pmode, vaddr, mode, req, false, false);
            if (hitInSp)
                e[0] = e[L_L2sp2];
            auto [return_flag, fault_return] =
                L2TLBSendRequest(fault, e[L_L2sp2], req, tc, translation, mode, vaddr, delayed, L2L2CheckLevel - 1);
            if (return_flag)
                return fault_return;
        } else if (satp.mode == AddrXlateMode::SV48 && e[L_L2sp3] && e[L_L2sp3]->pte.v) {  // hit in sp3
            DPRINTF(TLBVerbosel2, "hit in l2 tlb sp3\n");
            fault = L2TLBCheck(e[L_L2sp3]->pte, L2L3CheckLevel, status, pmode, vaddr, mode, req, false, false);
            if (hitInSp)
                e[0] = e[L_L2sp3];
            auto [return_flag, fault_return] =
                L2TLBSendRequest(fault, e[L_L2sp3], req, tc, translation, mode, vaddr, delayed, L2L3CheckLevel - 1);
            if (return_flag)
                return fault_return;
        } else if (e[L_L2L1] && e[L_L2L1]->pte.v) {
            DPRINTF(TLBVerbosel2, "hit in l2 tlb l1\n");
            DPRINTF(TLBVerbosel2, "hit ppn: %#x\n", e[L_L2L1]->pte.ppn);
            fault = L2TLBCheck(e[L_L2L1]->pte, L2L1CheckLevel, status, pmode, vaddr, mode, req, false, false);
            if (hitInSp)
                e[0] = e[L_L2L1];
            auto [return_flag, fault_return] =
                L2TLBSendRequest(fault, e[L_L2L1], req, tc, translation, mode, vaddr, delayed, L2L1CheckLevel - 1);
            if (return_flag)
                return fault_return;
        } else if (e[L_L2L2] && e[L_L2L2]->pte.v) {
            DPRINTF(TLBVerbosel2, "hit in l2 tlb l2\n");
            DPRINTF(TLBVerbosel2, "hit pte: %#x\n", e[L_L2L2]->pte);
            fault = L2TLBCheck(e[L_L2L2]->pte, L2L2CheckLevel, status, pmode, vaddr, mode, req, false, false);
            if (hitInSp)
                e[0] = e[L_L2L2];
            auto [return_flag, fault_return] =
                L2TLBSendRequest(fault, e[L_L2L2], req, tc, translation, mode, vaddr, delayed, L2L2CheckLevel - 1);
            if (return_flag)
                return fault_return;
        } else if (satp.mode == AddrXlateMode::SV48 && e[L_L2L3] && e[L_L2L3]->pte.v) {
            DPRINTF(TLBVerbosel2, "hit in l2 tlb l3\n");
            fault = L2TLBCheck(e[L_L2L3]->pte, L2L3CheckLevel, status, pmode, vaddr, mode, req, false, false);
            if (hitInSp)
                e[0] = e[L_L2L3];
            auto [return_flag, fault_return] =
                L2TLBSendRequest(fault, e[L_L2L3], req, tc, translation, mode, vaddr, delayed, L2L3CheckLevel - 1);
            if (return_flag)
                return fault_return;
        } else {
            DPRINTF(TLB, "miss in l1 tlb + l2 tlb\n");
            DPRINTF(TLBGPre, "pre_req %d vaddr %#x req_vaddr %#x pc %#x\n", req->get_forward_pre_tlb(), vaddr,
                    req->getVaddr(), req->getPC());

            if (traceFlag)
                DPRINTF(TLBtrace, "tlb miss vaddr %#x pc %#x\n", vaddr_trace, req->getPC());
            int walk_level = satp.mode == AddrXlateMode::SV48 ? 3 : 2;
            fault = walker->start(0, tc, translation, req, mode, false, false, walk_level, false, 0);
            DPRINTF(TLB, "finish start\n");
            if (translation != nullptr || fault != NoFault) {
                // This gets ignored in atomic mode.
                delayed = true;
                return fault;
            }
            e[0] = lookup(vaddr, satp.asid, mode, false, true, direct);
            assert(e[0] != nullptr);
        }
    }
    if (!e[0])
        e[0] = lookup(vaddr, satp.asid, mode, false, true, direct);
    assert(e[0] != nullptr);

    status = tc->readMiscReg(MISCREG_STATUS);
    if (mode == BaseMMU::Write && !e[0]->pte.d) {
        fault = createPagefault(vaddr, 0, mode, false);
    }

    if (fault == NoFault) {
        DPRINTF(TLB, "final checkpermission\n");
        DPRINTF(TLB, "translate(vpn=%#x, asid=%#x): %#x pc %#x mode %i pte.d %d\n", vaddr, satp.asid, paddr,
                req->getPC(), mode, e[0]->pte.d);
        fault = checkPermissions(status, pmode, vaddr, mode, e[0]->pte, 0, false);
    }


    if (fault != NoFault) {
        // if we want to write and it isn't writable, do a page table walk
        // again to update the dirty flag.
        //change update a/d not need to do a pagetable walker
        DPRINTF(TLB, "raise pf pc%#x vaddr %#x\n", req->getPC(), vaddr);
        DPRINTF(TLBVerbose3, "mode %i pte.d %d pte.w %d pte.r %d pte.x %d pte.u %d\n", mode, e[0]->pte.d, e[0]->pte.w,
                e[0]->pte.r, e[0]->pte.x, e[0]->pte.u);
        DPRINTF(TLBVerbose3, "paddr %#x ppn %#x\n", e[0]->paddr, e[0]->pte.ppn);
        if (traceFlag)
            DPRINTF(TLBtrace, "tlb hit in l1 but pf vaddr %#x,pc%#x\n", vaddr_trace, req->getPC());
        return fault;
    }
    assert(e[0] != nullptr);
    paddr = e[0]->paddr << PageShift | (vaddr & mask(e[0]->logBytes));

    DPRINTF(TLBVerbosel2, "translate(vpn=%#x, asid=%#x): %#x pc%#x\n", vaddr,
            satp.asid, paddr, req->getPC());
    req->setPaddr(paddr);

    if (e[0]) {
        // same block
        if (traceFlag)
            DPRINTF(TLBtrace, "tlb hit in l1 vaddr %#x,pc%#x\n", vaddr_trace,
                    req->getPC());
        if ((forward_pre_block != vaddr_block) && (!forward_pre[L_L2L0]) && openForwardPre && (!pre_forward)) {
            if (forward_pre[L_L2L1] || forward_pre[L_L2sp1]) {
                sendPreHitOnHitRequest(forward_pre[L_L2sp1], forward_pre[L_L2L1], req, forward_pre_block,
                                       satp.asid, true, L2L1CheckLevel, status, pmode, mode, tc, translation);
            } else {
                if (forward_pre[L_L2L2] || forward_pre[L_L2sp2]) {
                    sendPreHitOnHitRequest(forward_pre[L_L2sp2], forward_pre[L_L2L2], req, forward_pre_block,
                                           satp.asid, true, L2L2CheckLevel, status, pmode, mode, tc, translation);
                } else {
                    if (satp.mode == AddrXlateMode::SV48 && (forward_pre[L_L2L3] || forward_pre[L_L2sp3])){
                        sendPreHitOnHitRequest(forward_pre[L_L2sp3], forward_pre[L_L2L3], req, forward_pre_block,
                                               satp.asid, true, L2L3CheckLevel, status, pmode, mode, tc, translation);
                    }
                }
            }
        }
        if ((back_pre_block != vaddr_block) && (!back_pre[L_L2L0]) && openBackPre && (!pre_back)) {
            if (back_pre[L_L2L1] || back_pre[L_L2sp1]) {
                sendPreHitOnHitRequest(back_pre[L_L2sp1], back_pre[L_L2L1], req, back_pre_block, satp.asid, false,
                                       L2L1CheckLevel, status, pmode, mode, tc, translation);
            }
        }
    }

    return NoFault;
}

PrivilegeMode
TLB::getMemPriv(ThreadContext *tc, BaseMMU::Mode mode)
{
    if (use_old_priv && mode != BaseMMU::Execute) {
        if (mode == BaseMMU::Execute) {
            return old_priv_ex;
        } else {
            return old_priv_ldst;
        }
    }
    STATUS status = (STATUS)tc->readMiscReg(MISCREG_STATUS);
    PrivilegeMode pmode = (PrivilegeMode)tc->readMiscReg(MISCREG_PRV);
    if (mode != BaseMMU::Execute && status.mprv == 1)
        pmode = (PrivilegeMode)(RegVal)status.mpp;
    return pmode;
}
bool
TLB::hasTwoStageTranslation(ThreadContext *tc, const RequestPtr &req, BaseMMU::Mode mode)
{
    STATUS status = (STATUS)tc->readMiscReg(MISCREG_STATUS);
    int v_mode = tc->readMiscReg(MISCREG_VIRMODE);
    return (req->get_h_inst()) || (status.mprv && status.mpv) || v_mode;
}

MMUMode
TLB::isaMMUCheck(ThreadContext *tc, Addr vaddr, BaseMMU::Mode mode)
{
    STATUS status = tc->readMiscReg(MISCREG_STATUS);
    PrivilegeMode pp = (PrivilegeMode)tc->readMiscReg(MISCREG_PRV);
    SATP satp = tc->readMiscReg(MISCREG_SATP);
    SATP vsatp = tc->readMiscReg(MISCREG_VSATP);
    int v_mode = tc->readMiscReg(MISCREG_VIRMODE);
    HGATP hgatp = tc->readMiscReg(MISCREG_HGATP);
    bool vm_enable = (status.mprv && (mode == BaseMMU::Execute) ? status.mpp : pp) < PRV_M &&
                     (satp.mode == 8 || (v_mode && (vsatp.mode == 8 || hgatp.mode == 8)));
    Addr vaMask = ((((Addr)1) << (63 - 38 + 1)) - 1);
    Addr vaMsbs = vaddr >> 38;
    bool vaMsbsOk = (vaMsbs == vaMask) || vaMsbs == 0 || !vm_enable;
    bool gpf = false;
    if ((v_mode == 1) && (vsatp.mode == 0)) {
        Addr maxgpa = ((((Addr)1) << (41)) - 1);
        if ((vaddr & ~maxgpa) == 0) {
            vaMsbsOk = 1;
        } else {
            gpf = true;
        }
    }
    assert(vaMsbsOk);
    return MMU_DIRECT;
}

Fault
TLB::translate(const RequestPtr &req, ThreadContext *tc,
               BaseMMU::Translation *translation, BaseMMU::Mode mode,
               bool &delayed)
{
    delayed = false;

    if (FullSystem) {
        PrivilegeMode pmode = getMemPriv(tc, mode);
        SATP satp = tc->readMiscReg(MISCREG_SATP);
        SATP vsatp = tc->readMiscReg(MISCREG_VSATP);
        HGATP hgatp = tc->readMiscReg(MISCREG_HGATP);
        int v_mode = tc->readMiscReg(MISCREG_VIRMODE);
        bool two_stage_translation = false;
        STATUS status = tc->readMiscReg(MISCREG_STATUS);

        if ((pmode == PrivilegeMode::PRV_M || satp.mode == AddrXlateMode::BARE))
            req->setFlags(Request::PHYSICAL);

        Fault fault;

        if (req->getFlags() & Request::PHYSICAL) {
            req->setTwoStageState(false, 0, 0);
            /**
             * we simply set the virtual address to physical address
             */

            if ((hgatp.mode == NEMU_SATP_SV39 || vsatp.mode == NEMU_SATP_SV39
                || hgatp.mode == NEMU_SATP_SV48 || vsatp.mode == NEMU_SATP_SV48)
                && (pmode < PrivilegeMode::PRV_M)) {
                fault = doTwoStageTranslate(req, tc, translation, mode, delayed);
            } else {
                req->setPaddr(req->getVaddr());
                fault = NoFault;
                assert(!req->get_h_inst());
            }
        } else {
            two_stage_translation = hasTwoStageTranslation(tc, req, mode);
            if (two_stage_translation) {
                assert((vsatp.mode == NEMU_SATP_SV39) || (hgatp.mode == NEMU_SATP_SV39)
                       || (vsatp.mode == NEMU_SATP_SV48) || (hgatp.mode == NEMU_SATP_SV48));
                fault = doTwoStageTranslate(req, tc, translation, mode, delayed);
            } else {
                req->setTwoStageState(false, 0, 0);
                controlNum++;
                fault = doTranslate(req, tc, translation, mode, delayed);
            }
        }

        // according to the RISC-V tests, negative physical addresses trigger
        // an illegal address exception.
        // TODO where is that written in the manual?
        if (!delayed && fault == NoFault && bits(req->getPaddr(), 63)) {
            ExceptionCode code;
            if (mode == BaseMMU::Read)
                code = ExceptionCode::LOAD_ACCESS;
            else if (mode == BaseMMU::Write) {
                code = ExceptionCode::STORE_ACCESS;
            }
            else
                code = ExceptionCode::INST_ACCESS;
            fault = std::make_shared<AddressFault>(req->getVaddr(), 0, code);
        }

        if (!delayed && fault == NoFault) {
            pma->check(req);

            // do pmp check if any checking condition is met.
            // mainFault will be NoFault if pmp checks are
            // passed, otherwise an address fault will be returned.
            fault = pmp->pmpCheck(req, mode, pmode, tc);
        }

        return fault;
    } else {
        // In the O3 CPU model, sometimes a memory access will be speculatively
        // executed along a branch that will end up not being taken where the
        // address is invalid.  In that case, return a fault rather than trying
        // to translate it (which will cause a panic).  Since RISC-V allows
        // unaligned memory accesses, this should only happen if the request's
        // length is long enough to wrap around from the end of the memory to
        // the start.
        assert(req->getSize() > 0);
        if (req->getVaddr() + req->getSize() - 1 < req->getVaddr())
            return std::make_shared<GenericPageTableFault>(req->getVaddr());

        Process * p = tc->getProcessPtr();

        Fault fault = p->pTable->translate(req);
        if (fault != NoFault)
            return fault;

        return NoFault;
    }
}

Fault
TLB::translateAtomic(const RequestPtr &req, ThreadContext *tc,
                     BaseMMU::Mode mode)
{
    bool delayed;
    return translate(req, tc, nullptr, mode, delayed);
}

void
TLB::translateTiming(const RequestPtr &req, ThreadContext *tc,
                     BaseMMU::Translation *translation, BaseMMU::Mode mode)
{
    bool delayed;
    assert(translation);
    Fault fault = translate(req, tc, translation, mode, delayed);
    if (!delayed){
        translation->finish(fault, req, tc, mode);
    }
    else
        translation->markDelayed();
}

void
TLB::configVmodeInTLB(const RequestPtr &req, ThreadContext *tc,
                      BaseMMU::Mode mode)
{
    PrivilegeMode pmode = getMemPriv(tc, mode);
    SATP vsatp = tc->readMiscReg(MISCREG_VSATP);
    STATUS status = tc->readMiscReg(MISCREG_STATUS);
    HSTATUS hstatus = tc->readMiscReg(MISCREG_HSTATUS);
    int v_mode = tc->readMiscReg(MISCREG_VIRMODE);
    int two_stage_pmode = (int)getMemPriv(tc, mode);

    if (mode != BaseMMU::Execute) {
        if (status.mprv) {
            two_stage_pmode = status.mpp;
            v_mode = status.mpv && (two_stage_pmode != PrivilegeMode::PRV_M);
        }

        if (req->get_h_inst()) {
            v_mode = 1;
            two_stage_pmode = (PrivilegeMode)(RegVal)hstatus.spvp;
        }
    }
    if (v_mode != 0) {
        req->setVsatp0Mode(vsatp.mode == 0);
        req->setTwoStageState(true, v_mode, two_stage_pmode);
        req->setTwoPtwWalk(false, 2, 2, 0, false);
    }
}

void
TLB::configFunctional(const RequestPtr &req, ThreadContext *tc,
                       BaseMMU::Mode mode)
{
    PrivilegeMode pmode = getMemPriv(tc, mode);
    SATP vsatp = tc->readMiscReg(MISCREG_VSATP);
    HGATP hgatp = tc->readMiscReg(MISCREG_HGATP);
    SATP satp = tc->readMiscReg(MISCREG_SATP);

    if ((pmode == PrivilegeMode::PRV_M || satp.mode == AddrXlateMode::BARE)) {
        req->setTwoStageState(false, 0, 0);
        if ((hgatp.mode == 8 || vsatp.mode == 8) && (pmode < PrivilegeMode::PRV_M)) {
            configVmodeInTLB(req, tc, mode);
        }
    } else {
        if (hasTwoStageTranslation(tc, req, mode)) {
            configVmodeInTLB(req, tc, mode);
        } else {
            req->setTwoStageState(false, 0, 0);
        }
    }
}

Fault
TLB::translateFunctional(const RequestPtr &req, ThreadContext *tc,
                         BaseMMU::Mode mode)
{
    const Addr vaddr = req->getVaddr();
    Addr paddr = vaddr;

    if (FullSystem) {
        MMU *mmu = static_cast<MMU *>(tc->getMMUPtr());

        PrivilegeMode pmode = mmu->getMemPriv(tc, mode);
        SATP satp = tc->readMiscReg(MISCREG_SATP);
        SATP vsatp = tc->readMiscReg(MISCREG_VSATP);
        HGATP hgatp = tc->readMiscReg(MISCREG_HGATP);
        if ((pmode != PrivilegeMode::PRV_M &&
             satp.mode != AddrXlateMode::BARE) ||
            ((hgatp.mode == AddrXlateMode::SV39 || vsatp.mode == AddrXlateMode::SV39 ||
              hgatp.mode == AddrXlateMode::SV48 || vsatp.mode == AddrXlateMode::SV48) &&
             (pmode < PrivilegeMode::PRV_M))) {
            Walker *walker = mmu->getDataWalker();
            unsigned logBytes;
            configFunctional(req, tc, mode);
            Fault fault = walker->startFunctional(
                    req, tc, paddr, logBytes, mode);
            if (fault != NoFault)
                return fault;

            Addr masked_addr = vaddr & mask(logBytes);
            paddr |= masked_addr;
        }
    }
    else {
        Process *process = tc->getProcessPtr();
        const auto *pte = process->pTable->lookup(vaddr);

        if (!pte && mode != BaseMMU::Execute) {
            // Check if we just need to grow the stack.
            if (process->fixupFault(vaddr)) {
                // If we did, lookup the entry for the new page.
                pte = process->pTable->lookup(vaddr);
            }
        }

        if (!pte)
            return std::make_shared<GenericPageTableFault>(req->getVaddr());

        paddr = pte->paddr | process->pTable->pageOffset(vaddr);
    }

    DPRINTF(TLB, "Translated (functional) %#x -> %#x.\n", vaddr, paddr);
    req->setPaddr(paddr);
    pma->check(req);
    return pmp->pmpCheck(req, mode, static_cast<MMU *>(tc->getMMUPtr())->getMemPriv(tc, mode), tc);
}

void
TLB::translateFunctional(const RequestPtr &req, ThreadContext *tc,
                     BaseMMU::Translation *translation, BaseMMU::Mode mode)
{
    assert(translation);
    Fault fault = translateFunctional(req, tc, mode);
    translation->finish(fault, req, tc, mode);
}

Fault
TLB::finalizePhysical(const RequestPtr &req,
                      ThreadContext *tc, BaseMMU::Mode mode) const
{
    return NoFault;
}

void
TLB::serialize(CheckpointOut &cp) const
{
    // Only store the entries in use.
    printf("serialize\n");
    uint32_t _size = size - freeList.size();
    SERIALIZE_SCALAR(_size);
    SERIALIZE_SCALAR(lruSeq);

    uint32_t _count = 0;
    for (uint32_t x = 0; x < size; x++) {
        if (tlb[x].trieHandle != nullptr)
            tlb[x].serializeSection(cp, csprintf("Entry%d", _count++));
    }
}

void
TLB::unserialize(CheckpointIn &cp)
{
    // Do not allow to restore with a smaller tlb.
    printf("unserialize\n");
    uint32_t _size;
    UNSERIALIZE_SCALAR(_size);
    if (_size > size) {
        fatal("TLB size less than the one in checkpoint!");
    }

    UNSERIALIZE_SCALAR(lruSeq);

    for (uint32_t x = 0; x < _size; x++) {
        TlbEntry *newEntry = freeList.front();
        freeList.pop_front();

        newEntry->unserializeSection(cp, csprintf("Entry%d", x));
        Addr key = buildKey(newEntry->vaddr, newEntry->asid,0);
        newEntry->trieHandle = trie.insert(key,
            TlbEntryTrie::MaxBits - newEntry->logBytes + PGSHFT, newEntry);
    }
}

TLB::TlbStats::TlbStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(readHits, statistics::units::Count::get(), "read hits"),
      ADD_STAT(readMisses, statistics::units::Count::get(), "read misses"),
      ADD_STAT(readAccesses, statistics::units::Count::get(), "read accesses"),
      ADD_STAT(writeHits, statistics::units::Count::get(), "write hits"),
      ADD_STAT(writeMisses, statistics::units::Count::get(), "write misses"),
      ADD_STAT(writeAccesses, statistics::units::Count::get(),
               "write accesses"),
      ADD_STAT(readprefetchHits, statistics::units::Count::get(),
               "read prefetch Hits"),
      ADD_STAT(writeprefetchHits, statistics::units::Count::get(),
               "write prefetch Hits"),
      ADD_STAT(readprefetchAccesses, statistics::units::Count::get(),
               "read prefetch Accesses"),
      ADD_STAT(writeprefetchAccesses, statistics::units::Count::get(),
               "write prefetch Accesses"),
      ADD_STAT(readprefetchMisses, statistics::units::Count::get(),
               "read prefetch Misses"),
      ADD_STAT(writeprefetchMisses, statistics::units::Count::get(),
               "write prefetch Misses"),
      ADD_STAT(writeHitsSquashed, statistics::units::Count::get(),
               "write squashed hits"),
      ADD_STAT(readHitsSquashed, statistics::units::Count::get(),
               "read squashed hits"),
      ADD_STAT(squashedInsert, statistics::units::Count::get(),
               "number of squashed pte insert"),
      ADD_STAT(ALLInsert, statistics::units::Count::get(),
               "number of all pte insert"),
      ADD_STAT(backHits, statistics::units::Count::get(),
               "number of backHitst"),
      ADD_STAT(usedBackPre, statistics::units::Count::get(),
               "number of used back pre"),
      ADD_STAT(removeNoUseBackPre, statistics::units::Count::get(),
               "number of unused back pre"),
      ADD_STAT(usedForwardPre, statistics::units::Count::get(),
               "number of used forward pre"),
      ADD_STAT(removeNoUseForwardPre, statistics::units::Count::get(),
               "number of unused forward pre"),
      ADD_STAT(writeL2l0TlbMisses, statistics::units::Count::get(),
               "write misses in l2tlb"),
      ADD_STAT(ReadL2l0TlbMisses, statistics::units::Count::get(),
               "read misses in l2tlb"),
      ADD_STAT(writeL2Tlbl0Hits, statistics::units::Count::get(),
               "write hits in l2tlb"),
      ADD_STAT(ReadL2Tlbl0Hits, statistics::units::Count::get(),
               "read hits in l2tlb"),
      ADD_STAT(squashedInsertL2, statistics::units::Count::get(),
               "number of l2 squashe pte insert"),
      ADD_STAT(ALLInsertL2, statistics::units::Count::get(),
               "number of all l2 pte insert"),
      ADD_STAT(writeL2l0TlbSquashedHits, statistics::units::Count::get(),
               "l2 write squashed hits"),
      ADD_STAT(ReadL2l0TlbSquashedHits, statistics::units::Count::get(),
               "l2 read squashed hits"),
      ADD_STAT(l1tlbRemove, statistics::units::Count::get(),
               "l1tlb remove num"),
      ADD_STAT(l1tlbUsedRemove, statistics::units::Count::get(),
               "l1tlb used remove"),
      ADD_STAT(l1tlbUnusedRemove, statistics::units::Count::get(),
               "l1tlb unused remove"),
      ADD_STAT(l2tlbRemove, statistics::units::Count::get(),
               "l2tlb remove"),
      ADD_STAT(l2tlbUsedRemove, statistics::units::Count::get(),
               "l2sptlb used remove"),
      ADD_STAT(l2tlbUnusedRemove, statistics::units::Count::get(),
               "l2sptlb unused remove"),
      ADD_STAT(hitPreEntry, statistics::units::Count::get(),
               "number of pre entry hit"),
      ADD_STAT(hitPreNum, statistics::units::Count::get(),
               "number of pre hit times"),
      ADD_STAT(RemovePreUnused, statistics::units::Count::get(),
               "remove unused pre number"),
      ADD_STAT(AllPre, statistics::units::Count::get(), "all pre num"),
      ADD_STAT(hits, statistics::units::Count::get(),
               "Total TLB (read and write) hits", readHits + writeHits),
      ADD_STAT(misses, statistics::units::Count::get(),
               "Total TLB (read and write) misses", readMisses + writeMisses),
      ADD_STAT(accesses, statistics::units::Count::get(),
               "Total TLB (read and write) accesses",
               readAccesses + writeAccesses)
{
    l2tlbRemove
        .init(L_L2sp3 + 1)
        .flags(gem5::statistics::total);
    l2tlbUsedRemove
        .init(L_L2sp3 + 1)
        .flags(gem5::statistics::total);
    l2tlbUnusedRemove
        .init(L_L2sp3 + 1)
        .flags(gem5::statistics::total);
}

Port *
TLB::getTableWalkerPort()
{
    return &walker->getPort("port");
}

} // namespace gem5
