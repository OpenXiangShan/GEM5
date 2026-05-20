//Created on 24-01-03
//choose stride or berti in sms

#ifndef __MEM_CACHE_PREFETCH_SMSSTRIDE_HH__
#define __MEM_CACHE_PREFETCH_SMSSTRIDE_HH__

#include <deque>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <boost/compute/detail/lru_cache.hpp>

#include "base/sat_counter.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "debug/XSStridePrefetcher.hh"
#include "mem/cache/prefetch/associative_set.hh"
// #include "mem/cache/prefetch/queued.hh"
#include "mem/packet.hh"
#include "params/XSStridePrefetcher.hh"
#include "mem/cache/prefetch/prefetch_filter.hh"
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
  const bool useRedundantTable;
  const bool fuzzyStrideMatching;
  const unsigned shortStrideThres;
  const bool strideDynDepth{false};
  const bool enableNonStrideFilter;
  const bool enableOracleSegmentedStride;
  const std::vector<Addr> oracleSegmentedStridePcs;
  const Addr oracleMajorStrideBytes;
  const Addr oracleMinorStrideBytes;
  const unsigned oracleSegmentLengthLines;
  const std::vector<unsigned> oracleStepOverrideOffsets;
  const std::vector<Addr> oracleStepOverrideBytes;
  const unsigned oracleL1WindowLines;
  const bool oracleEnableL1Prefetch;
  const unsigned oracleL2WindowLines;
  const unsigned oracleObserveToleranceLines;
  const unsigned oracleDeactivateMisses;
  const uint64_t oracleRecentHistoryWindowTicks;
  const bool oracleOverrideRegularStride;
  protected:
    const unsigned int regionSize;
    const unsigned int regionBlks;


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


    bool sendPFWithFilter(const PrefetchInfo &pfi, Addr addr,
                          std::vector<AddrPriority> &addresses, int prio,
                          PrefetchSourceType src, int ahead_level = -1,
                          int depth_override = -1);
    Addr strideHashPc(Addr pc);

    static constexpr uint64_t oracleInvalidStreamIndex =
        std::numeric_limits<uint64_t>::max();

    struct OracleTrackedLine
    {
        uint64_t streamIndex{oracleInvalidStreamIndex};
        uint64_t segmentId{oracleInvalidStreamIndex};
        int segmentOffset{-1};
        bool hadL2Issue{false};
        bool hadL1Issue{false};
        Tick l2IssueTick{0};
        Tick l1IssueTick{0};
    };

    struct OracleRecentLine
    {
        OracleTrackedLine tracked;
        Tick feedbackTick{0};
    };

    struct OracleStreamState
    {
        bool active{false};
        bool aligned{false};
        Addr baseHeadLine{0};
        uint64_t frontierIndex{0};
        Addr frontierLine{0};
        bool lastTriggerValid{false};
        Addr lastTriggerLine{0};
        bool lastFeedbackValid{false};
        Addr lastFeedbackLine{0};
        uint64_t unmatchedStreak{0};
        bool l1Armed{false};
        uint64_t nextL2IssueDemandIndex{0};
        uint64_t nextL1IssueDemandIndex{0};
        std::unordered_set<uint64_t> seenFuture;
    };

    std::unordered_set<Addr> oracleSegmentedStridePcSet;
    std::unordered_map<unsigned, Addr> oracleStepOverrideMap;
    std::vector<Addr> oracleSegmentPrefixBytes;
    OracleStreamState oracleStreamState;
    std::unordered_map<Addr, OracleTrackedLine> oracleOutstandingTargets;
    std::unordered_map<Addr, OracleTrackedLine> oracleOutstandingL1Targets;
    std::unordered_map<Addr, OracleRecentLine> oracleRecentLines;
    std::deque<std::pair<Addr, Tick>> oracleRecentLineOrder;
    int64_t oracleBoundaryDeltaBytes{0};

    bool isOracleSegmentedStridePC(Addr pc) const;
    void oracleResetStreamState(OracleStreamState &state);
    void oracleActivateFromLine(OracleStreamState &state, Addr line_addr,
                                bool aligned);
    void oracleDeactivate(OracleStreamState &state);
    Addr oracleStepBytesForOffset(unsigned segment_offset) const;
    Addr oracleLineForOffset(Addr segment_base_line,
                             unsigned segment_offset) const;
    Addr oracleLineForStreamIndex(const OracleStreamState &state,
                                  uint64_t stream_index) const;
    uint64_t oracleSegmentStartIndex(uint64_t stream_index) const;
    uint64_t oracleSegmentEndIndex(uint64_t stream_index) const;
    uint64_t oracleSegmentIdForIndex(uint64_t stream_index) const;
    int oracleSegmentOffsetForIndex(uint64_t stream_index) const;
    bool oracleFindMatchedIndex(const OracleStreamState &state,
                                Addr line_addr,
                                uint64_t &matched_index) const;
    void oracleTrackLaneIssue(Addr line_addr, uint64_t stream_index,
                              int target_level);
    void oraclePruneRecentLines(Tick now);
    void oracleRememberRecentLine(Addr line_addr,
                                  const OracleTrackedLine &tracked,
                                  Tick feedback_tick);
    OracleTrackedLine oracleFallbackTrackedLine(const OracleStreamState &state,
                                                Addr line_addr) const;
    void oracleClassifyDemandFeedback(const PrefetchInfo &pfi,
                                      const OracleTrackedLine &tracked,
                                      int observed_level);
    void oracleMaybeArmL1Lead(OracleStreamState &state);
    void oracleDrainL2Lane(const PrefetchInfo &pfi,
                           OracleStreamState &state,
                           std::vector<AddrPriority> &addresses);
    void oracleDrainL1Lane(const PrefetchInfo &pfi,
                           OracleStreamState &state,
                           std::vector<AddrPriority> &addresses);
    void oracleDrainReadyLanes(const PrefetchInfo &pfi,
                               OracleStreamState &state,
                               std::vector<AddrPriority> &addresses);
    void oracleAdvanceFrontier(const PrefetchInfo &pfi,
                               OracleStreamState &state,
                               std::vector<AddrPriority> &addresses);
    void oracleObserveFeedback(const PrefetchInfo &pfi);
    bool oracleGenerate(const PrefetchInfo &pfi,
                        std::vector<AddrPriority> &addresses);
    bool oracleIssuePrefetch(const PrefetchInfo &pfi,
                             std::vector<AddrPriority> &addresses,
                             const OracleStreamState &state,
                             uint64_t stream_index, int target_level);

  public:
    boost::compute::detail::lru_cache<Addr, Addr> *filter;
    boost::compute::detail::lru_cache<Addr, Addr> *filterL2;
    XSStridePrefetcher(const XSStridePrefetcherParams &p);

    void calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addressed) override
    {
        panic("not implemented");
    };
    using Queued::calculatePrefetch;

    void calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool late,
                           PrefetchSourceType pf_source, bool miss_repeat, bool enter_new_region, bool is_first_shot,
                           Addr &pf_addr, int64_t &learned_bop_offset);
    void triggerFromS1(const PrefetchInfo &pfi,
                       std::vector<AddrPriority> &addresses);
    void prefetchUnused(Addr paddr, PrefetchSourceType pfSource) override;
  PrefetchFilter* stridestream_pfFilter_l1;
  PrefetchFilter* stridestream_pfFilter_l2l3;

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
      statistics::Scalar oracleActivateCount;
      statistics::Scalar oracleBoundaryResyncCount;
      statistics::Scalar oracleMatchCount;
      statistics::Scalar oracleReplayCount;
      statistics::Scalar oracleAdvanceFrontierCount;
      statistics::Scalar oracleWindowMissCount;
      statistics::Scalar oracleDeactivateCount;
      statistics::Scalar oracleL2IssueAttemptCount;
      statistics::Scalar oracleL2IssueSentCount;
      statistics::Scalar oracleL2IssueSuppressedCount;
      statistics::Scalar oracleL1ArmCount;
      statistics::Scalar oracleL1IssueAttemptCount;
      statistics::Scalar oracleL1IssueSentCount;
      statistics::Scalar oracleL1IssueSuppressedCount;
      statistics::Scalar oracleL1IssuePriorL2Count;
      statistics::Scalar oracleL1IssueNoPriorL2Count;
      statistics::Scalar oracleFirstTouchL1HitWithLeadCount;
      statistics::Scalar oracleFirstTouchL1HitWithoutLeadCount;
      statistics::Scalar oracleFirstTouchMissWithLeadLowerCoveredCount;
      statistics::Scalar oracleFirstTouchMissWithLeadLowerUncoveredCount;
      statistics::Scalar oracleFirstTouchMissNoLeadLowerCoveredCount;
      statistics::Scalar oracleFirstTouchMissNoLeadLowerUncoveredCount;
      statistics::Scalar oracleEvictBeforeUseL1Count;

  } stats;
};
}

}
#endif
