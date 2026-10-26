#ifndef __MEM_CACHE_PREFETCH_SANDBOX_HH__
#define __MEM_CACHE_PREFETCH_SANDBOX_HH__

#include <vector>

#include <boost/compute/detail/lru_cache.hpp>

#include "base/statistics.hh"
#include "mem/cache/prefetch/queued.hh"
#include "mem/cache/prefetch/sandbox_logic.hh"
#include "params/SandboxPrefetcher.hh"

namespace gem5
{
GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

class SandboxPrefetcher : public Queued
{
  public:
    explicit SandboxPrefetcher(const SandboxPrefetcherParams &p);

    void setParentInfo(System *sys, ProbeManager *pm,
                       CacheAccessor *cache, unsigned blk_size) override;
    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses) override;

    boost::compute::detail::lru_cache<Addr, Addr> *filter = nullptr;

  private:
    struct SandboxStats : public statistics::Group
    {
        explicit SandboxStats(statistics::Group *parent);
        statistics::Scalar trainings;
        statistics::Scalar bloomHits;
        statistics::Scalar phases;
        statistics::Scalar rounds;
        statistics::Scalar generated;
        statistics::Scalar filtered;
        statistics::Scalar positiveGenerated;
        statistics::Scalar negativeGenerated;
    } statsSandbox;

    SandboxLogic logic;
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_SANDBOX_HH__
