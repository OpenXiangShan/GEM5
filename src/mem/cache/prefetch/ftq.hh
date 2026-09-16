/*
 * Copyright (c) 2026 Beijing Institute of Open Source Chip
 * All rights reserved.
 */

#ifndef __MEM_CACHE_PREFETCH_FTQ_HH__
#define __MEM_CACHE_PREFETCH_FTQ_HH__

#include <deque>
#include <unordered_map>

#include "mem/cache/prefetch/queued.hh"
#include "params/FTQICachePrefetcher.hh"

namespace gem5
{

namespace prefetch
{

/**
 * Prefetcher driven by addresses selected by the decoupled instruction FTQ.
 * The normal cache-probe training path is deliberately disabled; Fetch is the
 * source of the prefetch stream.
 */
class FTQICachePrefetcher : public Queued
{
  private:
    struct PendingHint
    {
        FTQPrefetchHint hint;
        Tick readyAt = 0;
    };

    std::deque<PendingHint> pendingHints;
    std::unordered_map<ThreadID, uint64_t> generations;
    EventFunctionWrapper hintEvent;

    struct FTQStats : public statistics::Group
    {
        FTQStats(statistics::Group *parent);
        statistics::Scalar hintsQueued;
        statistics::Scalar hintsDispatched;
        statistics::Scalar hintsCanceled;
        statistics::Scalar hintsDroppedStale;
        statistics::Scalar hintsDroppedFull;
        statistics::Scalar hintsDroppedTranslationFull;
        statistics::Scalar hintsDroppedMshrFull;
    } stats;

    void processHints();
  public:
    FTQICachePrefetcher(const FTQICachePrefetcherParams &p);
    ~FTQICachePrefetcher() override = default;

    void calculatePrefetch(const PrefetchInfo &,
                           std::vector<AddrPriority> &) override
    {}
    void notify(const PacketPtr &, const PrefetchInfo &) override {}

    bool submitFTQHint(const FTQPrefetchHint &hint) override;
    void squashFTQHints(ThreadID tid, uint64_t generation) override;
    void pfHitNotify(float, PrefetchSourceType, const PacketPtr &) override {}
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_FTQ_HH__
