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

/**
 * @brief A specialized prefetcher for tracking memory access patterns with multiple interleaved data streams.
 *
 * @details The DespacitoStreamPrefetcher targets memory access patterns where a single instruction
 * accesses multiple data streams in a short time period, with the next element of each stream
 * typically accessed by other instructions much later. From a data stream perspective, these
 * accesses exhibit next-line patterns, but from an instruction perspective, they appear random.
 *
 * This prefetcher addresses cases where:
 * - Traditional stride/BOP prefetchers fail because they cannot establish stable PC-localized offsets
 * - Conventional stream prefetchers cannot maintain state for the large number of interleaved streams
 *
 * The implementation uses a sampling approach to identify potential stream patterns and tracks
 * confidence in detected patterns to generate prefetches.
 *
 * Key components:
 * - Sampler: Samples recent memory accesses to detect instructions that access two consecutive
 *   memory blocks within a specified time window
 * - Pattern table: Records identified instruction PCs that exhibit the target access pattern
 *   along with their confidence levels
 * - LRU filter: Prevents redundant prefetches
 */
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
