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
    int badPreNum;
    bool enableAutoDepth;
    bool enableL3StreamPre;
    const int l2Ratio = 2;
    const int l3Ratio = 3;
    const int DEPTHRIGHT = 1 << 9;
    const int DEPTHLEFT = 1;
    const int DEPTHSTEP = 1;
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
        STREAMEntry() : TaggedEntry(), tag(0), bitVec(0), active(false), cnt(0), decrMode(false) {}
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
    using Queued::calculatePrefetch;
    void calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses) override
    {
        panic("not implemented");
    };
    void calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, int late_num);
};
}
}
#endif