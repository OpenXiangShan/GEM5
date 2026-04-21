//Created on 24-01-03
//choose stride or berti in sms

#ifndef __MEM_CACHE_PREFETCH_SMSSTRIDE_HH__
#define __MEM_CACHE_PREFETCH_SMSSTRIDE_HH__

#include <map>
#include <set>
#include <string>
#include <vector>

#include <boost/compute/detail/lru_cache.hpp>

#include "base/sat_counter.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "debug/XSStridePrefetcher.hh"
#include "mem/cache/prefetch/associative_set.hh"

// #include "mem/cache/prefetch/queued.hh"
#include "mem/cache/prefetch/prefetch_filter.hh"
#include "mem/packet.hh"
#include "params/XSStridePrefetcher.hh"

struct sqlite3;

namespace gem5
{

struct XSStridePrefetcherParams;

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);

namespace prefetch
{

class XSStridePrefetcher : public Queued
{
  protected:
  const bool useXsDepth;
  const bool enableAutoDepth;
  const bool useRedundantTable;
  const bool fuzzyStrideMatching;
  const unsigned shortStrideThres;
  const bool strideDynDepth{false};
  const bool enableNonStrideFilter;
  const bool enableTraceDb;
  const unsigned traceHartId;
  protected:
    const unsigned int regionSize;
    const unsigned int regionBlks;
    std::string traceDbFile;
    sqlite3 *traceDb = nullptr;
    bool ownTraceDb = false;
    std::string replayConfigTableName;
    std::string replayInputTableName;
    std::string replayCandidateTableName;
    uint64_t lastReplayInputId = 0;


    Addr regionAddress(Addr a) const { return a / regionSize; };

    Addr regionOffset(Addr a) const { return (a / blkSize) % regionBlks; }

  class StrideEntry : public TaggedEntry
    {
      public:
        int64_t stride;
        uint64_t lastAddr;
        SatCounter8 conf;
        int32_t depth;
        SatCounter8 lateConf;
        SatCounter8 longStride;
        Addr pc;
        std::list<Addr> histStrides;
        bool matchedSinceAlloc;
        StrideEntry()
            : TaggedEntry(),
              stride(0),
              lastAddr(0),
              conf(2, 0),
              depth(1),
              lateConf(4, 7),
              longStride(4, 7),
              pc(0)
        {}
    };

    const unsigned maxHistStrides{12};

    //const bool strideDynDepth{false};

    int depthDownCounter{0};

    const int depthDownPeriod{128};

    void periodStrideDepthDown();

    bool strideLookup(AssociativeSet<StrideEntry> &stride, const PrefetchInfo &pfi, std::vector<AddrPriority> &address,
                      bool late, Addr &pf_addr, PrefetchSourceType src, bool enter_new_region, bool miss_repeat,
                      int64_t &learned_bop_offset, bool is_first_shot);

    AssociativeSet<StrideEntry> strideUnique;

    AssociativeSet<StrideEntry> strideRedundant;

    class NonStrideEntry: public TaggedEntry
    {
      public:
        Addr pc;
        NonStrideEntry() : TaggedEntry(), pc(0) {}
    };

    AssociativeSet<NonStrideEntry> nonStridePCs;

    void markNonStridePC(Addr pc);


    bool isNonStridePC(Addr pc);

    Addr nonStrideHash(Addr pc) { return pc >> 1; }


    void sendPFWithFilter(const PrefetchInfo &pfi, Addr addr, std::vector<AddrPriority> &addresses, int prio,
                              PrefetchSourceType src, int ahead_level = -1);
    Addr strideHashPc(Addr pc);
    void initReplayTraceDb(const XSStridePrefetcherParams &p);
    void saveReplayTraceDb() const;
    void execReplayTraceSql(const std::string &sql) const;
    void recordReplayConfigTrace(const XSStridePrefetcherParams &p);
    void recordReplayInputTrace(const PrefetchInfo &pfi, bool late,
                                PrefetchSourceType pf_source, bool miss_repeat,
                                bool enter_new_region, bool is_first_shot);
    void recordReplayCandidateTrace(Addr trigger_addr, Addr trigger_pc,
                                    Addr pf_addr, int priority, bool pfahead,
                                    int pfahead_host, int ahead_level);
    struct DepthWindowSnapshot
    {
        uint64_t lateStrong;
        uint64_t lateMSHR;
        uint64_t lateCache;
        uint64_t timely;
    };
    DepthWindowSnapshot getDepthWindowSnapshot(int level) const;
    void recordDepthFeedbackTrace(const char *event_kind, int level,
                                  PrefetchSourceType pf_source, int pf_depth,
                                  int ahead_level) const;
    void recordDepthDecisionTrace(const char *site, int level,
                                  const DepthWindowSnapshot &pre_window,
                                  const DepthWindowSnapshot &post_window,
                                  uint64_t total_feedback,
                                  uint64_t feedback_window,
                                  uint64_t weighted_late,
                                  uint64_t weighted_total,
                                  const char *action,
                                  const char *reason,
                                  int old_l1_depth,
                                  int old_l2_gap) const;

    struct CommitTrainSnapshot
    {
        InstSeqNum seqNum;
        Addr addr;
        Addr pc;
        Cycles readyCycle;
        bool secure;
        bool readyForTrain;

        explicit CommitTrainSnapshot(const PrefetchInfo &pfi)
            : seqNum(pfi.getSeqNum()),
              addr(pfi.getAddr()),
              pc(pfi.getPC()),
              readyCycle(0),
              secure(pfi.isSecure()),
              readyForTrain(false)
        {}
    };

    std::map<InstSeqNum, CommitTrainSnapshot> pendingSnapshots;
    std::set<InstSeqNum> readyToTrain;
    EventFunctionWrapper commitTrainEvent;

    void scheduleCommitTrain();
    void processCommitTrain();
    void traceCommitOrderStage(const char *stage, const CommitTrainSnapshot &snapshot,
                               int queue_size, const char *reason) const;
    void traceCommitOrderStage(const char *stage, InstSeqNum seq_num,
                               Addr pc, Addr addr, bool is_load,
                               int queue_size, const char *reason) const;
    void triggerFromCommitTable(const PrefetchInfo &pfi,
                                std::vector<AddrPriority> &addresses);
    void trainFromSnapshot(const CommitTrainSnapshot &snapshot);
    void evaluateGlobalL1Depth();
    void evaluateGlobalL2Depth();
    statistics::Counter getGlobalL1DepthStat() const;
    statistics::Counter getGlobalL2DepthStat() const;
    statistics::Counter getGlobalL2GapStat() const;
    int getEffectiveGlobalL2Depth() const;

  public:
    boost::compute::detail::lru_cache<Addr, Addr> *filter;
    boost::compute::detail::lru_cache<Addr, Addr> *filterL2;
    XSStridePrefetcher(const XSStridePrefetcherParams &p);
    ~XSStridePrefetcher();

    void calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addressed) override
    {
        panic("not implemented");
    };
    using Queued::calculatePrefetch;

    void calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool late,
                           PrefetchSourceType pf_source, bool miss_repeat, bool enter_new_region, bool is_first_shot,
                           Addr &pf_addr, int64_t &learned_bop_offset);
    void captureAndTriggerFromS1(const PrefetchInfo &pfi,
                                 std::vector<AddrPriority> &addresses);
    void markCommitted(InstSeqNum seq_num);
    void dropYoungerThan(InstSeqNum boundary);
    void observeGlobalDepthFeedback(const PrefetchInfo &pfi, bool late,
                                    PrefetchSourceType pf_source);
    void observeGlobalDepthIssueLateCache(int ahead_level, int pf_depth);
    void observeGlobalDepthIssueLateMSHR(int ahead_level, int pf_depth);
    void observeGlobalDepthDownstreamDemandLate(int ahead_level, int pf_depth);
  PrefetchFilter* stridestream_pfFilter_l1;
  PrefetchFilter* stridestream_pfFilter_l2l3;

    static constexpr int globalL1DepthInit = 6;
    static constexpr int globalL1DepthMin = 1;
    static constexpr int globalL1DepthMax = 32;
    static constexpr int globalL2GapMin = 12;
    static constexpr int globalL2GapMax = 64;
    static constexpr int globalL2GapInit = globalL2GapMin;
    static constexpr int globalL2DepthInit = globalL1DepthInit + globalL2GapInit;
    static constexpr int globalL2DepthMax = 128;
    static constexpr uint64_t globalL1DepthFeedbackWindow = 256;
    static constexpr uint64_t globalL2DepthFeedbackWindow = 128;
    static constexpr uint64_t strongLateWeight = 4;
    static constexpr uint64_t issueLateMSHRWeight = 2;
    static constexpr uint64_t issueLateCacheWeight = 1;
    static constexpr uint64_t raiseThresholdPct = 20;
    static constexpr uint64_t lowerWeakLateThresholdPct = 5;
    static constexpr uint64_t lowerTimelyThresholdPct = 60;
    int globalL1Depth{globalL1DepthInit};
    int globalL2Gap{globalL2GapInit};
    uint64_t globalL1DepthLateStrongWindow{0};
    uint64_t globalL1DepthLateCacheWindow{0};
    uint64_t globalL1DepthLateMSHRWindow{0};
    uint64_t globalL1DepthTimelyFirstHitWindow{0};
    uint64_t globalL2DepthLateStrongWindow{0};
    uint64_t globalL2DepthLateCacheWindow{0};
    uint64_t globalL2DepthLateMSHRWindow{0};
    uint64_t globalL2DepthTimelyFirstHitWindow{0};

  struct XSstrideStats : public statistics::Group
  {
      XSstrideStats(statistics::Group *parent);
      statistics::Scalar strideUniquequeryCount;
      statistics::Scalar strideUniquehitCount;
      statistics::Scalar strideUniquemissCount;
      statistics::Scalar strideUniquepfCount;
      statistics::Scalar strideUniquereplaceusefulCount;
      statistics::Scalar strideRedundantqueryCount;
      statistics::Scalar strideRedundanthitCount;
      statistics::Scalar strideRedundantmissCount;
      statistics::Scalar strideRedundantpfCount;
      statistics::Scalar strideRedundantreplaceusefulCount;
      statistics::Scalar commitOrderedS1CaptureCount;
      statistics::Scalar commitOrderedS1DuplicateCount;
      statistics::Scalar commitOrderedReadyCount;
      statistics::Scalar commitOrderedSquashDropCount;
      statistics::Scalar commitOrderedDeferCount;
      statistics::Scalar commitOrderedTrainDispatchCount;
      statistics::Scalar commitOrderedTrainEnterCount;
      statistics::Scalar commitOrderedTrainFilteredNonStrideCount;
      statistics::Scalar commitOrderedTrainZeroStrideCount;
      statistics::Scalar commitOrderedTrainGuardedCount;
      statistics::Scalar commitOrderedTrainLongStrideAdjustCount;
      statistics::Scalar commitOrderedTrainUpdateCount;
      statistics::Scalar commitOrderedTrainAllocCount;
      statistics::Scalar commitOrderedTrainMatchCount;
      statistics::Scalar commitOrderedTrainMismatchCount;
      statistics::Scalar commitOrderedTrainRetargetCount;
      statistics::Value globalL1DepthCurrent;
      statistics::Value globalL2GapCurrent;
      statistics::Value globalL2DepthCurrent;
      statistics::Scalar globalL1DepthEvalCount;
      statistics::Scalar globalL2DepthEvalCount;
      statistics::Scalar globalL1DepthRaiseCount;
      statistics::Scalar globalL2DepthRaiseCount;
      statistics::Scalar globalL1DepthLowerCount;
      statistics::Scalar globalL2DepthLowerCount;
      statistics::Scalar globalL1DepthLateStrongCount;
      statistics::Scalar globalL2DepthLateStrongCount;
      statistics::Scalar globalL1DepthDownstreamDemandLateCount;
      statistics::Scalar globalL2DepthDownstreamDemandLateCount;
      statistics::Scalar globalL1DepthLateHitInCacheCount;
      statistics::Scalar globalL2DepthLateHitInCacheCount;
      statistics::Scalar globalL1DepthLateHitInMSHRCount;
      statistics::Scalar globalL2DepthLateHitInMSHRCount;
      statistics::Scalar globalL1DepthTimelyFirstHitCount;
      statistics::Scalar globalL2DepthTimelyFirstHitCount;

  } stats;
};
}

}
#endif
