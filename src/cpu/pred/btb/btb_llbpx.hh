#ifndef __CPU_PRED_BTB_LLBPX_HH__
#define __CPU_PRED_BTB_LLBPX_HH__

#include <cstdlib>
#include <deque>
#include <memory>
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

class BTBLLBPX : public TimedBaseBTBPredictor
{
    static constexpr unsigned MaxThreads = o3::MaxThreads;

  public:
#ifndef UNIT_TEST
    typedef BTBLLBPXParams Params;
    BTBLLBPX(const Params &p);
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

  private:
    struct PatternEntry
    {
        bool valid{false};
        Addr tag{0};
        short counter{0};
        uint8_t confidence{0};
        uint64_t lastTouch{0};
        int providerDepth{0};

        void reset(Addr newTag, int depth = 0)
        {
            valid = true;
            tag = newTag;
            counter = 0;
            confidence = 0;
            lastTouch = 0;
            providerDepth = depth;
        }

        bool taken() const { return counter >= 0; }
        int replacementScore() const { return std::abs(counter * 2 + 1) + confidence; }
    };

    struct ContextEntry
    {
        bool valid{false};
        Addr tag{0};
        Addr patternKey{0};
        uint8_t confidence{0};
        uint64_t lastTouch{0};

        void reset(Addr newTag)
        {
            valid = true;
            tag = newTag;
            patternKey = 0;
            confidence = 0;
            lastTouch = 0;
        }

        int replacementScore() const { return confidence; }
    };

    struct PatternBufferEntry
    {
        Addr key{0};
        Addr tag{0};
        bool dirty{false};
        Tick readyTick{0};
    };

    struct BranchMeta
    {
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
        Addr cid{0};
        Addr bcid{0};
        unsigned wi{0};
        int hitHistIdx{-1};
        Addr key{0};
        bool contextHit{false};
        bool patternHit{false};
        int baseProviderHistIdx{-2};
        Addr contextTag{0};
        Addr patternTag{0};
    };

    struct RCRRecord
    {
        Addr pc{0};
        Addr target{0};
        bool taken{false};
        bool isCond{false};
    };

    struct LLBPXMeta
    {
        ThreadID tid{0};
        Addr startPC{0};
        uint8_t asidHash{0};
        std::deque<RCRRecord> rcrSnapshot;
        bool rcrUpdated{false};
        std::unordered_map<Addr, BranchMeta> branches;
    };

    using ContextStore = llbpx::SetAssociativeStore<ContextEntry>;
    using PatternStore = llbpx::SetAssociativeStore<PatternEntry>;

    struct ThreadState
    {
        std::shared_ptr<LLBPXMeta> meta;
        std::deque<RCRRecord> rcr;
        std::deque<PatternBufferEntry> patternBuffer;
    };

#ifndef UNIT_TEST
    struct LLBPXStats : public statistics::Group
    {
        statistics::Scalar lookup;
        statistics::Scalar contextHit;
        statistics::Scalar patternHit;
        statistics::Scalar providerUse;
        statistics::Scalar override;
        statistics::Scalar directionChange;
        statistics::Scalar allocContext;
        statistics::Scalar allocPattern;
        statistics::Scalar updatePattern;
        statistics::Scalar rcrRecover;
        statistics::Scalar providerFallback;
        statistics::Scalar providerRejectDepth;
        statistics::Scalar providerRejectTiming;
        statistics::Scalar patternBufferHit;
        statistics::Scalar patternBufferMiss;
        statistics::Scalar patternBufferNotReady;
        statistics::Scalar patternBufferInstall;

        LLBPXStats(statistics::Group *parent);
    };
#endif

    ThreadID predictorTid(const std::vector<FullBTBPrediction> &stagePreds) const;
    BranchMeta lookup(ThreadID tid, Addr startPC, const BTBEntry &entry,
                      bool basePred, const boost::dynamic_bitset<> &history,
                      uint8_t asidHash, int baseProviderHistIdx);
    void allocateFor(const BranchMeta &meta, bool actualTaken);
    void updatePattern(const BranchMeta &meta, bool actualTaken);
    PatternBufferEntry *findPatternBuffer(ThreadID tid, Addr key, Addr tag);
    void rememberPattern(ThreadID tid, Addr key, Addr tag, bool dirty,
                         Tick readyTick);
    void restoreRCR(ThreadID tid, const LLBPXMeta &meta);
    void pushRCR(ThreadID tid, const BranchInfo &branch, bool taken);

    Addr hashBits(const boost::dynamic_bitset<> &history, unsigned bits) const;
    Addr mix(Addr value) const;
    Addr contextKey(ThreadID tid, Addr startPC, Addr branchPC,
                    const boost::dynamic_bitset<> &history,
                    uint8_t asidHash) const;
    Addr patternKey(Addr contextKey, Addr branchPC, uint8_t asidHash) const;
    Addr tagFromKey(Addr key) const;
    void updateCounter(bool taken, short &counter) const;

    const unsigned numThreads;
    const unsigned tagBits;
    const unsigned keyBits;
    const unsigned rcrEntries;
    const unsigned counterBits;
    const bool enableTiming;
    const bool adaptCtxDepth;
    const bool overrideOnlyOnDiff;
    const Tick patternBufferLatency;

    ContextStore contexts;
    PatternStore patterns;
    unsigned patternBufferSize;
    std::vector<ThreadState> threadState;

#ifndef UNIT_TEST
    LLBPXStats llbpxStats;
#endif
};

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_BTB_LLBPX_HH__
