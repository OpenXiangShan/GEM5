#include "mem/cache/prefetch/sandbox.hh"

#include <cassert>

#include "base/logging.hh"

namespace gem5
{
namespace prefetch
{

SandboxPrefetcher::SandboxStats::SandboxStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(trainings, statistics::units::Count::get(),
               "L2 training events observed by Sandbox"),
      ADD_STAT(bloomHits, statistics::units::Count::get(),
               "Sandbox Bloom hits on training events"),
      ADD_STAT(phases, statistics::units::Count::get(),
               "Completed Sandbox candidate evaluation phases"),
      ADD_STAT(rounds, statistics::units::Count::get(),
               "Completed Sandbox candidate evaluation rounds"),
      ADD_STAT(generated, statistics::units::Count::get(),
               "Sandbox real prefetch candidates generated"),
      ADD_STAT(filtered, statistics::units::Count::get(),
               "Sandbox candidates suppressed by the shared filter"),
      ADD_STAT(positiveGenerated, statistics::units::Count::get(),
               "Sandbox positive-direction prefetch candidates"),
      ADD_STAT(negativeGenerated, statistics::units::Count::get(),
               "Sandbox negative-direction prefetch candidates")
{
}

SandboxPrefetcher::SandboxPrefetcher(const SandboxPrefetcherParams &p)
    : Queued(p), statsSandbox(this),
      logic({p.prefetch_degree, p.detect_streams, p.stream_length,
             p.accesses_per_phase, p.cycle_offsets, p.bloom_bits,
             p.bloom_seed, 64, 4096})
{
}

void
SandboxPrefetcher::setParentInfo(System *sys, ProbeManager *pm,
                                  CacheAccessor *cache, unsigned blk_size)
{
    Queued::setParentInfo(sys, pm, cache, blk_size);
    fatal_if(blkSize != 64 || pageBytes != 4096,
             "Sandbox requires 64-byte lines and 4-KiB pages");
}

void
SandboxPrefetcher::calculatePrefetch(const PrefetchInfo &pfi,
                                     std::vector<AddrPriority> &addresses)
{
    assert(filter);
    ++statsSandbox.trainings;
    // The L2 composite trains and enqueues in the same address space as BOP.
    // Queued translates same-page virtual candidates when it inserts them.
    const auto result = logic.access(blockAddress(pfi.getAddr()));
    if (result.bloomHit)
        ++statsSandbox.bloomHits;
    if (result.phaseEnd)
        ++statsSandbox.phases;
    if (result.roundEnd)
        ++statsSandbox.rounds;

    for (const auto &prediction : result.predictions) {
        const Addr addr = prediction.address;
        ++statsSandbox.generated;
        ++prefetchStats.pfGenerated;
        // Match L2 BOP: buffer first, then consult the shared filter.
        InsertPFRequestToBuffer(AddrPriority(
            addr, 0, PrefetchSourceType::Sandbox, pfi.trigger_info));
        const Addr key = sharedFilterKey(pfi, addr);
        if (filter->contains(key)) {
            ++statsSandbox.filtered;
            ++prefetchStats.pfFiltered;
            continue;
        }
        filter->insert(key, 0);
        addresses.emplace_back(addr, 0, PrefetchSourceType::Sandbox);
        if (prediction.offset > 0)
            ++statsSandbox.positiveGenerated;
        else
            ++statsSandbox.negativeGenerated;
    }
}

} // namespace prefetch
} // namespace gem5
