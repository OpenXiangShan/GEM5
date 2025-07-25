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


inline int getPageShiftForLevel(int level){ // 返回每个层级的页大小对应的 log2 值
    switch (level) {
        case 0: return 12; // log2(4KB)
        case 1: return 21; // log2(2MB)
        case 2: return 30; // log2(1GB)
        case 3: return 39; // log2(512GB)
        default: panic("Invalid MPT level: %d", level);
    }
}

uint64_t getPageSizeForLevel(int level);
uint64_t getRegionSizeForLevel(int level);
uint8_t log2floor(uint64_t x);
bool checkMPTEPermissions(const MPTE52 &mpte, BaseMMU::Mode mode, Addr range_offset, int level);

//MPT Information
BitUnion32(MPTInfoRaw)
    Bitfield<31, 12>   reserved;        // 剩余 bits 是保留位
    Bitfield<11, 6>    mptLogBytes;     // 6位最大63, 支持 log2(64EB) =48, 覆盖所有 napot 层级
    Bitfield<4>        napot;           // N 位，napot 模式
    Bitfield<3>        perm_x;
    Bitfield<2>        perm_w;
    Bitfield<1>        perm_r;
    Bitfield<0>        valid;
EndBitUnion(MPTInfoRaw);

// -----------------------------
// MPT 权限位定义
// -----------------------------
#define MPT_PERM_R  (1 << 0)
#define MPT_PERM_W  (1 << 1)
#define MPT_PERM_X  (1 << 2)


// -----------------------------
// Smmp52 相关参数定义
// -----------------------------
#define MPT_LEVELS 4                 // Smmp52 使用 4 级页表（L3 → L0）
#define MPT_MPTE_SIZE 8              // 每个 MPTE 占 8 字节
#define MPT_NUM_PERMS 16             // 每个 MPTE 控制 16 个子页权限
#define MPT_PERM_BITS_PER_ENTRY 3    // 每个子页权限占 3 bit
#define MPT_PERM_MASK 0x7            // 低 3 位掩码

// 各层粒度：单位页大小（注意：不是 MPTE 粒度，是“单页粒度”）
#define MPT_LEAF_L0_PAGE_SIZE   (1UL << 12) // 4KB
#define MPT_LEAF_L1_PAGE_SIZE   (1UL << 21) // 2MB
#define MPT_LEAF_L2_PAGE_SIZE   (1UL << 30) // 1GB
#define MPT_LEAF_L3_PAGE_SIZE   (1UL << 39) // 512GB

// 每个 MPTE 覆盖范围（单位：字节） = 16 × 单页粒度
#define MPT_REGION_SIZE_L0   (MPT_NUM_PERMS * MPT_LEAF_L0_PAGE_SIZE) // 64KB
#define MPT_REGION_SIZE_L1   (MPT_NUM_PERMS * MPT_LEAF_L1_PAGE_SIZE) // 32MB
#define MPT_REGION_SIZE_L2   (MPT_NUM_PERMS * MPT_LEAF_L2_PAGE_SIZE) // 16GB
#define MPT_REGION_SIZE_L3   (MPT_NUM_PERMS * MPT_LEAF_L3_PAGE_SIZE) // 8TB     //我们香山没有这样的需求，不过smmpt52的最大支持是这样，将来可拓展。

struct MPTE52
{
    uint64_t raw;
    // 默认构造函数（无效项）
    MPTE52();
    // 用原始值构造
    MPTE52(uint64_t val);
    // 是否有效
    bool isValid() const;
    // 是否为叶子
    bool isLeaf() const;
    // N 位（bit 63）
    bool getN() const;
    // 下一层页表的物理页号（非叶子时使用）
    Addr nextLevelPPN() const;
    // 下一层页表物理地址（按 4KB 页对齐）
    Addr nextLevelPAddr() const;
    // 获取第 pi 个页的权限（pi ∈ [0, 15]）
    uint8_t perms(uint8_t pi) const;
};



struct MPTCacheEntry
{
    Addr tag;                  // region base（对齐后的地址）   目前这个 tag 用不上，用于查找的 key 是下面 unordered map 中的 Addr，此处 tag 的用处为增加调试信息 + 以后扩展为 set-ass 时可用
    MPTE52 mpte;               // 缓存的 MPTE  // 这个结构体包含 raw 64-bit 值，N 位在内部就有
    bool valid = false;

    //让 cache entry 自带粒度信息
    int level = -1;
    uint8_t log2RegionSize = 0;   //C++ 的 uint8_t 是8-bit
};
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
        // 根据 napot 与否判断是否使用 pi（子页索引）
        uint8_t pi = (rangeOffset >> getPageShiftForLevel(entry.level)) & 0xF;
        uint8_t perm = entry.mpte.perms(entry.mpte.getN() ? 0 : pi);

        MPTInfoInTLB info;
        info.raw = 0;
        info.raw.valid = entry.valid;
        info.raw.perm_r = (perm & MPT_PERM_R) != 0;
        info.raw.perm_w = (perm & MPT_PERM_W) != 0;
        info.raw.perm_x = (perm & MPT_PERM_X) != 0;
        info.raw.napot = entry.mpte.getN();

        // 不再重复推导，直接使用已有值(在 mpt.cc 中或 tlb.cc 中已近分别考虑过 N 位开启后的等效 size)
        info.raw.mptLogBytes = entry.log2RegionSize;

        return info;
    }
};




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

    MPTInfoInTLB mptInfo;// JJW

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
          preSign(false),
          mptInfo()
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
