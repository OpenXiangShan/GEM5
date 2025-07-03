/*
 * Copyright (c) 2002-2005 The Regents of The University of Michigan
 * Copyright (c) 2007 MIPS Technologies, Inc.
 * Copyright (c) 2020 Barkhausen Institut
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

#ifndef __ARCH_RISCV_PAGETABLE_H__
#define __ARCH_RISCV_PAGETABLE_H__

#include "base/bitunion.hh"
#include "base/logging.hh"
#include "base/trie.hh"
#include "base/types.hh"
#include "sim/serialize.hh"

namespace gem5
{

namespace RiscvISA {

BitUnion64(SATP)
    Bitfield<63, 60> mode;
    Bitfield<59, 44> asid;
    Bitfield<43, 0> ppn;
EndBitUnion(SATP)

enum AddrXlateMode
{
    BARE = 0,
    SV39 = 8,
    SV48 = 9,
};

const Addr H_VADDR_BITS = 41;
// Sv39 paging
const Addr VADDR_BITS  = 39;
const Addr LEVEL_BITS  = 9;
const Addr LEVEL_MASK = ((1 << LEVEL_BITS) - 1);
const Addr PGMASK = ((1 << 12) - 1);
const Addr TWO_STAGE_L2_LEVEL_MASK = 0x7ff;
const Addr VPN_MASK = 0x1ff;
const Addr PGSHFT = 12;
const Addr PTESIZE = 8;
const Addr L2PageTypeNum = 4;
const Addr L2PageStoreTypeNum = 5;

const Addr L2TLB_BLK_OFFSET = 3;
const Addr VADDR_CHOOSE_MASK = 7;
const Addr l2tlbLineSize = 8;

const Addr preHitOnHitLNum = 500;
const double preHitOnHitPrecision = 0.08;
const double nextlinePrecision = 0.09;

const int L2L1CheckLevel = 2;
const int L2L2CheckLevel = 1;
const int L2L3CheckLevel = 0;


// L2L1 :L2TLB L1Page
// L2L2 :L2TLB L2Page
// L2L3 :L2TLB L3Page
// L2sp1 :L2TLB L1Page(leaf)
// L2sp2 :L2TLB L2Page(leaf)
enum l2TLBPage
{
    L_L2L1 =1,
    L_L2L2 ,
    L_L2L3 ,
    L_L2sp1,
    L_L2sp2

};
enum HTLBHitState
{
    H_L1miss = 0,
    h_l1AllstageHit,
    h_l1VSstageHit,
    h_l1GstageHit,
    h_l2VSstageHitEnd,
    h_l2VSstageHitContinue,
    h_l2GstageHitEnd,
    h_l2GstageHitContinue
};

enum TlbTranslateMode { direct = 0, vsstage, gstage, allstage };

enum TranslateMode
{
    defaultmode = 0,
    twoStageMode = 1

};

enum MMUMode { MMU_DIRECT = 0, MMU_TRANSLATE = 1, MMU_DYNAMIC = 2 };

BitUnion64(PTESv39)
    Bitfield<53, 10> ppn;
    Bitfield<53, 28> ppn2;
    Bitfield<27, 19> ppn1;
    Bitfield<18, 10> ppn0;
    Bitfield<7> d;
    Bitfield<6> a;
    Bitfield<5> g;
    Bitfield<4> u;
    Bitfield<3, 1> perm;
    Bitfield<3> x;
    Bitfield<2> w;
    Bitfield<1> r;
    Bitfield<0> v;
EndBitUnion(PTESv39)


BitUnion64(PTESv48)
    Bitfield<53, 10> ppn;
    Bitfield<53, 37> ppn3;
    Bitfield<36, 28> ppn2;
    Bitfield<27, 19> ppn1;
    Bitfield<18, 10> ppn0;
    Bitfield<7> d;
    Bitfield<6> a;
    Bitfield<5> g;
    Bitfield<4> u;
    Bitfield<3, 1> perm;
    Bitfield<3> x;
    Bitfield<2> w;
    Bitfield<1> r;
    Bitfield<0> v;
EndBitUnion(PTESv48)


BitUnion64(PTE)
    Bitfield<53, 10> ppn;
    Bitfield<7> d;
    Bitfield<6> a;
    Bitfield<5> g;
    Bitfield<4> u;
    Bitfield<3, 1> perm;
    Bitfield<3> x;
    Bitfield<2> w;
    Bitfield<1> r;
    Bitfield<0> v;
EndBitUnion(PTE)

struct TlbEntry;
//struct L2TlbEntry;
typedef Trie<Addr, TlbEntry> TlbEntryTrie;
//typedef Trie<Addr, L2TlbEntry> L2TlbEntryTrie;

struct TlbEntry : public Serializable
{
    // The base of the physical page.
    Addr paddr;

    // The beginning of the virtual page this entry maps.
    Addr vaddr;
    Addr gpaddr;
    // The size of the page this represents, in address bits.
    unsigned logBytes;
    //transalte mode
    //0:direct 1:vsstage 2:gstage 3:allstage
    uint8_t translateMode;
    //vsatp.asid or satp.asid
    uint16_t asid;
    // hgatp.vmid
    uint16_t vmid;

    PTESv39 pte;
    PTESv39 pteVS;

    TlbEntryTrie::Handle trieHandle;

    // A sequence number to keep track of LRU.
    uint64_t lruSeq;

    uint64_t level;
    uint64_t VSlevel;

    Addr index;

    bool isSquashed;

    bool used;
    bool isPre;
    bool fromForwardPreReq;
    bool fromBackPreReq;
    bool preSign;

    TlbEntry()
        : paddr(0),
          vaddr(0),
          gpaddr(0),
          logBytes(0),
          translateMode(0),
          asid(0),
          vmid(0),
          pte(),
          pteVS(),
          lruSeq(0),
          level(0),
          VSlevel(0),
          index(0),
          isSquashed(false),
          used(false),
          isPre(false),
          fromForwardPreReq(false),
          fromBackPreReq(false),
          preSign(false)
    {
    }

    // Return the page size in bytes
    Addr size() const
    {
        return (static_cast<Addr>(1) << logBytes);
    }

    void serialize(CheckpointOut &cp) const override;
    void unserialize(CheckpointIn &cp) override;
};

} // namespace RiscvISA
} // namespace gem5

#endif // __ARCH_RISCV_PAGETABLE_H__
