/*
 * Copyright (c) 2014 The University of Wisconsin
 *
 * Copyright (c) 2006 INRIA (Institut National de Recherche en
 * Informatique et en Automatique  / French National Research Institute
 * for Computer Science and Applied Mathematics)
 *
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

/* @file
 * Implementation of a TAGE branch predictor. TAGE is a global-history based
 * branch predictor. It features a PC-indexed bimodal predictor and N
 * partially tagged tables, indexed with a hash of the PC and the global
 * branch history. The different lengths of global branch history used to
 * index the partially tagged tables grow geometrically. A small path history
 * is also used in the hash.
 *
 * All TAGE tables are accessed in parallel, and the one using the longest
 * history that matches provides the prediction (some exceptions apply).
 * Entries are allocated in components using a longer history than the
 * one that predicted when the prediction is incorrect.
 */

#ifndef __CPU_PRED_TAGE_BASE_HH__
#define __CPU_PRED_TAGE_BASE_HH__

#include <vector>

#include "base/statistics.hh"
#include "cpu/null_static_inst.hh"
#include "cpu/static_inst.hh"
#include "params/TAGEBase.hh"
#include "sim/sim_object.hh"

namespace gem5
{

namespace branch_prediction
{

class TAGEBase : public SimObject
{
  public:
    TAGEBase(const TAGEBaseParams &p);
    void init() override;

  protected:
    // Prediction Structures

    // Tage Entry
    struct TageEntry
    {
        int8_t ctr;     // 方向计数器
        uint16_t tag;   // 标签
        uint8_t u;      // useful bit
        TageEntry() : ctr(0), tag(0), u(0) { }
    };

    // Folded History Table - compressed history
    // to mix with instruction PC to index partially
    // tagged tables.
    // 折叠历史表 - 压缩历史
    // 将指令PC与部分标记表中的历史压缩在一起
    struct FoldedHistory
    {
        unsigned comp;   // 压缩的历史
        int compLength; // 压缩历史的长度， 压缩后比较短，5
        int origLength; // 原始历史的长度， 可能很长20
        int outpoint;   // 输出点
        int bufferSize; // 缓冲区大小

        FoldedHistory()
        {
            comp = 0;
        }

        void init(int original_length, int compressed_length)
        {
            origLength = original_length;
            compLength = compressed_length;
            outpoint = original_length % compressed_length; // 计算输出点=20%5=0
        }

        void update(uint8_t * h)
        {
            // 步骤1：左移并加入最新的历史位
            comp = (comp << 1) | h[0];
            // 例如：comp = 10101 变成 01010，然后加入h[0]=1，变成01011

            // 步骤2：异或旧的历史位（超出origLength的部分）
            comp ^= h[origLength] << outpoint;
            // 如果h[20]=1，则与comp异或，01011 ^ (1 << 0) = 01010

            // 步骤3：折叠操作
            comp ^= (comp >> compLength);
            // 将高位信息折叠到低位，01010 ^ (01010 >> 5) = 01010 ^ 00000 = 01010

            // 步骤4：保持在compLength位内
            comp &= (1ULL << compLength) - 1;
            // 确保结果不超过5位，01010 & 00011111 = 01010
        }
    };

  public:

    // provider type
    enum
    {
        BIMODAL_ONLY = 0, // 只用bimodal
        TAGE_LONGEST_MATCH, // 最长匹配
        BIMODAL_ALT_MATCH, // bimodal备用匹配
        TAGE_ALT_MATCH, // tage备用匹配
        LAST_TAGE_PROVIDER_TYPE = TAGE_ALT_MATCH
    };

    // 主分支历史条目， 类似meta信息，每次预测时候保存，更新时候使用
    struct BranchInfo
    {
        int pathHist;    // 路径历史, 记录分支指令地址，捕捉程序的执行路径模式
        int ptGhist;     // 全局历史，记录方向历史，捕捉分支指令相关性
        int hitBank;     // 命中表
        int hitBankIndex; // 命中表索引
        int altBank;     // 备用表
        int altBankIndex; // 备用表索引
        int bimodalIndex; // 双模式表索引

        bool tagePred;
        bool altTaken;
        bool condBranch;
        bool longestMatchPred; // 最长匹配预测
        bool pseudoNewAlloc; // 伪新分配
        Addr branchPC; // 分支PC

        // 指向动态分配的存储空间
        // 保存表索引和折叠历史。
        // 一次调用new而不是五个。
        int *storage;

        // 指向实际保存的数组
        // 在动态分配的存储空间中。都是数组
        int *tableIndices; // 表索引
        int *tableTags; // 表标签
        int *ci; // 压缩历史， 更新时候保存computeIndices
        int *ct0; // 0级表， 保存computeTags[0]
        int *ct1; // 1级表， 保存computeTags[1]

        // 用于统计目的
        unsigned provider;

        BranchInfo(const TAGEBase &tage)
            : pathHist(0), ptGhist(0),
              hitBank(0), hitBankIndex(0),
              altBank(0), altBankIndex(0),
              bimodalIndex(0),
              tagePred(false), altTaken(false),
              condBranch(false), longestMatchPred(false),
              pseudoNewAlloc(false), branchPC(0),
              provider(-1)
        {
            int sz = tage.nHistoryTables + 1; // 历史表的数量加1
            storage = new int [sz * 5]; // 分配存储空间, 返回一个指向新分配的内存块的指针
            tableIndices = storage; // 表索引
            tableTags = storage + sz; // 表标签
            ci = tableTags + sz; // 压缩历史
            ct0 = ci + sz; // 0级表
            ct1 = ct0 + sz; // 1级表
        }

        virtual ~BranchInfo()
        {
            delete[] storage;
        }
    };

    virtual BranchInfo *makeBranchInfo();

    /**
     * Computes the index used to access the
     * bimodal table.
     * @param pc_in The unshifted branch PC.
     */
    virtual int bindex(Addr pc_in) const;

    /**
     * Computes the index used to access a
     * partially tagged table.
     * @param tid The thread ID used to select the
     * global histories to use.
     * @param pc The unshifted branch PC.
     * @param bank The partially tagged table to access.
     */
    virtual int gindex(ThreadID tid, Addr pc, int bank) const;

    /**
     * Utility function to shuffle the path history
     * depending on which tagged table we are accessing.
     * @param phist The path history.
     * @param size Number of path history bits to use.
     * @param bank The partially tagged table to access.
     */
    virtual int F(int phist, int size, int bank) const;

    /**
     * Computes the partial tag of a tagged table.
     * @param tid the thread ID used to select the
     * global histories to use.
     * @param pc The unshifted branch PC.
     * @param bank The partially tagged table to access.
     */
    virtual uint16_t gtag(ThreadID tid, Addr pc, int bank) const;

    /**
     * Updates a direction counter based on the actual
     * branch outcome.
     * @param ctr Reference to counter to update.
     * @param taken Actual branch outcome.
     * @param nbits Counter width.
     */
    template<typename T>
    static void ctrUpdate(T & ctr, bool taken, int nbits);

    /**
     * Updates an unsigned counter based on up/down parameter
     * @param ctr Reference to counter to update.
     * @param up Boolean indicating if the counter is incremented/decremented
     * If true it is incremented, if false it is decremented
     * @param nbits Counter width.
     */
    static void unsignedCtrUpdate(uint8_t & ctr, bool up, unsigned nbits);

    /**
     * Get a branch prediction from the bimodal
     * predictor.
     * @param pc The unshifted branch PC.
     * @param bi Pointer to information on the
     * prediction.
     */
    virtual bool getBimodePred(Addr pc, BranchInfo* bi) const;

    /**
     * Updates the bimodal predictor.
     * @param pc The unshifted branch PC.
     * @param taken The actual branch outcome.
     * @param bi Pointer to information on the prediction
     * recorded at prediction time.
     */
    void baseUpdate(Addr pc, bool taken, BranchInfo* bi);

   /**
    * (Speculatively) updates the global branch history.
    * @param h Reference to pointer to global branch history.
    * @param dir (Predicted) outcome to update the histories
    * with.
    * @param tab
    * @param PT Reference to path history.
    */
    void updateGHist(uint8_t * &h, bool dir, uint8_t * tab, int &PT);

    /**
     * Update TAGE. Called at execute to repair histories on a misprediction
     * and at commit to update the tables.
     * @param tid The thread ID to select the global
     * histories to use.
     * @param branch_pc The unshifted branch PC.
     * @param taken Actual branch outcome.
     * @param bi Pointer to information on the prediction
     * recorded at prediction time.
     */
    void update(ThreadID tid, Addr branch_pc, bool taken, BranchInfo* bi);

   /**
    * (Speculatively) updates global histories (path and direction).
    * Also recomputes compressed (folded) histories based on the
    * branch direction.
    * @param tid The thread ID to select the histories
    * to update.
    * @param branch_pc The unshifted branch PC.
    * @param taken (Predicted) branch direction.
    * @param b Wrapping pointer to BranchInfo (to allow
    * storing derived class prediction information in the
    * base class).
    */
    virtual void updateHistories(
        ThreadID tid, Addr branch_pc, bool taken, BranchInfo* b,
        bool speculative,
        const StaticInstPtr & inst = nullStaticInstPtr,
        Addr target = MaxAddr);

    /**
     * Restores speculatively updated path and direction histories.
     * Also recomputes compressed (folded) histories based on the
     * correct branch outcome.
     * This version of squash() is called once on a branch misprediction.
     * @param tid The Thread ID to select the histories to rollback.
     * @param taken The correct branch outcome.
     * @param bp_history Wrapping pointer to BranchInfo (to allow
     * storing derived class prediction information in the
     * base class).
     * @param target The correct branch target
     * @post bp_history points to valid memory.
     */
    virtual void squash(
        ThreadID tid, bool taken, BranchInfo *bi, Addr target);

    /**
     * Update TAGE for conditional branches.
     * @param branch_pc The unshifted branch PC.
     * @param taken Actual branch outcome.
     * @param bi Pointer to information on the prediction
     * recorded at prediction time.
     * @nrand Random int number from 0 to 3
     * @param corrTarget The correct branch target
     * @param pred Final prediction for this branch
     * @param preAdjustAlloc call adjustAlloc before checking
     * pseudo newly allocated entries
     */
    virtual void condBranchUpdate(
        ThreadID tid, Addr branch_pc, bool taken, BranchInfo* bi,
        int nrand, Addr corrTarget, bool pred, bool preAdjustAlloc = false);

    /**
     * TAGE prediction called from TAGE::predict
     * @param tid The thread ID to select the global
     * histories to use.
     * @param branch_pc The unshifted branch PC.
     * @param cond_branch True if the branch is conditional.
     * @param bi Pointer to the BranchInfo
     */
    bool tagePredict(
        ThreadID tid, Addr branch_pc, bool cond_branch, BranchInfo* bi);

    /**
     * Update the stats
     * @param taken Actual branch outcome
     * @param bi Pointer to information on the prediction
     * recorded at prediction time.
     */
    virtual void updateStats(bool taken, BranchInfo* bi);

    /**
     * Instantiates the TAGE table entries
     */
    virtual void buildTageTables();

    /**
     * Calculates the history lengths
     * and some other paramters in derived classes
     */
    virtual void calculateParameters();

    /**
     * On a prediction, calculates the TAGE indices and tags for
     * all the different history lengths
     */
    virtual void calculateIndicesAndTags(
        ThreadID tid, Addr branch_pc, BranchInfo* bi);

    /**
     * Calculation of the index for useAltPredForNewlyAllocated
     * On this base TAGE implementation it is always 0
     */
    virtual unsigned getUseAltIdx(BranchInfo* bi, Addr branch_pc);

    /**
     * Extra calculation to tell whether TAGE allocaitons may happen or not
     * on an update
     * For this base TAGE implementation it does nothing
     */
    virtual void adjustAlloc(bool & alloc, bool taken, bool pred_taken);

    /**
     * Handles Allocation and U bits reset on an update
     */
    virtual void handleAllocAndUReset(
        bool alloc, bool taken, BranchInfo* bi, int nrand);

    /**
     * Handles the U bits reset
     */
    virtual void handleUReset();

    /**
     * Handles the update of the TAGE entries
     */
    virtual void handleTAGEUpdate(
        Addr branch_pc, bool taken, BranchInfo* bi);

    /**
     * Algorithm for resetting a single U counter
     */
    virtual void resetUctr(uint8_t & u);

    /**
     * Extra steps for calculating altTaken
     * For this base TAGE class it does nothing
     */
    virtual void extraAltCalc(BranchInfo* bi);

    virtual bool isHighConfidence(BranchInfo* bi) const
    {
        return false;
    }

    void btbUpdate(ThreadID tid, Addr branch_addr, BranchInfo* &bi);
    unsigned getGHR(ThreadID tid, BranchInfo *bi) const;
    int8_t getCtr(int hitBank, int hitBankIndex) const;
    unsigned getTageCtrBits() const;
    int getPathHist(ThreadID tid) const;
    bool isSpeculativeUpdateEnabled() const;
    size_t getSizeInBits() const;

  protected:
    const unsigned logRatioBiModalHystEntries; // 默认2, Log num of prediction entries for a shared hysteresis bit for the Bimodal
    const unsigned nHistoryTables; // 历史表的数量，默认7
    const unsigned tagTableCounterBits; // 表计数器位数， 默认3
    const unsigned tagTableUBits; // 表u位数， 默认2
    const unsigned histBufferSize; // 历史缓冲区大小，默认2M
    const unsigned minHist; // 最小历史大小，默认5
    const unsigned maxHist; // 最大历史大小，默认130
    const unsigned pathHistBits; // 路径历史位数，默认16

    std::vector<unsigned> tagTableTagWidths; // 表标签宽度，默认[0, 9, 9, 10, 10, 11, 11, 12]
    std::vector<int> logTagTableSizes; // 表大小对数，默认[13, 9, 9, 9, 9, 9, 9, 9]

    std::vector<bool> btablePrediction; // 表预测，默认[]
    std::vector<bool> btableHysteresis; // 表滞后，默认[]
    TageEntry **gtable; // 所有表，默认[]

    // Keep per-thread histories to
    // support SMT.
    struct ThreadHistory
    {
        // Speculative path history
        // (LSB of branch address)
        int pathHist; // 路径历史， 推测路径， 分支地址的LSB

        // Speculative branch direction
        // history (circular buffer)
        // @TODO Convert to std::vector<bool>
        uint8_t *globalHistory; // 全局历史， 推测分支方向历史， 循环缓冲区，// 完整的全局历史数组

        // Pointer to most recent branch outcome
        uint8_t* gHist; // 最近分支结果， 全局历史// 指向当前位置的指针，当前位置开始是真正的globalHistory！防止出现环形溢出， = &globalHistory[ptGhist]

        // Index to most recent branch outcome, 指向最近分支结果的索引，索引globalHistory
        int ptGhist;    // 索引globalHistory，保存位置信息，便于恢复

        // Speculative folded histories.
        FoldedHistory *computeIndices; // 某个折叠历史，用于计算索引， 每个tage表一个
        FoldedHistory *computeTags[2]; // 用于计算标签, 两个，压缩长度不同
    };

    std::vector<ThreadHistory> threadHistory; // 线程历史， 每个线程一个

    /**
     * Initialization of the folded histories， 初始化折叠历史
     */
    virtual void initFoldedHistories(ThreadHistory & history);

    int *histLengths; // 历史长度， 默认[5, 9, 13, 21, 33, 49, 73]
    int *tableIndices; // 表索引， 默认[]
    int *tableTags; // 表标签， 默认[]

    std::vector<int8_t> useAltPredForNewlyAllocated; // 用于新分配的替代预测， 默认[]
    int64_t tCounter; // 默认1 << 17
    uint64_t logUResetPeriod; // 默认18
    const int64_t initialTCounterValue; // 默认1 << 17
    unsigned numUseAltOnNa; // 默认1
    unsigned useAltOnNaBits; // 默认4
    unsigned maxNumAlloc; // 默认1

    // Tells which tables are active
    // (for the base TAGE implementation all are active)
    // Some other classes use this for handling associativity
    // 告诉哪些表是活动的
    // (对于基类TAGE实现，所有表都是活动的)
    // 其他类使用此方法处理关联性
    std::vector<bool> noSkip; // 默认[]

    const bool speculativeHistUpdate; // 默认True, 是否采用推测更新tage历史
    // 推测更新，更新及时，但需要存储大量元数据bi，还需要恢复机制
    // commit 更新，更新延迟大，但只需要存储少量元数据bi

    const unsigned instShiftAmt; // 默认0

    bool initialized;

    struct TAGEBaseStats : public statistics::Group
    {
        TAGEBaseStats(statistics::Group *parent, unsigned nHistoryTables);
        // stats
        statistics::Scalar longestMatchProviderCorrect; // 最长匹配提供者正确
        statistics::Scalar altMatchProviderCorrect; // 替代匹配提供者正确
        statistics::Scalar bimodalAltMatchProviderCorrect; // 双模式替代匹配提供者正确
        statistics::Scalar bimodalProviderCorrect; // 双模式提供者正确
        statistics::Scalar longestMatchProviderWrong; // 最长匹配提供者错误
        statistics::Scalar altMatchProviderWrong; // 替代匹配提供者错误
        statistics::Scalar bimodalAltMatchProviderWrong; // 双模式替代匹配提供者错误
        statistics::Scalar bimodalProviderWrong; // 双模式提供者错误
        statistics::Scalar altMatchProviderWouldHaveHit; // 替代匹配提供者会命中
        statistics::Scalar longestMatchProviderWouldHaveHit; // 最长匹配提供者会命中

        statistics::Vector longestMatchProvider; // 最长匹配提供者
        statistics::Vector altMatchProvider; // 替代匹配提供者
    } stats;
};

} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_TAGE_BASE_HH__
