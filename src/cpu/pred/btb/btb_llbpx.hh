#ifndef __CPU_PRED_BTB_LLBPX_HH__
#define __CPU_PRED_BTB_LLBPX_HH__

#include <array>
#include <cstdlib>
#include <deque>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include <boost/dynamic_bitset.hpp>

#include "base/types.hh"
#include "cpu/o3/limits.hh"
#include "cpu/pred/btb/common.hh"
#include "cpu/pred/btb/llbpx_cache.hh"
#include "cpu/pred/btb/timed_base_pred.hh"

#ifndef UNIT_TEST
#include "base/statistics.hh"
#include "debug/LLBPX.hh"
#include "params/BTBLLBPX.hh"

#endif

namespace gem5
{
namespace branch_prediction
{
namespace btb_pred
{

#ifdef UNIT_TEST
namespace test {
#endif

class BTBTAGE;

class BTBLLBPX : public TimedBaseBTBPredictor
{
    static constexpr unsigned MaxThreads = o3::MaxThreads;
    static constexpr unsigned InstShiftAmt = 1;

  public:
#ifndef UNIT_TEST
    typedef BTBLLBPXParams Params;
    BTBLLBPX(const Params &p);
#else
    BTBLLBPX(bool adaptCtxDepth = false);
#endif

    void putPCHistory(Addr startAddr,
                      const boost::dynamic_bitset<> &history,
                      std::vector<FullBTBPrediction> &stagePreds) override;
    std::shared_ptr<void> getPredictionMeta(ThreadID tid = 0) override;
    void specUpdateGHist(const boost::dynamic_bitset<> &history,
                         FullBTBPrediction &pred,
                         const DirectionHistoryUpdate &update) override;
    void recoverHist(const boost::dynamic_bitset<> &history,
                     const FetchTarget &entry, int shamt,
                     bool cond_taken) override;
    void recoverPHist(const boost::dynamic_bitset<> &history,
                      const FetchTarget &entry,
                      const PathHistoryUpdate &update) override;
    void update(const FetchTarget &entry) override;
    void setTrace() override;
    void setTage(const BTBTAGE *tage);
    void onTageAllocation(const FetchTarget &entry, const BTBEntry &btbEntry,
                          const std::vector<unsigned> &providerDepths,
                          bool actualTaken);
    bool getProviderUpdateInfo(const FetchTarget &entry, Addr branchPC,
                               bool &llbpxPred, int &providerDepth);
#ifdef UNIT_TEST
    struct TestAccess;
#endif

  private:
    enum class RCRType : uint8_t
    {
        AllBranches = 0,
        CallsOnly = 1,
        CallsAndReturns = 2,
        UnconditionalOnly = 3,
        AllTakenBranches = 4,
    };

    struct PatternEntry
    {
        bool valid{false};
        Addr tag{0};
        short counter{0};
        uint64_t lastTouch{0};
        int providerDepth{0};

        void reset(Addr newTag, int depth = 0)
        {
            valid = true;
            tag = newTag;
            counter = 0;
            lastTouch = 0;
            providerDepth = depth;
        }

        bool taken() const { return counter >= 0; }
        int replacementScore() const { return std::abs(counter * 2 + 1); }
    };

    using PatternSet = llbpx::SparseSetAssociativeStore<PatternEntry>;

    struct ContextEntry
    {
        inline static unsigned defaultPatternSetCapacity = 64;
        inline static unsigned defaultPatternSetAssoc = 4;

        bool valid{false};
        Addr tag{0};
        Addr patternKey{0};
        uint16_t confidentPatterns{0};
        uint8_t confidence{0};
        uint64_t lastTouch{0};
        std::shared_ptr<PatternSet> patterns;

        ContextEntry() = default;

        void reset(Addr newTag)
        {
            valid = true;
            tag = newTag;
            patternKey = 0;
            confidentPatterns = 0;
            confidence = 0;
            lastTouch = 0;
            patterns = std::make_shared<PatternSet>(
                defaultPatternSetCapacity, defaultPatternSetAssoc);
        }

        int replacementScore() const { return confidence; }

        PatternSet &
        patternSet()
        {
            if (!patterns) {
                patterns = std::make_shared<PatternSet>(
                    defaultPatternSetCapacity, defaultPatternSetAssoc);
            }
            return *patterns;
        }

        const PatternSet *
        patternSetIfPresent() const
        {
            return patterns.get();
        }

        void sortPatterns(Addr key)
        {
            auto &set = patternSet().getSet(key);
            set.sort(
                [](const std::pair<Addr, PatternEntry> &lhs,
                   const std::pair<Addr, PatternEntry> &rhs)
                {
                    return std::abs(2 * lhs.second.counter + 1) >
                           std::abs(2 * rhs.second.counter + 1);
                });
        }
    };

    struct PatternBufferEntry
    {
        Addr cid{0};
        Tick lastUsed{0};
        bool dirty{false};
        bool locked{false};
        bool used{false};
        bool usedRetired{false};
        int dependants{0};
        Tick readyTick{0};
    };

    struct BranchMeta
    {
        struct TraceState
        {
            bool tageAllocTriggered{false};
            uint64_t tageAllocTableMask{0};
            uint64_t allocTableMask{0};
            uint64_t allocContextCreated{0};
            uint64_t allocContextRevisit{0};
            uint64_t allocPatternCreated{0};
            uint64_t allocPatternRevisit{0};
            uint64_t allocSkipNoKey{0};
            unsigned allocTargetWi{0};
            Addr allocTargetCid{0};
            bool updatePatternCalled{false};
            bool updatePatternFound{false};
            uint64_t counterBias{0};
            uint64_t counterBeforeBiased{0};
            uint64_t counterAfterBiased{0};
        };

        Addr startPC{0};
        Addr branchPC{0};
        ThreadID tid{0};
        uint8_t asidHash{0};
        bool basePred{false};
        bool llbpxPred{false};
        bool providerUsed{false};
        bool overridden{false};
        bool directionChanged{false};
        bool providerDepthEligible{false};
        bool providerTimingReady{true};
        bool providerInfoMissing{false};
        bool patternBufferInFlight{false};
        Addr cid{0};
        std::array<Addr, 2> cids{0, 0};
        Addr bcid{0};
        Addr pcid{0};
        Addr pbcid{0};
        unsigned wi{0};
        int hitHistIdx{-1};
        Addr key{0};
        unsigned keyTable{0};
        std::array<std::vector<Addr>, 2> tableKeysByWi;
        std::array<std::vector<bool>, 2> tableKeyValidByWi;
        bool contextHit{false};
        bool patternHit{false};
        int baseProviderHistIdx{-2};
        Addr contextTag{0};
        Addr patternTag{0};
        TraceState trace;
    };

    struct RCRRecord
    {
        Addr pc{0};
        Addr target{0};
        bool taken{false};
        bool isCond{false};
        bool isCall{false};
        bool isReturn{false};
    };

    struct RCRContextIds
    {
        Addr ccid{0};
        Addr pcid{0};
        Addr bcid{0};
        Addr pbcid{0};
    };

    struct LLBPXMeta
    {
        ThreadID tid{0};
        Addr startPC{0};
        uint8_t asidHash{0};
        boost::dynamic_bitset<> historySnapshot;
        std::deque<RCRRecord> rcrSnapshot;
        RCRContextIds rcrIdsSnapshot;
        std::array<Addr, 2> cidsSnapshot{0, 0};
        bool rcrUpdated{false};
        std::unordered_map<Addr, BranchMeta> branches;
    };

    struct ContextInfoEntry
    {
        bool valid{false};
        Addr tag{0};
        uint8_t wi{0};
        uint8_t fullPatternSets{0};
        uint8_t allocVsDrop{0};
        uint64_t lastTouch{0};

        void reset(Addr newTag)
        {
            valid = true;
            tag = newTag;
            wi = 0;
            fullPatternSets = 0;
            allocVsDrop = 0;
            lastTouch = 0;
        }

        int replacementScore() const
        {
            return fullPatternSets + allocVsDrop;
        }
    };

    using ContextStore = llbpx::SetAssociativeStore<ContextEntry>;
    using ContextInfoStore = llbpx::SetAssociativeStore<ContextInfoEntry>;

    struct ThreadState
    {
        std::shared_ptr<LLBPXMeta> meta;
        std::deque<RCRRecord> rcr;
        std::deque<PatternBufferEntry> patternBuffer;
        RCRContextIds rcrIds;
        Addr lastLookupCid{0};
    };

#ifndef UNIT_TEST
    struct LLBPXStats : public statistics::Group
    {
        statistics::Scalar lookup;
        statistics::Scalar lookupCidZero;
        statistics::Scalar lookupCidRepeatApprox;
        statistics::Scalar contextMiss;
        statistics::Scalar contextHit;
        statistics::Scalar patternMissAfterContextHit;
        statistics::Scalar patternHit;
        statistics::Vector lookupByDepthClass;
        statistics::Scalar providerUse;
        statistics::Scalar override;
        statistics::Scalar directionChange;
        statistics::Scalar allocContext;
        statistics::Scalar allocContextRevisit;
        statistics::Scalar allocPattern;
        statistics::Scalar allocPatternRevisit;
        statistics::Vector allocPatternByTable;
        statistics::Vector allocPatternByDepthClass;
        statistics::Scalar updateCalls;
        statistics::Scalar updateEntriesSeen;
        statistics::Scalar updateEntriesWithMeta;
        statistics::Scalar directTageAllocCalls;
        statistics::Scalar directTageAllocWithMeta;
        statistics::Scalar directTageAllocNoMeta;
        statistics::Scalar updatePattern;
        statistics::Scalar rcrRecover;
        statistics::Scalar providerFallback;
        statistics::Scalar providerRejectDepth;
        statistics::Scalar providerRejectTiming;
        statistics::Scalar patternBufferHit;
        statistics::Scalar patternBufferMiss;
        statistics::Scalar patternBufferNotReady;
        statistics::Scalar patternBufferInstall;
        statistics::Scalar patternBufferDemandPatternMiss;
        statistics::Scalar patternBufferPrefetchIssue;
        statistics::Scalar patternBufferPrefetchHit;
        statistics::Scalar patternBufferPrefetchDrop;
        statistics::Scalar cttEntryCreate;
        statistics::Scalar cttLookupMiss;
        statistics::Scalar cttDepthSwitchToShallow;
        statistics::Scalar cttDepthSwitchToDeep;

        LLBPXStats(statistics::Group *parent);
    };
#endif

    ThreadID predictorTid(const std::vector<FullBTBPrediction> &stagePreds) const;
    BranchMeta lookup(ThreadID tid, Addr startPC, const BTBEntry &entry,
                      bool basePred, const boost::dynamic_bitset<> &history,
                      uint8_t asidHash, int baseProviderHistIdx);
    void allocateFor(BranchMeta &meta, bool actualTaken,
                     const std::vector<unsigned> &providerDepths);
    void updatePattern(BranchMeta &meta, bool actualTaken);
    std::vector<BTBEntry> prepareUpdateEntries(const FetchTarget &entry) const;
    PatternBufferEntry *findPatternBuffer(ThreadID tid, Addr cid);
    bool rememberPatternBuffer(ThreadID tid, Addr cid, bool dirty,
                               Tick readyTick, bool isPrefetch);
    void usePatternBuffer(ThreadID tid, Addr cid);
    void commitPatternBuffer(ThreadID tid, Addr cid);
    void squashPatternBuffer(ThreadID tid, Addr cid);
    void prefetchContext(ThreadID tid);
    void initFilterTables();
    void restoreRCR(ThreadID tid, const LLBPXMeta &meta);
    void pushRCR(ThreadID tid, const BranchInfo &branch, bool taken);
    bool shouldRecordRCR(const BranchInfo &branch, bool taken) const;
    Addr calcRCRHash(ThreadID tid, unsigned n, unsigned skip, unsigned shift,
                     unsigned outBits) const;
    void recomputeRCRContextIds(ThreadID tid);
    bool contextExistsForPrefetch(Addr cid) const;
    unsigned selectContextDepth(Addr baseCtx) const;
    Addr contextIdForDepth(ThreadID tid, unsigned wi, bool prefetch = false) const;
    int filterCode(unsigned wi, unsigned table) const;
    void updateContextTracking(const BranchMeta &meta, short counterBefore,
                               short counterAfter, bool actualTaken);
    unsigned adaptContextDepth(const BranchMeta &meta,
                               const std::vector<unsigned> &providerTables);
    bool synthesizeMissingBranchMeta(const FetchTarget &entry,
                                     const LLBPXMeta &llbpxMeta,
                                     const BTBEntry &btbEntry,
                                     BranchMeta &branchMeta) const;
    void writeTraceRecord(const BranchMeta &meta, bool actualTaken);
    uint64_t tableMaskFor(const std::vector<unsigned> &tables) const;
    Addr patternKeyForSnapshotTable(const FetchTarget &entry, Addr startPC,
                                    Addr branchPC, Addr contextKey,
                                    unsigned wi, unsigned table,
                                    uint8_t asidHash) const;

    Addr hashBits(const boost::dynamic_bitset<> &history, unsigned bits) const;
    Addr mix(Addr value) const;
    Addr contextKey(ThreadID tid, Addr startPC, Addr branchPC,
                    const boost::dynamic_bitset<> &history,
                    uint8_t asidHash) const;
    Addr originalContextKey(ThreadID tid, Addr branchPC, unsigned window,
                            unsigned dist) const;
    Addr patternKey(Addr contextKey, Addr branchPC, uint8_t asidHash) const;
    Addr patternKeyForTable(ThreadID tid, Addr startPC, Addr branchPC,
                            Addr contextKey, unsigned wi, unsigned table,
                            uint8_t asidHash) const;
    Addr tagFromKey(Addr key) const;
    void updateCounter(bool taken, short &counter) const;

    const unsigned numThreads;
    const unsigned tageNumPredictors;
    const unsigned tagBits;
    const unsigned keyBits;
    const unsigned rcrEntries;
    const RCRType rcrType;
    const unsigned rcrWindow;
    const unsigned rcrDist;
    const unsigned rcrShift;
    const unsigned rcrTagBits;
    const unsigned rcrBaseTagBits;
    bool useOriginalRcr;
    const unsigned cttSets;
    const unsigned cttWays;
    const unsigned shallowContextDepth;
    const unsigned deepContextDepth;
    const unsigned trackingThreshold;
    const unsigned adaptThreshold;
    const unsigned histLenThreshold;
    const unsigned counterBits;
    const bool enableTiming;
    const bool adaptCtxDepth;
    const bool overrideOnlyOnDiff;
    const Tick patternBufferLatency;

    ContextStore contexts;
    ContextInfoStore ctt;
    std::array<std::vector<int>, 2> fltTables;
    unsigned patternBufferSize;
    std::vector<ThreadState> threadState;
    const BTBTAGE *tagePredictor{nullptr};

#ifndef UNIT_TEST
    LLBPXStats llbpxStats;
    TraceManager *llbpxTrace{nullptr};
#endif
};

#ifdef UNIT_TEST
struct BTBLLBPX::TestAccess
{
    static bool shouldRecordRCR(BTBLLBPX &llbpx, const BranchInfo &branch,
                                bool taken)
    {
        return llbpx.shouldRecordRCR(branch, taken);
    }

    static void pushRCR(BTBLLBPX &llbpx, ThreadID tid,
                        const BranchInfo &branch, bool taken)
    {
        llbpx.pushRCR(tid, branch, taken);
    }

    static void recomputeRCRContextIds(BTBLLBPX &llbpx, ThreadID tid)
    {
        llbpx.recomputeRCRContextIds(tid);
    }

    static Addr calcRCRHash(BTBLLBPX &llbpx, ThreadID tid, unsigned n,
                            unsigned skip, unsigned shift, unsigned outBits)
    {
        return llbpx.calcRCRHash(tid, n, skip, shift, outBits);
    }

    static Addr contextKey(BTBLLBPX &llbpx, ThreadID tid, Addr startPC,
                           Addr branchPC,
                           const boost::dynamic_bitset<> &history,
                           uint8_t asidHash)
    {
        return llbpx.contextKey(tid, startPC, branchPC, history, asidHash);
    }

    static Addr originalContextKey(BTBLLBPX &llbpx, ThreadID tid,
                                   Addr branchPC, unsigned window,
                                   unsigned dist)
    {
        return llbpx.originalContextKey(tid, branchPC, window, dist);
    }

    static const RCRContextIds &rcrIds(const BTBLLBPX &llbpx, ThreadID tid)
    {
        return llbpx.threadState.at(tid).rcrIds;
    }

    static const auto &rcr(const BTBLLBPX &llbpx, ThreadID tid)
    {
        return llbpx.threadState.at(tid).rcr;
    }

    static bool &useOriginalRcr(BTBLLBPX &llbpx)
    {
        return llbpx.useOriginalRcr;
    }

    static auto &contexts(BTBLLBPX &llbpx)
    {
        return llbpx.contexts;
    }

    static auto &ctt(BTBLLBPX &llbpx)
    {
        return llbpx.ctt;
    }

    static Addr contextTag(BTBLLBPX &llbpx, Addr cid, Addr branchPC)
    {
        return cid;
    }

    static Addr patternKey(BTBLLBPX &llbpx, Addr contextKey, Addr branchPC,
                           uint8_t asidHash)
    {
        return llbpx.patternKey(contextKey, branchPC, asidHash);
    }

    static Addr patternTag(BTBLLBPX &llbpx, Addr key, Addr branchPC,
                           uint8_t asidHash)
    {
        return key;
    }

    static auto &patternSet(ContextEntry &ctx)
    {
        return ctx.patternSet();
    }

    static Addr patternKeyForTable(BTBLLBPX &llbpx, ThreadID tid, Addr startPC,
                                   Addr branchPC, Addr contextKey,
                                   unsigned wi, unsigned table,
                                   uint8_t asidHash)
    {
        return llbpx.patternKeyForTable(tid, startPC, branchPC, contextKey,
                                        wi, table, asidHash);
    }

    static Addr contextIdForDepth(BTBLLBPX &llbpx, ThreadID tid, unsigned wi,
                                  bool prefetch = false)
    {
        return llbpx.contextIdForDepth(tid, wi, prefetch);
    }

    static int filterCode(BTBLLBPX &llbpx, unsigned wi, unsigned table)
    {
        return llbpx.filterCode(wi, table);
    }

    static BranchMeta lookup(BTBLLBPX &llbpx, ThreadID tid, Addr startPC,
                             const BTBEntry &entry, bool basePred,
                             const boost::dynamic_bitset<> &history,
                             uint8_t asidHash, int baseProviderHistIdx)
    {
        return llbpx.lookup(tid, startPC, entry, basePred, history, asidHash,
                            baseProviderHistIdx);
    }

    static PatternBufferEntry *findPatternBuffer(BTBLLBPX &llbpx, ThreadID tid,
                                                 Addr cid)
    {
        return llbpx.findPatternBuffer(tid, cid);
    }

    static bool rememberPatternBuffer(BTBLLBPX &llbpx, ThreadID tid, Addr cid,
                                      bool dirty, Tick readyTick,
                                      bool isPrefetch)
    {
        return llbpx.rememberPatternBuffer(tid, cid, dirty, readyTick,
                                           isPrefetch);
    }

    static std::shared_ptr<void> makeProviderMeta(ThreadID tid, Addr startPC,
                                                  Addr branchPC,
                                                  uint8_t asidHash, Addr cid,
                                                  Addr contextTag, Addr key,
                                                  bool patternBufferInFlight)
    {
        auto meta = std::make_shared<LLBPXMeta>();
        meta->tid = tid;
        meta->startPC = startPC;
        meta->asidHash = asidHash;

        BranchMeta branchMeta;
        branchMeta.startPC = startPC;
        branchMeta.branchPC = branchPC;
        branchMeta.tid = tid;
        branchMeta.asidHash = asidHash;
        branchMeta.providerUsed = true;
        branchMeta.patternBufferInFlight = patternBufferInFlight;
        branchMeta.cid = cid;
        branchMeta.cids = {cid, cid};
        branchMeta.wi = 0;
        branchMeta.key = key;
        branchMeta.keyTable = 0;
        for (unsigned snapshotWi = 0;
             snapshotWi < branchMeta.tableKeysByWi.size(); ++snapshotWi) {
            branchMeta.tableKeysByWi[snapshotWi] = {key};
            branchMeta.tableKeyValidByWi[snapshotWi] = {true};
        }
        branchMeta.contextHit = true;
        branchMeta.patternHit = true;
        branchMeta.contextTag = contextTag;
        branchMeta.patternTag = key;
        meta->branches.emplace(branchPC, branchMeta);
        return meta;
    }

    static void attachMeta(BTBLLBPX &llbpx, FetchTarget &entry,
                           const std::shared_ptr<void> &meta)
    {
        entry.predMetas[llbpx.getComponentIdx()] = meta;
    }

    static bool providerUsed(BTBLLBPX &llbpx, ThreadID tid, Addr branchPC)
    {
        if (tid >= llbpx.threadState.size()) {
            return false;
        }
        auto meta = llbpx.threadState[tid].meta;
        if (!meta) {
            return false;
        }
        auto it = meta->branches.find(branchPC);
        return it != meta->branches.end() && it->second.providerUsed;
    }
};
#endif

#ifdef UNIT_TEST
} // namespace test
#endif

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_BTB_LLBPX_HH__
