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

#include <array>

#include "arch/generic/mmu.hh"
#include "base/bitunion.hh"
#include "base/logging.hh"
#include "base/trie.hh"
#include "base/types.hh"
#include "sim/serialize.hh"

#ifndef MPT_ENABLED
#define MPT_ENABLED 1
#endif
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

#ifndef MPT_FORCE_ALLOW_PERMS
#define MPT_FORCE_ALLOW_PERMS 0
#endif


// -----------------------------
//Smmp52 related parameter definition.
// -----------------------------
#define MPT_LEVELS 4                 // Smmp52 uses levels L3 through L0.
#define MPT_MPTE_SIZE 8              // Each MPTE occupies 8 bytes.
#define MPT_NUM_PERMS 16             // Each MPTE controls the permissions of 16 sub-pages.
#define MPT_PERM_BITS_PER_ENTRY 3    // Each sub-page permission occupies 3 bits.
#define MPT_PERM_MASK 0x7            // Low 3-bit mask.

// Unit page size at each level. This is the single-page granularity, not the
// MPTE granularity.
#define MPT_LEAF_L0_PAGE_SIZE   (1UL << 12) // 4KB
#define MPT_LEAF_L1_PAGE_SIZE   (1UL << 21) // 2MB
#define MPT_LEAF_L2_PAGE_SIZE   (1UL << 30) // 1GB
#define MPT_LEAF_L3_PAGE_SIZE   (1UL << 39) // 512GB

// The coverage range of each MPTE (in bytes) = 16 × single page granularity.
#define MPT_REGION_SIZE_L0   (MPT_NUM_PERMS * MPT_LEAF_L0_PAGE_SIZE) // 64KB
#define MPT_REGION_SIZE_L1   (MPT_NUM_PERMS * MPT_LEAF_L1_PAGE_SIZE) // 32MB
#define MPT_REGION_SIZE_L2   (MPT_NUM_PERMS * MPT_LEAF_L2_PAGE_SIZE) // 16GB
#define MPT_REGION_SIZE_L3   (MPT_NUM_PERMS * MPT_LEAF_L3_PAGE_SIZE) // 8TB
// Smmpt52 permits this maximum range; Xiangshan does not require it today.

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

inline bool
mptPermAllowsAccess(uint8_t perm, BaseMMU::Mode mode)
{
    switch (mode) {
      case BaseMMU::Read:
        return perm & MPT_PERM_R;
      case BaseMMU::Write:
        return perm & MPT_PERM_W;
      case BaseMMU::Execute:
        return perm & MPT_PERM_X;
      default:
        return false;
    }
}

inline uint8_t
mptEffectivePerm(uint8_t rawPerm)
{
#if MPT_FORCE_ALLOW_PERMS
    (void)rawPerm;
    return MPT_PERM_R | MPT_PERM_W | MPT_PERM_X;
#else
    return rawPerm;
#endif
}

inline Addr
mptPermSlotAlign(Addr pa, int level)
{
    return pa & ~(getPageSizeForLevel(level) - 1);
}

inline bool
mptLevelCoversLogBytes(uint8_t level, unsigned logBytes)
{
    return static_cast<unsigned>(getPageShiftForLevel(level)) >= logBytes;
}

#endif //MPT_ENABLED

#if MPT_CACHE_ENABLED
struct MPTCacheEntry
{
    Addr tag;
    // Region base. The unordered map address is the lookup key; this tag is
    // kept for debug output and future set-associative mapping work.

    // Cached MPTE. This contains the raw 64-bit value, including the N bit.
    MPTE52 mpte;
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
    uint8_t raw;
    uint8_t mptlevel;
    bool valid;
    MPTInfoInTLB()
    {
        raw = 0;
        mptlevel = 0;
        valid = false;
    }

    void write_mpt_raw(uint8_t perm, uint8_t level)
    {
        raw = perm;
        mptlevel = level;
        valid = true;
    }

    void invalidate()
    {
        raw = 0;
        mptlevel = 0;
        valid = false;
    }
};
#endif //MPT_ENABLED


BitUnion64(MMPT)
    Bitfield<63, 60> mode;
    Bitfield<59, 58> zero;
    Bitfield<57, 52> sdid;
    Bitfield<51, 44> zero2;
    Bitfield<43, 0> ppn;
EndBitUnion(MMPT)

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
const Addr H_SV39X4_VADDR_BITS = 41;
const Addr H_SV48X4_VADDR_BITS = 50;
// Sv39 paging
const Addr SV39_VADDR_BITS = 39;
const Addr SV48_VADDR_BITS = 48;
const Addr LEVEL_BITS  = 9;
const Addr LEVEL_MASK = ((1 << LEVEL_BITS) - 1);
const Addr PGMASK = ((1 << 12) - 1);
const Addr TWO_STAGE_L2_LEVEL_MASK = 0x7ff;
const Addr VPN_MASK = 0x1ff;
const Addr PGSHFT = 12;
const Addr PTESIZE = 8;
const Addr L2PageTypeNum = 5; // l3/l2/l1/l0/sp
const Addr L2PageStoreTypeNum = 5;


const Addr L2TLB_BLK_OFFSET = 3;
const Addr VADDR_CHOOSE_MASK = 7;
const Addr l2tlbLineSize = 8;

const Addr preHitOnHitLNum = 500;
const double preHitOnHitPrecision = 0.08;
const double nextlinePrecision = 0.09;

const int L2L3CheckLevel = 3;
const int L2L2CheckLevel = 2;
const int L2L1CheckLevel = 1;
const int L2L0CheckLevel = 0;

const int L2L1LRU_NUM = 2;
const int L2L0LRU_NUM = 4;

// L2L3 :L2TLB L3Page
// L2L2 :L2TLB L2Page
// L2L1 :L2TLB L1Page
// L2L0 :L2TLB L0Page(leaf)
// L2sp3 :L2TLB L3Page(leaf)
// L2sp2 :L2TLB L2Page(leaf)
// L2sp1 :L2TLB L1Page(leaf)
enum l2TLBPage
{
    L_L2L3 =1,
    L_L2L2 ,
    L_L2L1 ,
    L_L2L0 ,
    L_L2sp3,
    L_L2sp2,
    L_L2sp1,
    L_L2SUM

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
    Bitfield<53, 46> ppn4;
    Bitfield<45, 37> ppn3;
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

    PTE pte;
    PTE pteVS;

    bool isCompressed;
    bool l1CompressedNarrow;
    uint8_t validIdx;
    uint8_t pteIdx;
    std::array<uint8_t, 8> ppnLow;
#if MPT_ENABLED
    MPTInfoInTLB mptInfo;
#endif //MPT_ENABLED

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
          isCompressed(false),
          l1CompressedNarrow(false),
          validIdx(0),
          pteIdx(0),
          ppnLow{},
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

inline Addr VADDR_SEXT(uint8_t addrXlateMode, Addr vaddr) {
    switch(addrXlateMode){
        case AddrXlateMode::BARE : return Addr(sext<SV48_VADDR_BITS>(vaddr));
        case AddrXlateMode::SV39 : return Addr(sext<SV39_VADDR_BITS>(vaddr));
        case AddrXlateMode::SV48 : return Addr(sext<SV48_VADDR_BITS>(vaddr));
        default: panic("addrXlateMode should be BARE/SV39/SV48.");
    }
}

inline int64_t H_VADDR_MASK(uint8_t addrXlateMode) {
    switch(addrXlateMode){
        case AddrXlateMode::BARE : return ((int64_t)1 << H_SV48X4_VADDR_BITS) - 1;
        case AddrXlateMode::SV39 : return ((int64_t)1 << H_SV39X4_VADDR_BITS) - 1;
        case AddrXlateMode::SV48 : return ((int64_t)1 << H_SV48X4_VADDR_BITS) - 1;
        default: panic("addrXlateMode should be BARE/SV39/SV48.");
    }
}

inline int PTW_TOP_LEVEL(uint8_t addrXlateMode) {
    switch(addrXlateMode){
        case AddrXlateMode::BARE : return 3;
        case AddrXlateMode::SV39 : return 2;
        case AddrXlateMode::SV48 : return 3;
        default: panic("addrXlateMode should be BARE/SV39/SV48.");
    }
}

} // namespace RiscvISA
} // namespace gem5

#endif // __ARCH_RISCV_PAGETABLE_H__
