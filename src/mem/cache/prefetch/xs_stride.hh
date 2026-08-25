//Created on 24-01-03
//choose stride or berti in sms

#ifndef __MEM_CACHE_PREFETCH_SMSSTRIDE_HH__
#define __MEM_CACHE_PREFETCH_SMSSTRIDE_HH__

#include <unordered_map>
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
  protected:
    const unsigned int regionSize;
    const unsigned int regionBlks;


    Addr regionAddress(Addr a) { return a / regionSize; };

    Addr regionOffset(Addr a) { return (a / blkSize) % regionBlks; }

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
        ContextID contextId;
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
              pc(0),
              contextId(InvalidContextID),
              matchedSinceAlloc(false)
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
        ContextID contextId;
        NonStrideEntry()
            : TaggedEntry(), pc(0), contextId(InvalidContextID)
        {}
    };

    AssociativeSet<NonStrideEntry> nonStridePCs;

    void markNonStridePC(Addr pc, ContextID context_id);


    bool isNonStridePC(Addr pc, ContextID context_id);

    Addr nonStrideHash(Addr pc) { return pc >> 1; }


    void sendPFWithFilter(const PrefetchInfo &pfi, Addr addr, std::vector<AddrPriority> &addresses, int prio,
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
