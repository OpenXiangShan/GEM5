//Created on 24-01-03
//choose stride or berti in sms

#ifndef __MEM_CACHE_PREFETCH_SMSSTRIDE_HH__
#define __MEM_CACHE_PREFETCH_SMSSTRIDE_HH__

#include <vector>

#include <boost/compute/detail/lru_cache.hpp>

#include "base/statistics.hh"
#include "base/types.hh"
#include "mem/cache/prefetch/associative_set.hh"
#include "mem/cache/prefetch/prefetch_filter.hh"
#include "mem/cache/prefetch/queued.hh"
#include "mem/cache/prefetch/xs_stride_state.hh"
#include "params/XSStridePrefetcher.hh"

namespace gem5
{

struct XSStridePrefetcherParams;

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);

namespace prefetch
{

class XSStridePrefetcher : public Queued
{
  protected:
    const unsigned strideHistoryBits;
    const unsigned int regionSize;
    const unsigned int regionBlks;

    Addr regionAddress(Addr a) { return a / regionSize; }
    Addr regionOffset(Addr a) { return (a / blkSize) % regionBlks; }

    class StrideEntry : public TaggedEntry
    {
      public:
        XSStrideState state;
    };

    AssociativeSet<StrideEntry> strideUnique;

    bool strideLookup(const PrefetchInfo &pfi,
                      std::vector<AddrPriority> &addresses,
                      bool is_first_shot);

    void sendPFWithFilter(const PrefetchInfo &pfi, Addr addr,
                          std::vector<AddrPriority> &addresses, int prio,
                          PrefetchSourceType src, int ahead_level = -1);
    Addr strideHashPc(Addr pc);

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

  } stats;
};
}

}
#endif
