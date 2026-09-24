/*
 * Copyright (c) 2026 Beijing Institute of Open Source Chip
 * All rights reserved.
 */

#include "mem/cache/prefetch/fdip.hh"

#include <algorithm>

#include "debug/FDIP.hh"

namespace gem5
{

namespace prefetch
{

FDIPPrefetcher::FDIPStats::FDIPStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(hintsQueued, statistics::units::Count::get(),
               "FDIP hints accepted into the delay queue"),
      ADD_STAT(hintsDispatched, statistics::units::Count::get(),
               "FDIP hints dispatched to the queued prefetcher"),
      ADD_STAT(hintsCanceled, statistics::units::Count::get(),
               "FDIP hints canceled by a squash"),
      ADD_STAT(hintsDroppedStale, statistics::units::Count::get(),
               "FDIP hints dropped due to stale generation"),
      ADD_STAT(hintsDroppedFull, statistics::units::Count::get(),
               "FDIP hints dropped due to a full delay queue"),
      ADD_STAT(hintsDroppedTranslationFull, statistics::units::Count::get(),
               "FDIP hints dropped due to a full translation queue"),
      ADD_STAT(hintsDroppedMshrFull, statistics::units::Count::get(),
               "FDIP hints dropped because no prefetch MSHR was available"),
      ADD_STAT(twoPrefetchCases, statistics::units::Count::get(),
               "FDIP bundles by RTL TwoPrefetchCase")
{
    twoPrefetchCases.init(5);
    twoPrefetchCases.subname(0, "Conflict");
    twoPrefetchCases.subname(1, "SameLine");
    twoPrefetchCases.subname(2, "Overlap1");
    twoPrefetchCases.subname(3, "Overlap2");
    twoPrefetchCases.subname(4, "Interleave");
}

FDIPPrefetcher::FDIPPrefetcher(const FDIPPrefetcherParams &p)
    : Queued(p), pendingHints(), generations(),
      hintEvent([this]{ processHints(); }, name()), stats(this)
{
}

bool
FDIPPrefetcher::isQueued(const FDIPPrefetchHint &hint) const
{
    const auto sameDeferred = [&hint](const DeferredPacket &entry) {
        return entry.specTid == hint.tid && entry.specId == hint.ftqId &&
            entry.specGeneration == hint.generation &&
            entry.pfInfo.getAddr() == hint.vaddr;
    };
    return std::any_of(pendingHints.begin(), pendingHints.end(),
                       [&hint](const PendingHint &entry) {
                           return entry.hint.tid == hint.tid &&
                               entry.hint.ftqId == hint.ftqId &&
                               entry.hint.generation == hint.generation &&
                               entry.hint.vaddr == hint.vaddr;
                       }) ||
        std::any_of(pfq.begin(), pfq.end(),
                    sameDeferred) ||
        std::any_of(pfqMissingTranslation.begin(), pfqMissingTranslation.end(),
                    sameDeferred);
}

void
FDIPPrefetcher::enqueuePendingHint(const FDIPPrefetchHint &hint)
{
    pendingHints.push_back({hint, curTick() + clockPeriod() * 2});
    stats.hintsQueued++;
    if (!hintEvent.scheduled())
        schedule(hintEvent, pendingHints.back().readyAt);
    DPRINTF(FDIP,
            "S0 accept tid=%u ftq=%llu gen=%llu pc=%#lx line=%#lx "
            "ready=%llu\n",
            hint.tid, static_cast<unsigned long long>(hint.ftqId),
            static_cast<unsigned long long>(hint.generation), hint.pc,
            hint.vaddr,
            static_cast<unsigned long long>(pendingHints.back().readyAt));
}

bool
FDIPPrefetcher::submitFDIPBundle(const std::vector<FDIPPrefetchHint> &hints)
{
    if (hints.empty() || hints.size() > 2)
        return false;

    // S0 is an atomic handshake: reserve all lines before inserting any of
    // them into the two-cycle delay queue.  This prevents pfPtr movement when
    // only the second line would fit.
    size_t newHints = 0;
    std::vector<const FDIPPrefetchHint *> toEnqueue;
    for (const auto &hint : hints) {
        const auto it = generations.find(hint.tid);
        if (it != generations.end() && hint.generation < it->second) {
            stats.hintsDroppedStale++;
            return false;
        }
        const bool bundleDuplicate = std::any_of(
            toEnqueue.begin(), toEnqueue.end(), [&hint](const auto *queued) {
                return queued->tid == hint.tid &&
                    queued->ftqId == hint.ftqId &&
                    queued->generation == hint.generation &&
                    queued->vaddr == hint.vaddr;
            });
        if (!isQueued(hint) && !bundleDuplicate) {
            ++newHints;
            toEnqueue.push_back(&hint);
        }
    }
    if (pendingHints.size() + newHints > queueSize ||
        pfq.size() + pfqMissingTranslation.size() + pfqSquashed.size() +
            pendingHints.size() + newHints > queueSize) {
        stats.hintsDroppedFull++;
        return false;
    }
    if (pfqMissingTranslation.size() + pfqSquashed.size() +
            pendingHints.size() + newHints > missingTranslationQueueSize) {
        stats.hintsDroppedTranslationFull++;
        return false;
    }
    if (cache != nullptr && !cache->canPrefetch()) {
        stats.hintsDroppedMshrFull++;
        return false;
    }
    for (const auto *hint : toEnqueue)
        enqueuePendingHint(*hint);
    stats.twoPrefetchCases[static_cast<unsigned>(
        hints.front().twoPrefetchCase)]++;
    return true;
}

void
FDIPPrefetcher::squashFDIPHints(ThreadID tid, uint64_t generation)
{
    generations[tid] = generation;

    for (auto it = pendingHints.begin(); it != pendingHints.end();) {
        if (it->hint.tid == tid && it->hint.generation < generation) {
            DPRINTF(FDIP, "Cancel queued FDIP hint tid=%d ftq=%llu\n",
                    tid, static_cast<unsigned long long>(it->hint.ftqId));
            stats.hintsCanceled++;
            it = pendingHints.erase(it);
        } else {
            ++it;
        }
    }

    stats.hintsCanceled += squashSpeculation(tid, generation);
    if (!pendingHints.empty() && !hintEvent.scheduled()) {
        schedule(hintEvent, std::max(curTick(), pendingHints.front().readyAt));
    }
}

void
FDIPPrefetcher::processHints()
{
    while (!pendingHints.empty() && pendingHints.front().readyAt <= curTick()) {
        if (pfqMissingTranslation.size() + pfqSquashed.size() >=
            missingTranslationQueueSize) {
            break;
        }
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
        schedule(hintEvent,
                 std::max(nextCycle(), pendingHints.front().readyAt));
    }
}

} // namespace prefetch
} // namespace gem5
