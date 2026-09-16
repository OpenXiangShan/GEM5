/*
 * Copyright (c) 2026 Beijing Institute of Open Source Chip
 * All rights reserved.
 */

#include "mem/cache/prefetch/ftq.hh"

#include <algorithm>

#include "debug/FTQPrefetch.hh"

namespace gem5
{

namespace prefetch
{

FTQICachePrefetcher::FTQStats::FTQStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(hintsQueued, statistics::units::Count::get(),
               "FTQ prefetch hints accepted into the delay queue"),
      ADD_STAT(hintsDispatched, statistics::units::Count::get(),
               "FTQ prefetch hints dispatched to the queued prefetcher"),
      ADD_STAT(hintsCanceled, statistics::units::Count::get(),
               "FTQ prefetch hints canceled by a squash"),
      ADD_STAT(hintsDroppedStale, statistics::units::Count::get(),
               "FTQ prefetch hints dropped due to stale generation"),
      ADD_STAT(hintsDroppedFull, statistics::units::Count::get(),
               "FTQ prefetch hints dropped due to a full delay queue"),
      ADD_STAT(hintsDroppedTranslationFull, statistics::units::Count::get(),
               "FTQ prefetch hints dropped due to a full translation queue"),
      ADD_STAT(hintsDroppedMshrFull, statistics::units::Count::get(),
               "FTQ prefetch hints dropped because no prefetch MSHR was available")
{
}

FTQICachePrefetcher::FTQICachePrefetcher(
        const FTQICachePrefetcherParams &p)
    : Queued(p), pendingHints(), generations(),
      hintEvent([this]{ processHints(); }, name()), stats(this)
{
}

bool
FTQICachePrefetcher::submitFTQHint(const FTQPrefetchHint &hint)
{
    const auto it = generations.find(hint.tid);
    if (it != generations.end() && hint.generation < it->second) {
        DPRINTF(FTQPrefetch, "Drop stale FTQ hint tid=%d ftq=%llu\n",
                hint.tid, static_cast<unsigned long long>(hint.ftqId));
        stats.hintsDroppedStale++;
        return false;
    }

    // Submission is idempotent for a target/line/epoch tuple.  This matters
    // when a target spans two lines and the second line is temporarily
    // blocked: Fetch can retry the target without accumulating another copy
    // of the first line in the delay queue.
    const auto sameHint = [&hint](const FTQPrefetchHint &queued) {
        return queued.tid == hint.tid && queued.ftqId == hint.ftqId &&
            queued.generation == hint.generation &&
            queued.vaddr == hint.vaddr;
    };
    for (const auto &pending : pendingHints) {
        if (sameHint(pending.hint)) {
            return true;
        }
    }
    // DeferredPacket carries the FTQ identity separately from PrefetchInfo,
    // so compare those fields directly for requests already dispatched to a
    // translation or ready queue.
    const auto sameDeferred = [&hint](const auto &deferred) {
        return deferred.ftqTid == hint.tid &&
            deferred.ftqId == hint.ftqId &&
            deferred.ftqGeneration == hint.generation &&
            deferred.pfInfo.getAddr() == hint.vaddr;
    };
    for (const auto &deferred : pfq) {
        if (sameDeferred(deferred)) {
            return true;
        }
    }
    for (const auto &deferred : pfqMissingTranslation) {
        if (sameDeferred(deferred)) {
            return true;
        }
    }

    if (pendingHints.size() >= queueSize) {
        DPRINTF(FTQPrefetch, "Drop FTQ hint because hint queue is full tid=%d ftq=%llu\n",
                hint.tid, static_cast<unsigned long long>(hint.ftqId));
        stats.hintsDroppedFull++;
        return false;
    }

    // FTQ hints enter the translation queue first.  Account for delayed
    // hints as reservations so Fetch does not advance pfPtr after the
    // translation queue has already reached its configured bound.
    if (pfqMissingTranslation.size() + pendingHints.size() >=
        missingTranslationQueueSize) {
        DPRINTF(FTQPrefetch,
                "Drop FTQ hint because translation queue is full "
                "tid=%d ftq=%llu\n",
                hint.tid, static_cast<unsigned long long>(hint.ftqId));
        stats.hintsDroppedTranslationFull++;
        return false;
    }

    // Respect the cache's demand MSHR reservation at hint admission time.
    // The hint remains at the current FTQ cursor when no prefetch MSHR is
    // available, allowing a later cycle to retry it after demand pressure
    // subsides.
    if (cache != nullptr && !cache->canPrefetch()) {
        DPRINTF(FTQPrefetch,
                "Drop FTQ hint because cache has no prefetch MSHR "
                "tid=%d ftq=%llu\n",
                hint.tid, static_cast<unsigned long long>(hint.ftqId));
        stats.hintsDroppedMshrFull++;
        return false;
    }

    pendingHints.push_back({hint, curTick() + clockPeriod() * 2});
    stats.hintsQueued++;
    if (!hintEvent.scheduled()) {
        schedule(hintEvent, pendingHints.back().readyAt);
    }
    DPRINTF(FTQPrefetch,
            "Queue FTQ hint tid=%d ftq=%llu gen=%llu va=%#lx ready=%llu\n",
            hint.tid, static_cast<unsigned long long>(hint.ftqId),
            static_cast<unsigned long long>(hint.generation), hint.vaddr,
            static_cast<unsigned long long>(pendingHints.back().readyAt));
    return true;
}

void
FTQICachePrefetcher::squashFTQHints(ThreadID tid, uint64_t generation)
{
    generations[tid] = generation;

    for (auto it = pendingHints.begin(); it != pendingHints.end();) {
        if (it->hint.tid == tid && it->hint.generation < generation) {
            DPRINTF(FTQPrefetch, "Cancel queued FTQ hint tid=%d ftq=%llu\n",
                    tid, static_cast<unsigned long long>(it->hint.ftqId));
            stats.hintsCanceled++;
            it = pendingHints.erase(it);
        } else {
            ++it;
        }
    }

    squashFTQGeneration(tid, generation);
    if (!pendingHints.empty() && !hintEvent.scheduled()) {
        schedule(hintEvent, std::max(curTick(), pendingHints.front().readyAt));
    }
}

void
FTQICachePrefetcher::processHints()
{
    while (!pendingHints.empty() && pendingHints.front().readyAt <= curTick()) {
        PendingHint pending = pendingHints.front();
        pendingHints.pop_front();
        stats.hintsDispatched++;

        const auto it = generations.find(pending.hint.tid);
        if (it != generations.end() && pending.hint.generation < it->second) {
            continue;
        }
        enqueueVirtualPrefetch(pending.hint);
    }

    if (!pendingHints.empty()) {
        schedule(hintEvent, pendingHints.front().readyAt);
    }
}

} // namespace prefetch
} // namespace gem5
