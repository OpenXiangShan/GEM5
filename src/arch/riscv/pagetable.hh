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

//#include "arch/riscv/mmu.hh"
#include "arch/generic/mmu.hh"
// Whether MPT is enabled (enabled by default, use `-D__ARCH_RISCV_MMU_MPT_HH__` to disable).
//#ifndef __ARCH_RISCV_MMU_MPT_HH__
#ifndef MPT_ENABLED
#define MPT_ENABLED 1
#endif
//#include "sim/stat_control.hh" 
//#else
//#define MPT_ENABLED 0
//#endif

// Whether MPT Cache is enabled (enabled by default, use `-D__ARCH_RISCV_MMU_MPT_CACHE_HH__` to disable), provided that MPT is enabled.																											
//#if MPT_ENABLED && !defined(__ARCH_RISCV_MMU_MPT_CACHE_HH__)
#define MPT_CACHE_ENABLED 1
//#include "params/RiscvTLB.hh" 
//#else
//#define MPT_CACHE_ENABLED 0
//#endif


#ifndef MPT_SIMULATE_N_BIT
#define MPT_SIMULATE_N_BIT 0
#endif


#ifndef MPT_CACHE_SIZE
#define MPT_CACHE_SIZE 128    //`MPT_CACHE_SIZE` is 128 by default.
#endif


namespace gem5
{

namespace RiscvISA {

#if MPT_ENABLED 
inline int getPageShiftForLevel(int level){ // Return the `log2` value corresponding to the page size for each level.
    switch (level) {
        case 0: return 12; // log2(4KB)
        case 1: return 21; // log2(2MB)
        case 2: return 30; // log2(1GB)
        case 3: return 39; // log2(512GB)
        default: panic("Invalid MPT level: %d", level);
    }
}



//MPT Information
BitUnion32(MPTInfoRaw)
    Bitfield<31, 12>   reserved;        // The remaining bits are reserved.
    Bitfield<11, 6>    mptLogBytes;  
        // 6 bits, with a maximum value of 63, supporting `log2(64EB) = 48`, covering all napot levels.
    Bitfield<4>        napot;           // N ，napot 
    Bitfield<3>        perm_x;
    Bitfield<2>        perm_w;
    Bitfield<1>        perm_r;
    Bitfield<0>        valid;
EndBitUnion(MPTInfoRaw);




#endif //MPT_ENABLED



#if MPT_ENABLED 
// -----------------------------
// MPT permission bit definition.
// -----------------------------
#define MPT_PERM_R  (1 << 0)
#define MPT_PERM_W  (1 << 1)
#define MPT_PERM_X  (1 << 2)


// -----------------------------
//Smmp52 related parameter definition.
// -----------------------------
#define MPT_LEVELS 4                 // Smmp52 uses a 4-level page table (L3 → L0).
#define MPT_MPTE_SIZE 8              // Each MPTE occupies 8 bytes.
#define MPT_NUM_PERMS 16             // Each MPTE controls the permissions of 16 sub-pages.
#define MPT_PERM_BITS_PER_ENTRY 3    // Each sub-page permission occupies 3 bits.
#define MPT_PERM_MASK 0x7            // Low 3-bit mask.

// Granularity at each level: unit page size (note: this is not the MPTE granularity, but the "single page granularity").
#define MPT_LEAF_L0_PAGE_SIZE   (1UL << 12) // 4KB
#define MPT_LEAF_L1_PAGE_SIZE   (1UL << 21) // 2MB
#define MPT_LEAF_L2_PAGE_SIZE   (1UL << 30) // 1GB
#define MPT_LEAF_L3_PAGE_SIZE   (1UL << 39) // 512GB

// The coverage range of each MPTE (in bytes) = 16 × single page granularity.
#define MPT_REGION_SIZE_L0   (MPT_NUM_PERMS * MPT_LEAF_L0_PAGE_SIZE) // 64KB
#define MPT_REGION_SIZE_L1   (MPT_NUM_PERMS * MPT_LEAF_L1_PAGE_SIZE) // 32MB
#define MPT_REGION_SIZE_L2   (MPT_NUM_PERMS * MPT_LEAF_L2_PAGE_SIZE) // 16GB
#define MPT_REGION_SIZE_L3   (MPT_NUM_PERMS * MPT_LEAF_L3_PAGE_SIZE) // 8TB     
    //We do not have such a requirement at Xiangshan, but the maximum supported by Smmpt52 is this, and it can be expanded in the future.

struct MPTE52
{
    uint64_t raw;
    // Default constructor (invalid entry).
    MPTE52();
    // Construct with the raw value.
    MPTE52(uint64_t val);
    // Whether it is valid.
    bool isValid() const;
    // Whether it is a leaf.
    bool isLeaf() const;
    // N （bit 63）
    bool getN() const;
    // Physical page number of the next-level page table (used when not a leaf).
    Addr nextLevelPPN() const;
    // Physical address of the next-level page table (aligned to 4KB pages).
    Addr nextLevelPAddr() const;
    // Retrieve the permissions of the `pi`-th page.（pi ∈ [0, 15]）
    uint8_t perms(uint8_t pi) const;
};

uint64_t getPageSizeForLevel(int level);
uint64_t getRegionSizeForLevel(int level);
uint8_t log2floor(uint64_t x);
bool checkMPTEPermissions(const MPTE52 &mpte, BaseMMU::Mode mode, Addr range_offset, int level);

#endif //MPT_ENABLED

#if MPT_CACHE_ENABLED
struct MPTCacheEntry
{
    Addr tag;                  
    // Region base (aligned address). Currently, this tag is not used; the key for lookup is the `Addr` in the unordered map below. 
    // The purpose of this tag is to add debug information and will be useful for future expansion when set-associative mapping is implemented.

    MPTE52 mpte;          // Cached MPTE // This structure contains the raw 64-bit value, with N bits already included internally.
    bool valid = false;

    //Make the cache entry carry its own granularity information.
    int level = -1;
    uint8_t log2RegionSize = 0;   

     // Use a static function to implement the alignment functionality.
    static Addr regionAlignStatic(Addr pa, int level) {
        // The same logic as `MPTCache52::regionAlign`.
        return pa & ~(getRegionSizeForLevel(level) - 1);
    }
};
#endif //MPT_CACHE_ENABLED

#if MPT_ENABLED 
struct MPTInfoInTLB
{
    MPTInfoRaw raw;
    MPTInfoInTLB()
    {
        raw = 0;
    }

    bool mptinfoTrust(uint8_t tlbLogBytes) const
    {
        return raw.valid && (raw.mptLogBytes >= tlbLogBytes);
    }

    static MPTInfoInTLB fromEntry(const MPTCacheEntry &entry, Addr rangeOffset)
    {
        // Determine whether to use the `pi` (subpage index) based on whether napot is enabled.
        uint8_t pi = (rangeOffset >> getPageShiftForLevel(entry.level)) & 0xF;
        uint8_t perm = entry.mpte.perms(entry.mpte.getN() ? 0 : pi);

        MPTInfoInTLB info;
        info.raw = 0;
        info.raw.valid = entry.valid;
        info.raw.perm_r = (perm & MPT_PERM_R) != 0;
        info.raw.perm_w = (perm & MPT_PERM_W) != 0;
        info.raw.perm_x = (perm & MPT_PERM_X) != 0;
        info.raw.napot = entry.mpte.getN();

        // No need to re-derive, directly use the existing value 
        //(the equivalent size after N bits are enabled has already been considered in `tlb.cc`).
        info.raw.mptLogBytes = entry.log2RegionSize;

        return info;
    }
};
#endif //MPT_ENABLED



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

    #if MPT_ENABLED 
    MPTInfoInTLB mptInfo;
    #endif //MPT_ENABLED


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
#if MPT_ENABLED
        , mptInfo()
#endif 
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
