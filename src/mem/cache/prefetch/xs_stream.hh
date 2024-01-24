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
#include "mem/cache/prefetch/queued.hh"
#include "mem/packet.hh"
#include "params/XsStreamPrefetcher.hh"

namespace gem5
{
struct XsStreamPrefetcherParams;
GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{
class XsStreamPrefetcher : public Queued
{
  protected:
    int depth;
    int bad_pre_num;
    const int l2Ratio = 2;
    const int l3Ratio = 3;
    const int BLOCK_OFFST = 6;
    const int BIT_VEC_WIDTH = 128;
    const int REGION_BITS = 7;
    const int REGION_TAG_OFFSET = 10;
    const int REGION_TAG_NUM = 16;
    const int ACTIVE_THRESHOLD = 12;
    const int VALIDITY_CHECK_INTERVAL = 1000;
    const int LATE_MISS_THRESHOLD = 200;
    const int LATE_HIT_THRESHOLD = 900;

    Addr tagAddress(Addr a) { return a >> REGION_TAG_OFFSET; };
    Addr tagOffset(Addr a) { return (a / blkSize) % REGION_TAG_NUM; };

    class STREAMEntry : public TaggedEntry
    {
      public:
        Addr tag;
        Addr bitVec;
        bool active;
        int cnt;
        bool decr_mode;
        STREAMEntry() : TaggedEntry(), tag(0), bitVec(0), active(false), cnt(0), decr_mode(false) {}
    };
    AssociativeSet<STREAMEntry> stream_array;
    STREAMEntry *streamLookup(const PrefetchInfo &pfi, bool &in_active_page, bool &decr);
    bool sendPFWithFilter(const PrefetchInfo &pfi, Addr addr, std::vector<AddrPriority> &addresses, int prio,
                          PrefetchSourceType src, int ahead_level = -1);

  public:
    boost::compute::detail::lru_cache<Addr, Addr> *filter;
    const unsigned pfFilterSize{256};
    boost::compute::detail::lru_cache<Addr, Addr> streamBlkFilter;
    XsStreamPrefetcher(const XsStreamPrefetcherParams &p);
    void calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses) override
    {
        panic("not implemented");
    };
    void calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, int late_num);
};
}
}
#endif