#ifndef __MEM_CACHE_PREFETCH_XSSTREAM_HH__
#define __MEM_CACHE_PREFETCH_XSSTREAM_HH__
#include <unordered_map>
#include <vector>

#include <boost/compute/detail/lru_cache.hpp>

#include "base/sat_counter.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "debug/XsStreamPrefetcher.hh"
#include "mem/cache/prefetch/associative_set.hh"
// #include "mem/cache/prefetch/queued.hh"
#include "mem/packet.hh"
#include "params/XsStreamPrefetcher.hh"
#include "mem/cache/prefetch/prefetch_filter.hh"
namespace gem5
{
struct XsStreamPrefetcherParams;
GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{
class XsStreamPrefetcher : public Queued
{
    protected:
    const unsigned int regionSize;
    const unsigned int regionBlks;


    Addr regionAddress(Addr a) { return a / regionSize; };

    Addr regionOffset(Addr a) { return (a / blkSize) % regionBlks; }
  protected:
    int depth;
    int badPreNum;
    bool enableAutoDepth;
    bool enableL3StreamPre;
    const unsigned l2Depth;
    const int l2Ratio = 2;
    const int l3Ratio = 3;
    const int DEPTHRIGHT = 1 << 9;
    const int DEPTHLEFT = 1;
    const int DEPTHSTEP = 1;
    const int L1BLKDEGREE = 2;
    const int L2BLKDEGREE = 4;
    const int L3BLKDEGREE = 8;
    const int BLOCKOFFST = 6;
    const int BITVECWIDTH = 128;
    const int REGIONBITS = 7;
    const int REGIONTAGOFFSET = 10;
    const int REGIONTAGNUM = 16;
    const int ACTIVETHRESHOLD = 12;
    const int VALIDITYCHECKINTERVAL = 1000;
    const double LATECOVERAGE = 0.4;
    const int LATEMISSTHRESHOLD = 200;
    const int LATEHITTHRESHOLD = 900;
    const int LOWMASK = 0x3ff;
    const int HIGHMASK = 0x7ff;
    const int VADDRHASHOFFSET = 5;
    const int VADDRHASHOFFSETMASK = 0x1f;

    struct StreamFeedback
    {
        uint64_t sent = 0;
        uint64_t tlbMisses = 0;
        uint64_t dcacheHits = 0;
        uint64_t pdbHits = 0;
        uint64_t mshrHits = 0;
        uint64_t demandMshrHits = 0;
        uint64_t pdbLoadUses = 0;
        uint64_t pdbUnusedReplacements = 0;
        uint64_t pdbRefills = 0;
        uint64_t pdbRefillIntervalSamples = 0;
        uint64_t pdbRefillIntervalCycles = 0;
        Tick lastPdbRefillTick = 0;
        uint64_t mshrResponseSamples = 0;
        uint64_t mshrResponseCycles = 0;
        uint64_t refillToUseSamples = 0;
        uint64_t refillToUseCycles = 0;
        uint64_t refillToReplaceSamples = 0;
        uint64_t refillToReplaceCycles = 0;
        uint64_t useToReplaceSamples = 0;
        uint64_t useToReplaceCycles = 0;
        uint64_t lateHits = 0;
    } feedback;

    struct FeedbackStats : statistics::Group
    {
        explicit FeedbackStats(XsStreamPrefetcher *parent);
        statistics::Scalar windows;
        statistics::Scalar sent;
        statistics::Scalar tlbMisses;
        statistics::Scalar dcacheHits;
        statistics::Scalar pdbHits;
        statistics::Scalar mshrHits;
        statistics::Scalar demandMshrHits;
        statistics::Scalar pdbLoadUses;
        statistics::Scalar pdbUnusedReplacements;
        statistics::Scalar pdbRefills;
        statistics::Scalar pdbRefillIntervalSamples;
        statistics::Scalar pdbRefillIntervalAvgCycles;
        statistics::Scalar mshrResponseSamples;
        statistics::Scalar mshrResponseAvgCycles;
        statistics::Scalar refillToUseSamples;
        statistics::Scalar refillToUseAvgCycles;
        statistics::Scalar refillToReplaceSamples;
        statistics::Scalar refillToReplaceAvgCycles;
        statistics::Scalar useToReplaceSamples;
        statistics::Scalar useToReplaceAvgCycles;
        statistics::Scalar depth;
    } feedbackStats;

    void completeFeedbackWindow();


    Addr tagAddress(Addr a) { return a >> REGIONTAGOFFSET; };
    Addr vaddrHash(Addr a)
    {
        int low = a & VADDRHASHOFFSETMASK;
        int mid = (a >> VADDRHASHOFFSET) & VADDRHASHOFFSETMASK;
        int high = (a >> (2 * VADDRHASHOFFSET)) & VADDRHASHOFFSETMASK;
        return low ^ mid ^ high;
    }
    Addr regionHashTag(Addr a)
    {
        int low = a & LOWMASK;
        int high = vaddrHash(a >> REGIONTAGOFFSET);
        return high << REGIONTAGOFFSET | low;
    }
    Addr tagOffset(Addr a) { return (a / blkSize) % REGIONTAGNUM; };

    class STREAMEntry : public TaggedEntry
    {
      public:
        Addr tag;
        Addr bitVec;
        bool active;
        int cnt;
        bool decrMode;
        ContextID contextId;
        STREAMEntry()
            : TaggedEntry(), tag(0), bitVec(0), active(false), cnt(0),
              decrMode(false), contextId(InvalidContextID)
        {}
    };
    AssociativeSet<STREAMEntry> stream_array;
    STREAMEntry *streamLookup(const PrefetchInfo &pfi, bool &in_active_page, bool &decr);
    void sendPFWithFilter(const PrefetchInfo &pfi, Addr addr, std::vector<AddrPriority> &addresses, int prio,
                          PrefetchSourceType src, int pf_degree, int ahead_level = -1, STREAMEntry *entry = nullptr);

  public:
    boost::compute::detail::lru_cache<Addr, Addr> *filter;
    const unsigned pfFilterSize{256};
    boost::compute::detail::lru_cache<Addr, Addr> streamBlkFilter;
    XsStreamPrefetcher(const XsStreamPrefetcherParams &p);
    using Queued::calculatePrefetch;
    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses) override;
    void recordStreamDequeued(PrefetchSourceType source)
    {
        Base::recordPrefetchDequeued(source);
        ++feedback.sent;
    }
    void recordStreamTlbMiss() { ++feedback.tlbMisses; }
    void recordStreamPdbRefill();
    void recordStreamMshrResponse(uint64_t latency_cycles);
    void recordStreamDemandMshrHit() { ++feedback.demandMshrHits; }
    void recordStreamProbe(PrefetchSourceType source,
                           Base::PrefetchProbeResult result);
    void recordStreamPdbFirstUse(bool load, uint64_t refill_to_use);
    void recordStreamPdbReplacement(bool used, uint64_t refill_to_replace,
                                    uint64_t use_to_replace);
    PrefetchFilter* stridestream_pfFilter_l1;
    PrefetchFilter* stridestream_pfFilter_l2l3;
};
}
}
#endif
