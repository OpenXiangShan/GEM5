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
#include "arch/riscv/plru.hh"

namespace gem5
{

class ThreadContext;

/* To maintain compatibility with other architectures, we'll
   simply create an ITLB and DTLB that will point to the real TLB */
namespace RiscvISA {

struct MPTSenderState : public Packet::SenderState {
    ThreadContext *tc;
    BaseMMU::Translation *translation;

    MPTSenderState(ThreadContext *tc_, BaseMMU::Translation *tr_)
        : tc(tc_), translation(tr_) {}
};
struct MPT {

    Addr rootPPN; // 根页表物理页号（页号单位）
    //override:
    std::unordered_map<Addr, MPTE52> simulatedMPTMemory;
    Addr nextPPN;
    MPT(); // 默认构造函数
    MPTE52 simulateLeafAllowAll() const;
    MPTE52 simulateNonLeaf(Addr nextLevelPPN) const;
    Addr allocMPTPage(const std::vector<MPTE52>& entries);
    Addr buildSimulatedMPTTree(int levels = MPT_LEVELS);
    uint64_t readMPTE(Addr paddr, ThreadContext *tc, PMAChecker *pma, PMP *pmp, int &accessCounter) const;
    // Smmp52 多级 MPT 遍历，根据虚拟地址返回 MPTE52（或无效项）
    MPTE52 walk(Addr vaddr, ThreadContext *tc, PMAChecker *pma, PMP *pmp, int &accessCounter) const;
    
    //all miss, 127*4; L3 hit , else miss,  127*3.   L3 L2 hit , l1 l0 miss, 127*2.   L3 L2 L1 hit , l0 miss 127

    //新增：异步延迟 walk 接口，127 cycle 后触发回调返回结果
    void walkDelayed(Addr vaddr,
                     ThreadContext *tc,
                     PMAChecker *pma, PMP *pmp,
                     std::function<void(MPTE52)> callback) const;

};

class MPTCache52 {
  private:
    size_t capacity;
    size_t capacityL0;
    size_t capacityL1;
    size_t capacityL2;
    size_t capacityL3;
    size_t capacitySP;

	  static int configuredSize;//JJW2  用来存 param 传进来的值（类全局共享）
	
    std::unordered_map<Addr, MPTCacheEntry> table;
	
    std::unordered_map<Addr, MPTCacheEntry> tableL0;
    std::unordered_map<Addr, MPTCacheEntry> tableL1;
    std::unordered_map<Addr, MPTCacheEntry> tableL2;
    std::unordered_map<Addr, MPTCacheEntry> tableL3;
    std::unordered_map<Addr, MPTCacheEntry> tableSP;

    // -------- PLRU 替换支持 --------
    // 每级 tag 顺序 & 替换路径
    // 每级 cache 的 tag 顺序表 + 对应的 PLRU 树
    std::vector<Addr> tagListL0;
    std::vector<Addr> tagListL1;
    std::vector<Addr> tagListL2;
    std::vector<Addr> tagListL3;
    std::vector<Addr> tagListSP;

    PLRUTreeN plruL0 = PLRUTreeN(1); // 默认构造，稍后 resize
    PLRUTreeN plruL1 = PLRUTreeN(1);
    PLRUTreeN plruL2 = PLRUTreeN(1);
    PLRUTreeN plruL3 = PLRUTreeN(1);
    PLRUTreeN plruSP = PLRUTreeN(1);
	// -------------------------------
	
    Addr regionAlign(Addr pa, int level) const;

    // MPTCache Hit 
    mutable statistics::Scalar mptCacheL0Hits;
    mutable statistics::Scalar mptCacheL1Hits;
    mutable statistics::Scalar mptCacheL2Hits;
    mutable statistics::Scalar mptCacheL3Hits;
    mutable statistics::Scalar mptCacheSPHits;

    // MPTCache Miss
    mutable statistics::Scalar mptCacheL0Misses;
    mutable statistics::Scalar mptCacheL1Misses;
    mutable statistics::Scalar mptCacheL2Misses;
    mutable statistics::Scalar mptCacheL3Misses;
    mutable statistics::Scalar mptCacheSPMisses;

  public:
    MPTCache52(size_t capL0, size_t capL1, size_t capL2, size_t capL3, size_t capSP);
    MPTCache52();

    static int configuredSizeL0;
    static int configuredSizeL1;
    static int configuredSizeL2;
    static int configuredSizeL3;
    static int configuredSizeSP;
	
    static void configureSize(int sL0, int sL1, int sL2, int sL3, int sSP);

    void initMPTCacheFromParams(const RiscvTLBParams *params);
    
    // -------- PLRU 替换支持 --------
    std::vector<Addr>& getTagListByLevel(int level);
    PLRUTreeN& getPLRUByLevel(int level);
    // -------------------------------


    // 非 const 版本：允许修改
    std::unordered_map<Addr, MPTCacheEntry>& getTableByLevel(int level);

    // const 版本：只读
    const std::unordered_map<Addr, MPTCacheEntry>& getTableByLevel(int level) const;

    // 允许修改
    size_t& getCapacityByLevel(int level);

    // 只读版本（如果需要在 const 函数中读取容量）
    size_t getCapacityByLevel(int level) const;

    void fetchDelayed(
          Addr pa,
          int level,
          const MPT &mpt,
          ThreadContext *tc,
          PMAChecker *pma, PMP *pmp,
          std::function<void(bool /*hit*/, MPTCacheEntry)> callback) const;
          //callback是一个函数指针的封装，类型是：std::function<void(bool, MPTCacheEntry)>

};



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

  public:
    Addr L2TLB_L2_MASK;
    Addr L2TLB_L3_MASK;

  protected:
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
    uint64_t controlNum;
    uint64_t allForwardPre;
    uint64_t removeNoUseForwardPre;
    uint64_t removeNoUseBackPre;
    uint64_t usedBackPre;
    uint64_t test_num;
    uint64_t allUsed;
    uint64_t forwardUsedPre;
    uint64_t lastVaddr;
    uint64_t lastPc;
    uint64_t traceFlag;

    bool use_old_priv;
    PrivilegeMode old_priv_ldst;
    PrivilegeMode old_priv_ex;

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

        statistics::Vector l2tlbRemove;
        statistics::Vector l2tlbUsedRemove;
        statistics::Vector l2tlbUnusedRemove;


        statistics::Scalar hitPreEntry;
        statistics::Scalar hitPreNum;
        statistics::Scalar RemovePreUnused;
        statistics::Scalar AllPre;

        statistics::Formula hits;
        statistics::Formula misses;
        statistics::Formula accesses;

          //MPT Cache Miss/Hit Statistics
        statistics::Scalar mptL0Hits;
        statistics::Scalar mptL0Misses;
        statistics::Formula mptL0Accesses;
        statistics::Formula mptL0HitRate;
        statistics::Formula mptL0MissRate;

        statistics::Scalar mptL1Hits;
        statistics::Scalar mptL1Misses;
        statistics::Formula mptL1Accesses;
        statistics::Formula mptL1HitRate;
        statistics::Formula mptL1MissRate;

        statistics::Scalar mptL2Hits;
        statistics::Scalar mptL2Misses;
        statistics::Formula mptL2Accesses;
        statistics::Formula mptL2HitRate;
        statistics::Formula mptL2MissRate;

        statistics::Scalar mptL3Hits;
        statistics::Scalar mptL3Misses;
        statistics::Formula mptL3Accesses;
        statistics::Formula mptL3HitRate;
        statistics::Formula mptL3MissRate;

        statistics::Scalar mptSPHits;
        statistics::Scalar mptSPMisses;
        statistics::Formula mptSPAccesses;
        statistics::Formula mptSPHitRate;
        statistics::Formula mptSPMissRate;

        // Total MPT 
        statistics::Formula mptTotalHits;
        statistics::Formula mptTotalMisses;
        statistics::Formula mptTotalAccesses;
        statistics::Formula mptHitRate;
        statistics::Formula mptMissRate;

    } stats;

  public:
    PMAChecker *pma;
    PMP *pmp;

  public:
    typedef RiscvTLBParams Params;
    TLB(const Params &p);

    Walker *getWalker();

    void takeOverFrom(BaseTLB *old) override {}
    void setPTWmode(bool _enable_sv48) override;

    TlbEntry *insert(Addr vpn, const TlbEntry &entry, bool suqashed_update, uint8_t translateMode);
    TlbEntry *insertForwardPre(Addr vpn, const TlbEntry &entry);
    TlbEntry *insertBackPre(Addr vpn, const TlbEntry &entry);
    void configL2Tlb(EntryList *List_choose, TlbEntryTrie *Trie_l2_choose, std::vector<TlbEntry> &l2Tlb_choose,
                     size_t size, bool sp);

    TlbEntry *L2TLBInsert(Addr vpn, const TlbEntry &entry, int level, int choose, int sign, bool squashed_update,
                          uint8_t translateMode);
    TlbEntry *L2TLBInsertIn(Addr vpn, const TlbEntry &entry, int choose, EntryList *List, TlbEntryTrie *Trie_l2,
                            int sign, bool squashed_update, uint8_t translateMode);
    // TlbEntry *L2TLB_insert_in(Addr vpn,const TlbEntry &entry,int level);


    void flushAll() override;
    void demapPage(Addr vaddr, uint64_t asn) override;
    void demapPageL2(Addr vaddr,uint64_t asn);

    Fault checkPermissions(STATUS status, PrivilegeMode pmode, Addr vaddr, BaseMMU::Mode mode, PTESv39 pte,
                           Addr gpaddr, bool G);
    Fault checkGuestPermissions(STATUS status, PrivilegeMode pmode, Addr vaddr,
                      BaseMMU::Mode mode, PTESv39 pte);
    std::pair<bool, Fault> checkGPermissions(STATUS status, Addr vaddr, Addr gpaddr, BaseMMU::Mode mode, PTESv39 pte,
                                             bool h_inst);
    Fault createPagefault(Addr vaddr, Addr gPaddr,BaseMMU::Mode mode,bool G);

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

    Addr translateWithTLB(Addr vaddr, uint16_t asid, BaseMMU::Mode mode, uint8_t translateMode);

    Fault L2TLBPagefault(Addr vaddr, BaseMMU::Mode mode, const RequestPtr &req, bool is_pre, bool is_back_pre);

    Fault L2TLBCheck(PTESv39 pte, int level, STATUS status, PrivilegeMode pmode, Addr vaddr, BaseMMU::Mode mode,
                     const RequestPtr &req, bool is_pre, bool is_back_pre);
    bool checkPrePrecision(uint64_t &removeNoUsePre, uint64_t &usedPre);
    void sendPreHitOnHitRequest(TlbEntry *e_pre_1, TlbEntry *e_pre_2, const RequestPtr &req, Addr pre_block,
                                uint16_t asid, bool forward, int check_level, STATUS status, PrivilegeMode pmode,
                                BaseMMU::Mode mode, ThreadContext *tc, BaseMMU::Translation *translation);
    std::pair<bool, Fault> L2TLBSendRequest(Fault fault, TlbEntry *e_l2tlb, const RequestPtr &req, ThreadContext *tc,
                                            BaseMMU::Translation *translation, BaseMMU::Mode mode, Addr vaddr,
                                            bool &delayed, int level);

    Fault translateAtomic(const RequestPtr &req,
                          ThreadContext *tc, BaseMMU::Mode mode) override;
    void translateTiming(const RequestPtr &req, ThreadContext *tc,
                         BaseMMU::Translation *translation,
                         BaseMMU::Mode mode) override;
    void configVmodeInTLB(const RequestPtr &req, ThreadContext *tc,
                      BaseMMU::Mode mode);
    void configFunctional(const RequestPtr &req, ThreadContext *tc,
                       BaseMMU::Mode mode);
    Fault translateFunctional(const RequestPtr &req, ThreadContext *tc,
                              BaseMMU::Mode mode) override;
    void  translateFunctional(const RequestPtr &req, ThreadContext *tc,
                              BaseMMU::Translation *translation, BaseMMU::Mode mode) override;
    Fault finalizePhysical(const RequestPtr &req, ThreadContext *tc,
                           BaseMMU::Mode mode) const override;
    TlbEntry *lookup(Addr vpn, uint16_t asid, BaseMMU::Mode mode, bool hidden, bool sign_used, uint8_t translateMode);
    TlbEntry *lookupForwardPre(Addr vpn, uint64_t asid, bool hidden);
    TlbEntry *lookupBackPre(Addr vpn, uint64_t asid, bool hidden);
    bool autoOpenNextline();
    TlbEntry *lookupL2TLB(Addr vpn, uint16_t asid, BaseMMU::Mode mode, bool hidden, int f_level, bool sign_used,
                          uint8_t translateMode);

    void setOldPriv(ThreadContext *tc) {
      use_old_priv = true;
      old_priv_ex = getMemPriv(tc, BaseMMU::Execute);
      old_priv_ldst = getMemPriv(tc, BaseMMU::Read);
    }
    void useNewPriv(ThreadContext *tc) {
      use_old_priv = false;
    }


    


    void checkMPTPermInTLB(TlbEntry* entry, Addr vaddr, Addr paForMPTCheck, BaseMMU::Mode mode,
      ThreadContext *tc,
      BaseMMU::Translation *translation,
      //  #if MPT_ENABLED
      const MPT& mpt,
  //    #if MPT_CACHE_ENABLED
      MPTCache52* cache,
  // //    #endif
  //  #endif
      RequestPtr req
 
  );
	
	// #if MPT_ENABLED
		// 根据 logBytes 推导出 MPT 的层级（L0~L3）
		inline int getLevelForPageSizeLog2(uint8_t logBytes);
	// #endif
	
	// #if MPT_ENABLED
    Fault createMPTPagefault(Addr vaddr, Addr paForMPTCheck, BaseMMU::Mode mode);
  // #endif



    std::vector<TlbEntry> tlbL2L1;  // our TLB
    TlbEntryTrie trieL2L1;          // for next line
    EntryList freeListL2L1;         // free entries

    std::vector<TlbEntry> tlbL2L2;  // our TLB
    TlbEntryTrie trieL2L2;          // for next line
    EntryList freeListL2L2;         // free entries

    std::vector<TlbEntry> tlbL2L3;  // our TLB
    TlbEntryTrie trieL2L3;          // for next line
    EntryList freeListL2L3;         // free entries

    std::vector<TlbEntry> tlbL2Sp;  // our TLB
    TlbEntryTrie trieL2sp;          // for next line
    EntryList freeListL2sp;         // free entries


    std::vector<TlbEntry> forwardPre;
    TlbEntryTrie trieForwardPre;
    EntryList freeListForwardPre;

    std::vector<TlbEntry> backPre;
    TlbEntryTrie trieBackPre;
    EntryList freeListBackPre;

    std::vector<TlbEntry *> l2Tlb;
    std::vector<size_t> l2TlbSize;
    std::vector<TlbEntryTrie *> l2Trie;
    std::vector<EntryList *> l2Freelist;

  private:
    uint64_t nextSeq() { return ++lruSeq; }
    void updateL2TLBSeq(TlbEntryTrie *Trie_l2,Addr vpn,Addr step, uint16_t asid,uint8_t translateMode);


    void evictLRU();
    void evictForwardPre();
    void evictBackPre();

    void l2TLBEvictLRU(int l2TLBlevel, Addr vaddr);

    void remove(size_t idx);
    void removeForwardPre(size_t idx);
    void removeBackPre(size_t idx);
    void l2tlbRemoveIn(EntryList *List, TlbEntryTrie *Trie_l2,std::vector<TlbEntry>&tlb,size_t idx, int choose);
    void l2TLBRemove(size_t idx, int choose);
    bool hasTwoStageTranslation(ThreadContext *tc, const RequestPtr &req, BaseMMU::Mode mode);
    Fault misalignDataAddrCheck(const RequestPtr &req, BaseMMU::Mode mode);
    MMUMode isaMMUCheck(ThreadContext *tc, Addr vaddr, BaseMMU::Mode mode);


    Fault translate(const RequestPtr &req, ThreadContext *tc,
                    BaseMMU::Translation *translation, BaseMMU::Mode mode,
                    bool &delayed);
    Fault doTwoStageTranslate(const RequestPtr &req, ThreadContext *tc,
                      BaseMMU::Translation *translation, BaseMMU::Mode mode,
                      bool &delayed);
    std::pair<int, Fault> checkHL1Tlb(const RequestPtr &req, ThreadContext *tc, BaseMMU::Translation *translation,
                                      BaseMMU::Mode mode);
    std::pair<int, Fault> checkHL2Tlb(const RequestPtr &req, ThreadContext *tc, BaseMMU::Translation *translation,
                                      BaseMMU::Mode mode, int l1tlbtype);
    Fault doTranslate(const RequestPtr &req, ThreadContext *tc,
                      BaseMMU::Translation *translation, BaseMMU::Mode mode,
                      bool &delayed);

};

} // namespace RiscvISA
} // namespace gem5

#endif // __RISCV_MEMORY_HH__
