#ifndef __MEM_CACHE_PREFETCH_DESPACITO_STREAM_HH__
#define __MEM_CACHE_PREFETCH_DESPACITO_STREAM_HH__

#include <vector>

#include <boost/compute/detail/lru_cache.hpp>

#include "base/sat_counter.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "debug/DespacitoStreamPrefetcher.hh"
#include "mem/cache/prefetch/associative_set.hh"
#include "mem/cache/prefetch/queued.hh"
#include "mem/packet.hh"
#include "params/DespacitoStreamPrefetcher.hh"

namespace gem5
{

struct DespacitoStreamPrefetcherParams;

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);

namespace prefetch
{

class DespacitoStreamPrefetcher : public Queued
{
  protected:
    struct SamplerEntry : TaggedEntry
    {
        uint64_t timestamp;
        Addr address;
        Addr pc;
        bool touched;
        SamplerEntry() : TaggedEntry(), timestamp(0), address(0), pc(0), touched(false) {}
    };

    struct PatternEntry : TaggedEntry
    {
        SatCounter8 conf;
        PatternEntry(SatCounter8 cnt) : TaggedEntry(), conf(cnt) {}
    };

    const uint64_t sampleRate;
    const uint64_t minDistance;
    const uint64_t maxDistance;

    AssociativeSet<SamplerEntry> sampler;
    AssociativeSet<PatternEntry> patterns;
    uint64_t timestamp;

    void updateSampler(const PrefetchInfo &pfi);

    void updatePatternTable(SamplerEntry *sampler_entry);

  public:
    boost::compute::detail::lru_cache<Addr, Addr> *filter;

    DespacitoStreamPrefetcher(const DespacitoStreamPrefetcherParams &p);

    void calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses) override
    {
        panic("not implemented");
    };

    void calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool late,
                           PrefetchSourceType pf_source, bool miss_repeat) override
    {
        panic("not implemented");
    };

    void calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool late);

    bool sendPFWithFilter(const PrefetchInfo &pfi, Addr addr, std::vector<AddrPriority> &addresses, int prio,
                          PrefetchSourceType src);
};

}

}


#endif
