/*
 * Copyright (c) 2026
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "arch/riscv/mpt_unit.hh"

#include <algorithm>
#include <map>
#include <type_traits>
#include <unordered_set>

#include "arch/riscv/page_size.hh"
#include "arch/riscv/regs/misc.hh"
#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "cpu/thread_context.hh"
#include "debug/PageTableWalker.hh"
#include "mem/packet_access.hh"
#include "mem/request.hh"
#include "sim/system.hh"

namespace gem5
{

namespace RiscvISA
{

namespace
{

constexpr uint8_t MptAllowAllPerms =
    MPT_PERM_R | MPT_PERM_W | MPT_PERM_X;
constexpr unsigned MptRootEntries = PageBytes / MPT_MPTE_SIZE;
constexpr Addr MptPpnMask = (1ULL << 44) - 1;

constexpr uint64_t
makeAllowAllLeafMpte()
{
    uint64_t mpte = 0x3;
    for (unsigned i = 0; i < MPT_NUM_PERMS; ++i) {
        mpte |= static_cast<uint64_t>(MptAllowAllPerms)
            << (10 + i * MPT_PERM_BITS_PER_ENTRY);
    }
    return mpte;
}

constexpr uint64_t MptAllowAllLeafMpte = makeAllowAllLeafMpte();

uint64_t
makeInternalMpte(Addr next_level_paddr)
{
    return 0x1 | ((next_level_paddr >> PageShift) << 10);
}

} // anonymous namespace

size_t
MptUnit::MshrKeyHash::operator()(const MshrKey &key) const
{
    size_t hash = std::hash<uint64_t>{}(key.epoch);
    auto combine = [&hash](uint64_t value) {
        hash ^= std::hash<uint64_t>{}(value) + 0x9e3779b97f4a7c15ULL +
                (hash << 6) + (hash >> 2);
    };
    combine(key.rootPpn);
    combine(static_cast<uint64_t>(key.level));
    combine(key.mptePaddr);
    return hash;
}

MptUnit::MptStats::MptStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(requests, statistics::units::Count::get(),
               "MPT requests by source"),
      ADD_STAT(leafHits, statistics::units::Count::get(),
               "MPT cache leaf hits by level"),
      ADD_STAT(internalHits, statistics::units::Count::get(),
               "MPT cache internal hits by level"),
      ADD_STAT(mpteMisses, statistics::units::Count::get(),
               "MPT MPTE misses accepted by level"),
      ADD_STAT(totalCacheMisses, statistics::units::Count::get(),
               "MPT lookups with no cached leaf or internal entry"),
      ADD_STAT(cacheBypasses, statistics::units::Count::get(),
               "MPT lookups that bypassed the dedicated MPT cache"),
      ADD_STAT(lookupQueueOccupancy, statistics::units::Count::get(),
               "Accumulated MPT lookup queue occupancy"),
      ADD_STAT(lookupQueueSamples, statistics::units::Count::get(),
               "MPT lookup queue occupancy samples"),
      ADD_STAT(lookupQueueFullCycles, statistics::units::Cycle::get(),
               "Cycles with an input blocked by a full lookup queue"),
      ADD_STAT(pipelineCompletions, statistics::units::Count::get(),
               "MPT cache pipeline completions"),
      ADD_STAT(pipelineLatency, statistics::units::Cycle::get(),
               "Accumulated MPT cache pipeline latency"),
      ADD_STAT(completedLookups, statistics::units::Count::get(),
               "Completed MPT permission lookups"),
      ADD_STAT(totalLookupLatency, statistics::units::Cycle::get(),
               "Accumulated end-to-end MPT lookup latency"),
      ADD_STAT(mshrAllocations, statistics::units::Count::get(),
               "MPT MSHR allocations"),
      ADD_STAT(mshrMerges, statistics::units::Count::get(),
               "Targets merged into an existing MPT MSHR"),
      ADD_STAT(mshrFullEvents, statistics::units::Count::get(),
               "MPT misses delayed because all MSHRs were allocated"),
      ADD_STAT(mshrTargetFullEvents, statistics::units::Count::get(),
               "MPT misses delayed because a matching MSHR target list was full"),
      ADD_STAT(mshrOccupancy, statistics::units::Count::get(),
               "Accumulated MPT MSHR occupancy"),
      ADD_STAT(mshrOccupancySamples, statistics::units::Count::get(),
               "MPT MSHR occupancy samples"),
      ADD_STAT(memoryRequests, statistics::units::Count::get(),
               "MPT memory requests issued"),
      ADD_STAT(memoryRetries, statistics::units::Count::get(),
               "MPT memory request port retries"),
      ADD_STAT(memoryLatency, statistics::units::Cycle::get(),
               "Accumulated MPT memory response latency"),
      ADD_STAT(maxMemoryInflight, statistics::units::Count::get(),
               "Maximum simultaneous MPT memory requests"),
      ADD_STAT(prefetchIssued, statistics::units::Count::get(),
               "MPT cache prefetches allocated to a prefetch MSHR"),
      ADD_STAT(prefetchFilled, statistics::units::Count::get(),
               "Valid MPT cache prefetch responses inserted into the cache"),
      ADD_STAT(prefetchUseful, statistics::units::Count::get(),
               "MPT cache prefetches consumed by a demand lookup"),
      ADD_STAT(prefetchUnused, statistics::units::Count::get(),
               "Prefetched MPT cache entries evicted or invalidated before "
               "demand use"),
      ADD_STAT(prefetchDropped, statistics::units::Count::get(),
               "MPT cache prefetch candidates dropped before useful fill"),
      ADD_STAT(prefetchMerges, statistics::units::Count::get(),
               "Demand targets merged into an outstanding prefetch"),
      ADD_STAT(prefetchMshrFull, statistics::units::Count::get(),
               "MPT cache prefetch allocation stalls from full MSHRs"),
      ADD_STAT(prefetchMemoryRequests, statistics::units::Count::get(),
               "MPT cache prefetch requests issued to memory"),
      ADD_STAT(walkDepth, statistics::units::Count::get(),
               "Completed MPT lookups by number of memory levels read"),
      ADD_STAT(staleEpochResponses, statistics::units::Count::get(),
               "MPT responses discarded after an MMPT/fence epoch change"),
      ADD_STAT(fenceFlushes, statistics::units::Count::get(),
               "MPT cache/epoch flushes"),
      ADD_STAT(squashes, statistics::units::Count::get(),
               "MPT clients cancelled before completion"),
      ADD_STAT(avgLookupQueueOccupancy, statistics::units::Ratio::get(),
               "Average MPT lookup queue occupancy",
               lookupQueueOccupancy / lookupQueueSamples),
      ADD_STAT(avgMshrOccupancy, statistics::units::Ratio::get(),
               "Average MPT MSHR occupancy",
               mshrOccupancy / mshrOccupancySamples),
      ADD_STAT(avgLookupLatency,
               statistics::units::Rate<statistics::units::Cycle,
                                       statistics::units::Count>::get(),
               "Average end-to-end MPT lookup latency",
               totalLookupLatency / completedLookups),
      ADD_STAT(avgMemoryLatency,
               statistics::units::Rate<statistics::units::Cycle,
                                       statistics::units::Count>::get(),
               "Average MPT memory request latency",
               memoryLatency / memoryRequests)
{
    requests.init(NumSources);
    requests.subname(sourceIndex(MptRequestSource::Instruction), "instruction");
    requests.subname(sourceIndex(MptRequestSource::Data), "data");
    requests.subname(sourceIndex(MptRequestSource::Ptw), "ptw");

    leafHits.init(NumLevels);
    internalHits.init(NumLevels);
    mpteMisses.init(NumLevels);
    for (unsigned level = 0; level < NumLevels; ++level) {
        const std::string level_name = "L" + std::to_string(level);
        leafHits.subname(level, level_name);
        internalHits.subname(level, level_name);
        mpteMisses.subname(level, level_name);
    }

    walkDepth.init(NumLevels + 1);
    for (unsigned depth = 0; depth <= NumLevels; ++depth) {
        walkDepth.subname(depth, std::to_string(depth) + "_reads");
    }
}

MptUnit::MptUnit(const Params &p)
    : ClockedObject(p), stats(this), port(name() + ".port", *this),
      system(p.system), requestorId(system->getRequestorId(this)),
      enableMptCache(p.enable_mpt_cache),
      enableMptCachePrefetch(p.enable_cache_prefetch),
      prefetchDegree(p.prefetch_degree), prefetchLevel(p.prefetch_level),
      numPrefetchMshrs(p.prefetch_mshrs),
      prefetchQueueCapacity(p.prefetch_queue_size),
      prefetchIssueWidth(p.prefetch_issue_width),
      hitLatency(p.hit_latency), lookupWidth(p.lookup_width),
      acceptWidth{{p.instruction_accept_width, p.data_accept_width,
                   p.ptw_accept_width}},
      queueCapacity{{p.instruction_queue_size, p.data_queue_size,
                     p.ptw_queue_size}},
      numMshrs(p.num_mshrs), targetsPerMshr(p.targets_per_mshr),
      memoryIssueWidth(p.memory_issue_width),
      maxMemoryInflight(p.max_memory_inflight), mshrs(numMshrs),
      prefetchMshrs(numPrefetchMshrs),
      serviceEvent([this] { process(); }, name() + ".service")
{
    panic_if(system == nullptr, "MPT unit requires a System object\n");
    panic_if(lookupWidth == 0, "MPT lookup_width must be positive\n");
    panic_if(numMshrs == 0, "MPT num_mshrs must be positive\n");
    panic_if(targetsPerMshr == 0,
             "MPT targets_per_mshr must be positive\n");
    panic_if(memoryIssueWidth == 0,
             "MPT memory_issue_width must be positive\n");
    const unsigned total_mshrs = numMshrs +
        (enableMptCachePrefetch ? numPrefetchMshrs : 0);
    panic_if(maxMemoryInflight == 0 || maxMemoryInflight > total_mshrs,
             "MPT max_memory_inflight must be in [1, total MSHRs]\n");
    panic_if(enableMptCachePrefetch && !enableMptCache,
             "MPT cache prefetching requires the MPT cache\n");
    panic_if(enableMptCachePrefetch && prefetchDegree == 0,
             "MPT prefetch_degree must be positive when enabled\n");
    panic_if(enableMptCachePrefetch && prefetchLevel >= NumLevels,
             "MPT prefetch_level must be in [0, %u]\n", NumLevels - 1);
    panic_if(enableMptCachePrefetch && numPrefetchMshrs == 0,
             "MPT prefetch_mshrs must be positive when enabled\n");
    panic_if(enableMptCachePrefetch && prefetchQueueCapacity == 0,
             "MPT prefetch_queue_size must be positive when enabled\n");
    panic_if(enableMptCachePrefetch && prefetchIssueWidth == 0,
             "MPT prefetch_issue_width must be positive when enabled\n");

    for (unsigned source = 0; source < NumSources; ++source) {
        panic_if(acceptWidth[source] == 0,
                 "MPT source %u acceptance width must be positive\n", source);
        panic_if(queueCapacity[source] == 0,
                 "MPT source %u queue size must be positive\n", source);
    }

    const std::array<size_t, NumLevels + 1> cache_capacities{{
        p.cache_l0_size,
        p.cache_l1_size,
        p.cache_l2_size,
        p.cache_l3_size,
        p.cache_sp_size,
    }};
    const std::array<replacement_policy::Base *, NumLevels + 1>
        replacement_policies{{
            p.cache_l0_replacement_policy,
            p.cache_l1_replacement_policy,
            p.cache_l2_replacement_policy,
            p.cache_l3_replacement_policy,
            p.cache_sp_replacement_policy,
        }};
    if (enableMptCache) {
        for (unsigned partition = 0; partition < cache.size(); ++partition) {
            configureCachePartition(cache[partition],
                                    cache_capacities[partition],
                                    replacement_policies[partition]);
        }
    }
}

MptUnit::~MptUnit()
{
    auto discard_packets = [this](auto &pool) {
        for (auto &mshr : pool) {
            if (mshr.packet != nullptr) {
                discardPacket(mshr.packet);
                mshr.packet = nullptr;
            }
        }
    };
    discard_packets(mshrs);
    discard_packets(prefetchMshrs);
}

Port &
MptUnit::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "port") {
        return port;
    }
    return ClockedObject::getPort(if_name, idx);
}

bool
MptUnit::MptPort::recvTimingResp(PacketPtr pkt)
{
    return owner.recvTimingResp(pkt);
}

void
MptUnit::MptPort::recvReqRetry()
{
    owner.recvReqRetry();
}

void
MptUnit::scheduleService(Cycles delay)
{
    if (!serviceEvent.scheduled()) {
        schedule(serviceEvent, clockEdge(delay));
    }
}

bool
MptUnit::hasServiceWork() const
{
    if (!pipeline.empty() || !pendingWalks.empty() ||
        !bypassCompletions.empty() || !prefetchQueue.empty() ||
        !readyMshrs.empty() || !readyPrefetchMshrs.empty()) {
        return true;
    }
    for (unsigned source = 0; source < NumSources; ++source) {
        if (!arrivals[source].empty() || !lookupQueues[source].empty()) {
            return true;
        }
    }
    return false;
}

void
MptUnit::sampleOccupancy()
{
    size_t queued = 0;
    for (const auto &queue : lookupQueues) {
        queued += queue.size();
    }
    stats.lookupQueueOccupancy += queued;
    stats.lookupQueueSamples++;

    unsigned allocated = 0;
    for (const auto &mshr : mshrs) {
        allocated += mshr.allocated;
    }
    stats.mshrOccupancy += allocated;
    stats.mshrOccupancySamples++;
}

void
MptUnit::process()
{
    sampleOccupancy();
    completeBypasses();
    completePipeline();
    retryPendingWalks();
    acceptArrivals();
    issueLookups();
    /* A zero-cycle cache hit is allowed to complete in this service event. */
    if (hitLatency == Cycles(0)) {
        completePipeline();
    }
    issuePrefetches();
    issueMemoryRequests();

    if (hasServiceWork()) {
        scheduleService(Cycles(1));
    }
}

void
MptUnit::submit(MptClient *client, Addr paddr, BaseMMU::Mode mode,
                MptRequestSource source)
{
    panic_if(client == nullptr, "Cannot submit an MPT request without a client\n");
    panic_if(sourceIndex(source) >= NumSources,
             "Invalid MPT request source %u\n", sourceIndex(source));

    Target target;
    target.id = nextTargetId++;
    target.client = client;
    target.paddr = paddr;
    target.mode = mode;
    target.source = source;
    target.epoch = epoch;
    target.rootPpn = mmpt.ppn;
    target.level = 3;
    target.tableBase = mmpt.ppn << PageShift;
    target.enqueueTick = curTick();

    stats.requests[sourceIndex(source)]++;
    if (!enabled()) {
        bypassCompletions.push_back(target);
    } else {
        arrivals[sourceIndex(source)].push_back(target);
    }
    scheduleService();
}

void
MptUnit::acceptArrivals()
{
    bool queue_full = false;
    for (unsigned source = 0; source < NumSources; ++source) {
        unsigned accepted = 0;
        auto &arrival = arrivals[source];
        auto &queue = lookupQueues[source];
        while (!arrival.empty() && accepted < acceptWidth[source] &&
               queue.size() < queueCapacity[source]) {
            queue.push_back(arrival.front());
            arrival.pop_front();
            ++accepted;
        }
        queue_full |= !arrival.empty() &&
                      queue.size() >= queueCapacity[source];
    }
    if (queue_full) {
        stats.lookupQueueFullCycles++;
    }
}

void
MptUnit::issueLookups()
{
    unsigned issued = 0;
    unsigned empty_sources = 0;
    while (issued < lookupWidth && empty_sources < NumSources) {
        const unsigned source = nextSource;
        nextSource = (nextSource + 1) % NumSources;
        auto &queue = lookupQueues[source];
        if (queue.empty()) {
            ++empty_sources;
            continue;
        }

        empty_sources = 0;
        Target target = queue.front();
        queue.pop_front();
        if (target.epoch != epoch || target.rootPpn != mmpt.ppn) {
            restartTarget(target);
            continue;
        }

        if (!enableMptCache) {
            ++stats.cacheBypasses;
            target.level = 3;
            target.tableBase = target.rootPpn << PageShift;
            target.depth = 0;
            if (!startTargetRead(target)) {
                pendingWalks.push_back(target);
            }
            ++issued;
            continue;
        }

        PipelineEntry entry;
        entry.target = target;
        entry.probe = probeCache(target.paddr);
        entry.readyTick = curTick() + cyclesToTicks(hitLatency);
        pipeline.push_back(entry);
        ++issued;
    }
}

void
MptUnit::completePipeline()
{
    while (!pipeline.empty() && pipeline.front().readyTick <= curTick()) {
        PipelineEntry entry = pipeline.front();
        pipeline.pop_front();
        stats.pipelineCompletions++;
        stats.pipelineLatency += hitLatency;

        Target target = entry.target;
        if (target.epoch != epoch || target.rootPpn != mmpt.ppn) {
            restartTarget(target);
            continue;
        }
        if (!enabled()) {
            bypassCompletions.push_back(target);
            continue;
        }

        if (entry.probe.kind == ProbeKind::LeafHit) {
            const MPTE52 &mpte = entry.probe.entry.mpte;
            const uint8_t pi =
                (target.paddr >> getPageShiftForLevel(entry.probe.level)) &
                0xf;
            const uint8_t permission = mptEffectivePerm(mpte.perms(pi));
            MptResult result;
            result.valid = true;
            result.allowed = mptPermAllowsAccess(permission, target.mode);
            result.permission = permission;
            result.level = entry.probe.level;
            completeTarget(target, result);
            continue;
        }

        if (entry.probe.kind == ProbeKind::InternalHit) {
            target.level = entry.probe.level - 1;
            target.tableBase = entry.probe.entry.mpte.nextLevelPAddr();
        } else {
            target.level = 3;
            target.tableBase = target.rootPpn << PageShift;
        }

        if (!startTargetRead(target)) {
            pendingWalks.push_back(target);
        }
    }
}

void
MptUnit::retryPendingWalks()
{
    const size_t pending = pendingWalks.size();
    for (size_t i = 0; i < pending; ++i) {
        Target target = pendingWalks.front();
        pendingWalks.pop_front();
        if (!startTargetRead(target)) {
            pendingWalks.push_back(target);
        }
    }
}

void
MptUnit::completeBypasses()
{
    while (!bypassCompletions.empty()) {
        Target target = bypassCompletions.front();
        bypassCompletions.pop_front();
        MptResult result;
        result.allowed = true;
        result.valid = true;
        result.permission = MptAllowAllPerms;
        result.level = 3;
        completeTarget(target, result);
    }
}

Addr
MptUnit::cacheKey(Addr paddr, int level, bool superpage)
{
    Addr aligned = paddr & ~(getRegionSizeForLevel(level) - 1);
    if (superpage) {
        assert((aligned & 0x7) == 0);
        aligned |= static_cast<Addr>(level & 0x7);
    }
    return aligned;
}

void
MptUnit::configureCachePartition(CachePartition &partition, size_t capacity,
                                 replacement_policy::Base *policy)
{
    panic_if(capacity == 0 || !isPowerOf2(capacity),
             "MPT cache partition capacity must be a non-zero power of two "
             "(got %zu)\n", capacity);
    panic_if(policy == nullptr,
             "MPT cache partition requires a replacement policy\n");

    partition.capacity = capacity;
    partition.replacementPolicy = policy;
    partition.ways.resize(capacity);
    partition.wayIndex.reserve(capacity);
    for (auto &way : partition.ways) {
        way.replacementData = policy->instantiateEntry();
    }
}

MptUnit::CacheWay *
MptUnit::findCacheWay(CachePartition &partition, Addr key)
{
    const auto found = partition.wayIndex.find(key);
    if (found == partition.wayIndex.end()) {
        return nullptr;
    }

    panic_if(found->second >= partition.ways.size(),
             "MPT cache key %#lx has invalid way %zu\n", key, found->second);
    CacheWay &way = partition.ways[found->second];
    panic_if(!way.entry.valid || way.key != key,
             "MPT cache key-to-way index is inconsistent for key %#lx\n",
             key);
    return &way;
}

const MptUnit::CacheWay *
MptUnit::findCacheWay(const CachePartition &partition, Addr key) const
{
    const auto found = partition.wayIndex.find(key);
    if (found == partition.wayIndex.end()) {
        return nullptr;
    }

    panic_if(found->second >= partition.ways.size(),
             "MPT cache key %#lx has invalid way %zu\n", key, found->second);
    const CacheWay &way = partition.ways[found->second];
    panic_if(!way.entry.valid || way.key != key,
             "MPT cache key-to-way index is inconsistent for key %#lx\n",
             key);
    return &way;
}

MptUnit::CacheWay *
MptUnit::findCacheVictim(CachePartition &partition)
{
    for (auto &way : partition.ways) {
        if (!way.entry.valid) {
            return &way;
        }
    }

    ReplacementCandidates candidates;
    candidates.reserve(partition.ways.size());
    for (auto &way : partition.ways) {
        candidates.push_back(&way);
    }
    return static_cast<CacheWay *>(
        partition.replacementPolicy->getVictim(candidates));
}

MptUnit::ProbeResult
MptUnit::probeCache(Addr paddr)
{
    bool leaf_hit = false;
    int leaf_level = -1;
    MPTCacheEntry leaf_entry;
    bool internal_hit = false;
    int internal_level = NumLevels;
    MPTCacheEntry internal_entry;

    auto consider = [&](unsigned partition, Addr key, int level) {
        auto &part = cache[partition];
        CacheWay *way = findCacheWay(part, key);
        if (way == nullptr) {
            return;
        }
        if (part.capacity > 1) {
            part.replacementPolicy->touch(way->replacementData);
        }
        if (way->entry.mpte.isLeaf()) {
            if (!leaf_hit || level > leaf_level) {
                leaf_hit = true;
                leaf_level = level;
                leaf_entry = way->entry;
            }
        } else if (!internal_hit || level < internal_level) {
            internal_hit = true;
            internal_level = level;
            internal_entry = way->entry;
        }
    };

    for (int level = 0; level < static_cast<int>(NumLevels); ++level) {
        consider(level, cacheKey(paddr, level, false), level);
        consider(SuperpageCache, cacheKey(paddr, level, true), level);
    }

    ProbeResult result;
    if (leaf_hit) {
        result.kind = ProbeKind::LeafHit;
        result.entry = leaf_entry;
        result.level = leaf_level;
        stats.leafHits[leaf_level]++;
    } else if (internal_hit && internal_level > 0) {
        result.kind = ProbeKind::InternalHit;
        result.entry = internal_entry;
        result.level = internal_level;
        stats.internalHits[internal_level]++;
    } else {
        result.kind = ProbeKind::Miss;
        stats.totalCacheMisses++;
    }
    if (result.kind != ProbeKind::Miss && result.entry.prefetched) {
        markPrefetchUseful(paddr, result.entry);
        result.entry.prefetched = false;
    }
    return result;
}

bool
MptUnit::cacheCoversPrefetch(Addr paddr, int requested_level) const
{
    for (int level = 0; level < static_cast<int>(NumLevels); ++level) {
        const auto &part = cache[level];
        const CacheWay *entry = findCacheWay(
            part, cacheKey(paddr, level, false));
        if (entry != nullptr &&
            (entry->entry.mpte.isLeaf() || level <= requested_level)) {
            return true;
        }

        const auto &superpage = cache[SuperpageCache];
        const CacheWay *leaf = findCacheWay(
            superpage, cacheKey(paddr, level, true));
        if (leaf != nullptr && leaf->entry.mpte.isLeaf()) {
            return true;
        }
    }
    return false;
}

void
MptUnit::markPrefetchUseful(Addr paddr, const MPTCacheEntry &entry)
{
    const bool superpage = entry.mpte.isLeaf() && entry.level > 0;
    CachePartition &part = cache[superpage ? SuperpageCache : entry.level];
    const Addr key = cacheKey(paddr, entry.level, superpage);
    CacheWay *stored = findCacheWay(part, key);
    if (stored != nullptr && stored->entry.prefetched) {
        stored->entry.prefetched = false;
        stats.prefetchUseful++;
    }
}

bool
MptUnit::insertCache(int level, Addr paddr, const MPTE52 &mpte,
                     bool prefetched)
{
    if (!enableMptCache) {
        return false;
    }

    const bool superpage = mpte.isLeaf() && level > 0;
    const unsigned partition = superpage ? SuperpageCache : level;
    CachePartition &part = cache[partition];
    const Addr key = cacheKey(paddr, level, superpage);

    MPTCacheEntry entry;
    entry.tag = paddr & ~(getRegionSizeForLevel(level) - 1);
    entry.mpte = mpte;
    entry.valid = true;
    entry.prefetched = prefetched;
    entry.level = level;
    entry.log2RegionSize = log2floor(getRegionSizeForLevel(level));

    CacheWay *existing = findCacheWay(part, key);
    if (prefetched && existing != nullptr) {
        return false;
    }
    if (!prefetched && existing != nullptr &&
        existing->entry.prefetched) {
        stats.prefetchUnused++;
    }
    if (existing != nullptr) {
        existing->entry = entry;
        if (part.capacity > 1) {
            part.replacementPolicy->touch(existing->replacementData);
        }
        return true;
    }

    CacheWay *victim = findCacheVictim(part);
    panic_if(victim == nullptr, "MPT cache replacement found no victim\n");
    if (victim->entry.valid) {
        if (victim->entry.prefetched) {
            stats.prefetchUnused++;
        }
        const size_t erased = part.wayIndex.erase(victim->key);
        panic_if(erased != 1,
                 "MPT cache victim key %#lx is missing from way index\n",
                 victim->key);
    }

    victim->key = key;
    victim->entry = entry;
    const size_t way = victim - part.ways.data();
    const bool inserted = part.wayIndex.emplace(key, way).second;
    panic_if(!inserted, "MPT cache key %#lx was inserted twice\n", key);
    if (part.capacity > 1) {
        part.replacementPolicy->reset(victim->replacementData);
    }
    return true;
}

void
MptUnit::clearCache()
{
    for (auto &partition : cache) {
        for (auto &way : partition.ways) {
            if (way.entry.valid && way.entry.prefetched) {
                stats.prefetchUnused++;
            }
            way.key = 0;
            way.entry = MPTCacheEntry();
            if (partition.capacity > 1) {
                partition.replacementPolicy->invalidate(
                    way.replacementData);
            }
        }
        partition.wayIndex.clear();
    }
}

Addr
MptUnit::mpteAddress(const Target &target) const
{
    const size_t shift = getPageShiftForLevel(target.level) + 4;
    const size_t index = (target.paddr >> shift) & 0x1ff;
    return target.tableBase + index * MPT_MPTE_SIZE;
}

MptUnit::MshrKey
MptUnit::mshrKey(const Target &target) const
{
    MshrKey key;
    key.epoch = target.epoch;
    key.rootPpn = target.rootPpn;
    key.level = target.level;
    key.mptePaddr = mpteAddress(target);
    return key;
}

MptUnit::Mshr &
MptUnit::getMshr(const MshrRef &ref)
{
    auto &pool = ref.pool == MshrPool::Prefetch ? prefetchMshrs : mshrs;
    panic_if(ref.slot >= pool.size(), "Invalid MPT MSHR slot %u\n", ref.slot);
    return pool[ref.slot];
}

const MptUnit::Mshr &
MptUnit::getMshr(const MshrRef &ref) const
{
    const auto &pool =
        ref.pool == MshrPool::Prefetch ? prefetchMshrs : mshrs;
    panic_if(ref.slot >= pool.size(), "Invalid MPT MSHR slot %u\n", ref.slot);
    return pool[ref.slot];
}

bool
MptUnit::validMshr(const MshrRef &ref) const
{
    const auto &pool =
        ref.pool == MshrPool::Prefetch ? prefetchMshrs : mshrs;
    return ref.slot < pool.size() && pool[ref.slot].allocated;
}

void
MptUnit::removeReadyMshr(const MshrRef &ref)
{
    readyMshrs.erase(
        std::remove(readyMshrs.begin(), readyMshrs.end(), ref),
        readyMshrs.end());
    if (ref.pool == MshrPool::Prefetch) {
        readyPrefetchMshrs.erase(
            std::remove(readyPrefetchMshrs.begin(),
                        readyPrefetchMshrs.end(), ref.slot),
            readyPrefetchMshrs.end());
    }
}

void
MptUnit::promotePrefetchMshr(unsigned slot)
{
    const MshrRef ref{MshrPool::Prefetch, slot};
    if (!validMshr(ref)) {
        return;
    }
    Mshr &mshr = getMshr(ref);
    if (mshr.inFlight || mshr.packet == nullptr) {
        return;
    }

    readyPrefetchMshrs.erase(
        std::remove(readyPrefetchMshrs.begin(), readyPrefetchMshrs.end(), slot),
        readyPrefetchMshrs.end());
    if (blockedMshr.has_value() && *blockedMshr == ref) {
        return;
    }
    if (std::find(readyMshrs.begin(), readyMshrs.end(), ref) ==
        readyMshrs.end()) {
        readyMshrs.push_back(ref);
    }
}

bool
MptUnit::startTargetRead(Target target)
{
    if (target.epoch != epoch || target.rootPpn != mmpt.ppn) {
        restartTarget(target);
        return true;
    }
    if (!enabled()) {
        bypassCompletions.push_back(target);
        return true;
    }
    panic_if(target.level < 0 || target.level >= static_cast<int>(NumLevels),
             "Invalid MPT walk level %d\n", target.level);

    const MshrKey key = mshrKey(target);

    auto existing = mshrIndex.find(key);
    if (existing != mshrIndex.end()) {
        Mshr &mshr = mshrs[existing->second];
        if (mshr.targets.size() >= targetsPerMshr) {
            stats.mshrTargetFullEvents++;
            return false;
        }
        mshr.targets.push_back(target);
        stats.mshrMerges++;
        stats.mpteMisses[target.level]++;
        return true;
    }

    auto prefetched = prefetchMshrIndex.find(key);
    if (prefetched != prefetchMshrIndex.end()) {
        Mshr &mshr = prefetchMshrs[prefetched->second];
        if (mshr.targets.size() >= targetsPerMshr) {
            stats.mshrTargetFullEvents++;
            return false;
        }
        mshr.targets.push_back(target);
        stats.mshrMerges++;
        stats.prefetchMerges++;
        stats.mpteMisses[target.level]++;
        if (!mshr.prefetchUseful) {
            mshr.prefetchUseful = true;
            stats.prefetchUseful++;
        }
        promotePrefetchMshr(prefetched->second);
        return true;
    }

    unsigned slot = numMshrs;
    for (unsigned i = 0; i < numMshrs; ++i) {
        if (!mshrs[i].allocated) {
            slot = i;
            break;
        }
    }
    if (slot == numMshrs) {
        stats.mshrFullEvents++;
        return false;
    }

    Mshr &mshr = mshrs[slot];
    mshr.allocated = true;
    mshr.inFlight = false;
    ++mshr.generation;
    mshr.key = key;
    mshr.targets.clear();
    mshr.targets.reserve(targetsPerMshr);
    mshr.targets.push_back(target);
    mshr.prefetchTarget.reset();
    mshr.prefetchUseful = false;
    const MshrRef ref{MshrPool::Demand, slot};
    mshr.packet = createReadPacket(ref, mshr.generation, key.mptePaddr);
    mshr.issueTick = 0;
    mshrIndex.emplace(key, slot);
    readyMshrs.push_back(ref);
    stats.mshrAllocations++;
    stats.mpteMisses[target.level]++;
    return true;
}

bool
MptUnit::prefetchQueued(const MshrKey &key) const
{
    return std::any_of(
        prefetchQueue.begin(), prefetchQueue.end(),
        [this, &key](const Target &target) {
            return mshrKey(target) == key;
        });
}

void
MptUnit::queuePrefetches(const Target &target)
{
    if (!enableMptCachePrefetch || !enabled() ||
        target.source == MptRequestSource::Ptw ||
        target.level != static_cast<int>(prefetchLevel) ||
        target.epoch != epoch || target.rootPpn != mmpt.ppn) {
        return;
    }

    const Addr region_size = getRegionSizeForLevel(target.level);
    const Addr region_base = target.paddr & ~(region_size - 1);
    const unsigned index =
        (target.paddr >> (getPageShiftForLevel(target.level) + 4)) & 0x1ff;

    for (unsigned distance = 1; distance <= prefetchDegree; ++distance) {
        if (distance >= MptRootEntries ||
            index > MptRootEntries - 1 - distance) {
            stats.prefetchDropped += prefetchDegree - distance + 1;
            break;
        }

        Target candidate = target;
        candidate.id = 0;
        candidate.client = nullptr;
        candidate.paddr = region_base + distance * region_size;
        candidate.depth = 0;
        candidate.enqueueTick = curTick();
        const MshrKey key = mshrKey(candidate);

        if (cacheCoversPrefetch(candidate.paddr, candidate.level) ||
            mshrIndex.find(key) != mshrIndex.end() ||
            prefetchMshrIndex.find(key) != prefetchMshrIndex.end() ||
            prefetchQueued(key)) {
            stats.prefetchDropped++;
            continue;
        }
        if (prefetchQueue.size() >= prefetchQueueCapacity) {
            stats.prefetchDropped++;
            continue;
        }
        prefetchQueue.push_back(candidate);
    }
}

MptUnit::PrefetchStartResult
MptUnit::startPrefetch(Target target)
{
    if (!enableMptCachePrefetch || target.epoch != epoch ||
        target.rootPpn != mmpt.ppn || !enabled()) {
        stats.prefetchDropped++;
        return PrefetchStartResult::Dropped;
    }

    const MshrKey key = mshrKey(target);
    if (cacheCoversPrefetch(target.paddr, target.level) ||
        mshrIndex.find(key) != mshrIndex.end() ||
        prefetchMshrIndex.find(key) != prefetchMshrIndex.end()) {
        stats.prefetchDropped++;
        return PrefetchStartResult::Dropped;
    }

    unsigned slot = numPrefetchMshrs;
    for (unsigned i = 0; i < numPrefetchMshrs; ++i) {
        if (!prefetchMshrs[i].allocated) {
            slot = i;
            break;
        }
    }
    if (slot == numPrefetchMshrs) {
        stats.prefetchMshrFull++;
        return PrefetchStartResult::Retry;
    }

    Mshr &mshr = prefetchMshrs[slot];
    mshr.allocated = true;
    mshr.inFlight = false;
    ++mshr.generation;
    mshr.key = key;
    mshr.targets.clear();
    mshr.targets.reserve(targetsPerMshr);
    mshr.prefetchTarget = target;
    mshr.prefetchUseful = false;
    const MshrRef ref{MshrPool::Prefetch, slot};
    mshr.packet = createReadPacket(ref, mshr.generation, key.mptePaddr);
    mshr.issueTick = 0;
    prefetchMshrIndex.emplace(key, slot);
    readyPrefetchMshrs.push_back(slot);
    stats.prefetchIssued++;
    return PrefetchStartResult::Started;
}

void
MptUnit::issuePrefetches()
{
    if (!enableMptCachePrefetch) {
        return;
    }

    unsigned issued = 0;
    while (issued < prefetchIssueWidth && !prefetchQueue.empty()) {
        const PrefetchStartResult result = startPrefetch(prefetchQueue.front());
        if (result == PrefetchStartResult::Retry) {
            break;
        }
        prefetchQueue.pop_front();
        if (result == PrefetchStartResult::Started) {
            ++issued;
        }
    }
}

PacketPtr
MptUnit::createReadPacket(const MshrRef &ref, uint64_t generation, Addr paddr)
{
    RequestPtr request = std::make_shared<Request>(
        paddr, MPT_MPTE_SIZE, Request::PHYSICAL, requestorId);
    request->setMptWalk(true);
    PacketPtr packet = new Packet(request, MemCmd::ReadReq);
    packet->allocate();
    packet->pushSenderState(
        new MptSenderState(ref.pool, ref.slot, generation));
    return packet;
}

void
MptUnit::issueMemoryRequests()
{
    if (blockedMshr.has_value()) {
        return;
    }

    unsigned issued = 0;
    while (issued < memoryIssueWidth &&
           memoryInflight < maxMemoryInflight && !readyMshrs.empty()) {
        const MshrRef ref = readyMshrs.front();
        if (!validMshr(ref)) {
            readyMshrs.pop_front();
            continue;
        }
        Mshr &mshr = getMshr(ref);
        if (!mshr.allocated || mshr.inFlight || mshr.packet == nullptr) {
            readyMshrs.pop_front();
            continue;
        }
        if (!sendMshr(ref)) {
            blockedMshr = ref;
            return;
        }
        readyMshrs.pop_front();
        ++issued;
    }

    unsigned prefetch_issued = 0;
    while (issued < memoryIssueWidth &&
           prefetch_issued < prefetchIssueWidth &&
           memoryInflight < maxMemoryInflight &&
           !readyPrefetchMshrs.empty()) {
        const unsigned slot = readyPrefetchMshrs.front();
        const MshrRef ref{MshrPool::Prefetch, slot};
        if (!validMshr(ref)) {
            readyPrefetchMshrs.pop_front();
            continue;
        }
        Mshr &mshr = getMshr(ref);
        if (mshr.inFlight || mshr.packet == nullptr) {
            readyPrefetchMshrs.pop_front();
            continue;
        }
        if (!sendMshr(ref)) {
            blockedMshr = ref;
            return;
        }
        readyPrefetchMshrs.pop_front();
        ++issued;
        ++prefetch_issued;
    }
}

bool
MptUnit::sendMshr(const MshrRef &ref)
{
    Mshr &mshr = getMshr(ref);
    panic_if(!mshr.allocated || mshr.inFlight || mshr.packet == nullptr,
             "Invalid MPT MSHR send for slot %u\n", ref.slot);
    if (!port.sendTimingReq(mshr.packet)) {
        return false;
    }

    mshr.packet = nullptr;
    mshr.inFlight = true;
    mshr.issueTick = curTick();
    ++memoryInflight;
    stats.memoryRequests++;
    if (ref.pool == MshrPool::Prefetch) {
        stats.prefetchMemoryRequests++;
    }
    if (stats.maxMemoryInflight.value() < memoryInflight) {
        stats.maxMemoryInflight = memoryInflight;
    }
    return true;
}

void
MptUnit::recvReqRetry()
{
    if (!blockedMshr.has_value()) {
        return;
    }
    const MshrRef ref = *blockedMshr;
    if (!validMshr(ref)) {
        blockedMshr.reset();
        scheduleService();
        return;
    }
    Mshr &mshr = getMshr(ref);
    if (mshr.inFlight || mshr.packet == nullptr) {
        blockedMshr.reset();
        scheduleService();
        return;
    }
    stats.memoryRetries++;
    if (memoryInflight < maxMemoryInflight && sendMshr(ref)) {
        blockedMshr.reset();
        removeReadyMshr(ref);
        scheduleService();
    }
}

bool
MptUnit::recvTimingResp(PacketPtr pkt)
{
    auto *sender = dynamic_cast<MptSenderState *>(pkt->popSenderState());
    panic_if(sender == nullptr, "MPT response has invalid sender state\n");
    const unsigned slot = sender->slot;
    const uint64_t generation = sender->generation;
    const MshrRef ref{sender->pool, slot};
    delete sender;

    panic_if(!validMshr(ref),
             "MPT response has invalid MSHR slot %u\n", slot);
    Mshr &mshr = getMshr(ref);
    panic_if(!mshr.allocated || !mshr.inFlight ||
             mshr.generation != generation,
             "Stale or unmatched MPT response for slot %u generation %llu\n",
             slot, static_cast<unsigned long long>(generation));
    panic_if(memoryInflight == 0,
             "MPT memory in-flight counter underflow\n");

    --memoryInflight;
    stats.memoryLatency += ticksToCycles(curTick() - mshr.issueTick);
    const MshrKey key = mshr.key;
    std::vector<Target> targets = std::move(mshr.targets);
    std::optional<Target> prefetch_target = mshr.prefetchTarget;
    const bool response_error = pkt->isError();
    const uint64_t raw = response_error ? 0 : pkt->getLE<uint64_t>();
    releaseMshr(ref);
    delete pkt;

    if (key.epoch != epoch || key.rootPpn != mmpt.ppn) {
        stats.staleEpochResponses++;
        for (auto &target : targets) {
            restartTarget(target);
        }
    } else {
        const bool demand_merged = !targets.empty();
        if (prefetch_target.has_value()) {
            consumePrefetchedMpte(*prefetch_target, raw, demand_merged);
        }
        for (auto &target : targets) {
            consumeMpte(target, raw);
        }

        const MPTE52 mpte(raw);
        const auto trigger = std::find_if(
            targets.begin(), targets.end(), [](const Target &target) {
                return target.source != MptRequestSource::Ptw;
            });
        if (!response_error && mpte.isValid() &&
            (mpte.isLeaf() || key.level > 0) && trigger != targets.end()) {
            queuePrefetches(*trigger);
        }
    }

    scheduleService();
    return true;
}

void
MptUnit::releaseMshr(const MshrRef &ref)
{
    Mshr &mshr = getMshr(ref);
    if (!mshr.allocated) {
        return;
    }
    if (ref.pool == MshrPool::Prefetch) {
        prefetchMshrIndex.erase(mshr.key);
    } else {
        mshrIndex.erase(mshr.key);
    }
    removeReadyMshr(ref);
    mshr.allocated = false;
    mshr.inFlight = false;
    mshr.packet = nullptr;
    mshr.targets.clear();
    mshr.prefetchTarget.reset();
    mshr.prefetchUseful = false;
    mshr.issueTick = 0;
}

void
MptUnit::discardPacket(PacketPtr pkt)
{
    if (pkt == nullptr) {
        return;
    }
    if (pkt->senderState != nullptr) {
        delete pkt->popSenderState();
    }
    delete pkt;
}

void
MptUnit::consumePrefetchedMpte(const Target &target, uint64_t raw,
                               bool demand_merged)
{
    if (target.epoch != epoch || target.rootPpn != mmpt.ppn) {
        stats.prefetchDropped++;
        return;
    }

    const MPTE52 mpte(raw);
    if (!mpte.isValid() || (!mpte.isLeaf() && target.level == 0)) {
        stats.prefetchDropped++;
        return;
    }

    if (insertCache(target.level, target.paddr, mpte, !demand_merged)) {
        stats.prefetchFilled++;
    } else {
        stats.prefetchDropped++;
    }
}

void
MptUnit::consumeMpte(Target target, uint64_t raw)
{
    if (target.epoch != epoch || target.rootPpn != mmpt.ppn) {
        restartTarget(target);
        return;
    }

    const MPTE52 mpte(raw);
    ++target.depth;
    if (!mpte.isValid() || (!mpte.isLeaf() && target.level == 0)) {
        MptResult result;
        result.allowed = false;
        result.valid = false;
        result.level = target.level;
        completeTarget(target, result);
        return;
    }

    insertCache(target.level, target.paddr, mpte);
    if (mpte.isLeaf()) {
        const uint8_t pi =
            (target.paddr >> getPageShiftForLevel(target.level)) & 0xf;
        const uint8_t permission = mptEffectivePerm(mpte.perms(pi));
        MptResult result;
        result.allowed = mptPermAllowsAccess(permission, target.mode);
        result.valid = true;
        result.permission = permission;
        result.level = target.level;
        completeTarget(target, result);
        return;
    }

    target.tableBase = mpte.nextLevelPAddr();
    --target.level;
    if (!startTargetRead(target)) {
        pendingWalks.push_back(target);
    }
}

void
MptUnit::completeTarget(const Target &target, const MptResult &result)
{
    if (target.client == nullptr) {
        return;
    }
    stats.completedLookups++;
    stats.totalLookupLatency += ticksToCycles(curTick() - target.enqueueTick);
    const unsigned depth = std::min<unsigned>(target.depth, NumLevels);
    stats.walkDepth[depth]++;
    target.client->finishMptLookup(result);
}

void
MptUnit::restartTarget(Target target)
{
    target.epoch = epoch;
    target.rootPpn = mmpt.ppn;
    target.level = 3;
    target.tableBase = mmpt.ppn << PageShift;
    target.depth = 0;
    if (!enabled()) {
        bypassCompletions.push_back(target);
    } else {
        arrivals[sourceIndex(target.source)].push_back(target);
    }
}

void
MptUnit::invalidateForNewEpoch()
{
    ++epoch;
    clearCache();
    stats.fenceFlushes++;

    std::vector<Target> restart;
    stats.prefetchDropped += prefetchQueue.size();
    prefetchQueue.clear();

    auto invalidate_pool = [this, &restart](auto &pool, MshrPool pool_type) {
        for (unsigned slot = 0; slot < pool.size(); ++slot) {
            Mshr &mshr = pool[slot];
            if (!mshr.allocated) {
                continue;
            }
            restart.insert(restart.end(), mshr.targets.begin(),
                           mshr.targets.end());
            mshr.targets.clear();
            if (pool_type == MshrPool::Prefetch) {
                stats.prefetchDropped++;
            }
            if (!mshr.inFlight) {
                discardPacket(mshr.packet);
                mshr.packet = nullptr;
                releaseMshr(MshrRef{pool_type, slot});
            }
        }
    };
    invalidate_pool(mshrs, MshrPool::Demand);
    invalidate_pool(prefetchMshrs, MshrPool::Prefetch);

    readyMshrs.erase(
        std::remove_if(readyMshrs.begin(), readyMshrs.end(),
                       [this](const MshrRef &ref) {
                           return !validMshr(ref);
                       }),
        readyMshrs.end());
    readyPrefetchMshrs.erase(
        std::remove_if(readyPrefetchMshrs.begin(),
                       readyPrefetchMshrs.end(),
                       [this](unsigned slot) {
                           return !validMshr(
                               MshrRef{MshrPool::Prefetch, slot});
                       }),
        readyPrefetchMshrs.end());
    if (blockedMshr.has_value() && !validMshr(*blockedMshr)) {
        blockedMshr.reset();
    }

    for (auto &target : restart) {
        restartTarget(target);
    }
    scheduleService();
}

void
MptUnit::flush()
{
    invalidateForNewEpoch();
}

void
MptUnit::cancel(MptClient *client)
{
    if (client == nullptr) {
        return;
    }
    bool removed = false;
    auto erase_targets = [&removed, client](auto &container) {
        const auto old_size = container.size();
        container.erase(
            std::remove_if(container.begin(), container.end(),
                           [client](const auto &entry) {
                               if constexpr (std::is_same_v<
                                       std::decay_t<decltype(entry)>,
                                       PipelineEntry>) {
                                   return entry.target.client == client;
                               } else {
                                   return entry.client == client;
                               }
                           }),
            container.end());
        removed |= container.size() != old_size;
    };

    for (unsigned source = 0; source < NumSources; ++source) {
        erase_targets(arrivals[source]);
        erase_targets(lookupQueues[source]);
    }
    erase_targets(pipeline);
    erase_targets(pendingWalks);
    erase_targets(bypassCompletions);

    auto cancel_targets = [&removed, client](Mshr &mshr) {
        const auto old_size = mshr.targets.size();
        mshr.targets.erase(
            std::remove_if(mshr.targets.begin(), mshr.targets.end(),
                           [client](const Target &target) {
                               return target.client == client;
                           }),
            mshr.targets.end());
        removed |= old_size != mshr.targets.size();
    };

    for (unsigned slot = 0; slot < mshrs.size(); ++slot) {
        Mshr &mshr = mshrs[slot];
        if (!mshr.allocated) {
            continue;
        }
        cancel_targets(mshr);
        if (mshr.targets.empty() && !mshr.inFlight) {
            discardPacket(mshr.packet);
            mshr.packet = nullptr;
            releaseMshr(MshrRef{MshrPool::Demand, slot});
        }
    }

    for (unsigned slot = 0; slot < prefetchMshrs.size(); ++slot) {
        Mshr &mshr = prefetchMshrs[slot];
        if (!mshr.allocated) {
            continue;
        }
        const bool had_targets = !mshr.targets.empty();
        cancel_targets(mshr);
        if (had_targets && mshr.targets.empty() && !mshr.inFlight) {
            const MshrRef ref{MshrPool::Prefetch, slot};
            removeReadyMshr(ref);
            if ((!blockedMshr.has_value() || *blockedMshr != ref) &&
                mshr.packet != nullptr) {
                readyPrefetchMshrs.push_back(slot);
            }
        }
    }

    readyMshrs.erase(
        std::remove_if(readyMshrs.begin(), readyMshrs.end(),
                       [this](const MshrRef &ref) {
                           return !validMshr(ref);
                       }),
        readyMshrs.end());
    readyPrefetchMshrs.erase(
        std::remove_if(readyPrefetchMshrs.begin(),
                       readyPrefetchMshrs.end(),
                       [this](unsigned slot) {
                           return !validMshr(
                               MshrRef{MshrPool::Prefetch, slot});
                       }),
        readyPrefetchMshrs.end());
    if (blockedMshr.has_value() && !validMshr(*blockedMshr)) {
        blockedMshr.reset();
    }
    if (removed) {
        stats.squashes++;
    }
}

void
MptUnit::syncMMPT(ThreadContext *tc)
{
    panic_if(tc == nullptr, "Cannot synchronize MMPT without a ThreadContext\n");
    const MMPT previous = mmpt;
    MMPT observed = tc->readMiscReg(MISCREG_MMPT);
    if (observed.mode != 0 && observed.ppn == 0) {
        mmpt = observed;
        ensureSimulatedMptTree(tc);
        observed = tc->readMiscReg(MISCREG_MMPT);
        mmpt = previous;
    }

    if (static_cast<uint64_t>(observed) != static_cast<uint64_t>(mmpt)) {
        mmpt = observed;
        invalidateForNewEpoch();
    }
}

Addr
MptUnit::buildSimulatedMptTree()
{
    static constexpr Addr IndexMask = MptRootEntries - 1;
    auto mpt_index = [](Addr paddr, int level) {
        return (paddr >> (getPageShiftForLevel(level) + 4)) & IndexMask;
    };
    auto l1_key = [](Addr root_idx, Addr l2_idx) {
        return (root_idx << 9) | l2_idx;
    };
    auto l0_key = [](Addr root_idx, Addr l2_idx, Addr l1_idx) {
        return (root_idx << 18) | (l2_idx << 9) | l1_idx;
    };

    const AddrRangeList ranges = system->getPhysMem().getConfAddrRanges();
    const Addr l0_coverage =
        MptRootEntries * getRegionSizeForLevel(0);
    std::map<Addr, Addr> l2_tables;
    std::map<Addr, Addr> l1_tables;
    std::map<Addr, Addr> l0_tables;

    for (const auto &range : ranges) {
        if (!range.valid() || range.size() == 0) {
            continue;
        }
        Addr chunk = range.start() & ~(l0_coverage - 1);
        while (chunk <= range.end()) {
            const Addr root_idx = mpt_index(chunk, 3);
            const Addr l2_idx = mpt_index(chunk, 2);
            const Addr l1_idx = mpt_index(chunk, 1);
            l2_tables.emplace(root_idx, 0);
            l1_tables.emplace(l1_key(root_idx, l2_idx), 0);
            l0_tables.emplace(l0_key(root_idx, l2_idx, l1_idx), 0);
            if (chunk > MaxAddr - l0_coverage) {
                break;
            }
            chunk += l0_coverage;
        }
    }

    const uint64_t table_pages =
        1 + l2_tables.size() + l1_tables.size() + l0_tables.size();
    const uint64_t table_bytes = table_pages * PageBytes;
    const AddrRangeList &reserved = system->getMptReservedMemRanges();
    panic_if(reserved.empty(),
             "Cannot build simulated MPT tree: no explicit reserved memory "
             "for %llu pages (%llu bytes)\n",
             static_cast<unsigned long long>(table_pages),
             static_cast<unsigned long long>(table_bytes));

    Addr selected = 0;
    bool found = false;
    for (const auto &range : reserved) {
        if (!range.valid() || range.size() < table_bytes) {
            continue;
        }
        const Addr candidate =
            (range.start() + PageBytes - 1) & ~(PageBytes - 1);
        if (candidate > MaxAddr - table_bytes + 1) {
            continue;
        }
        if (range.contains(candidate) &&
            range.contains(candidate + table_bytes - 1)) {
            selected = candidate;
            found = true;
            break;
        }
    }
    panic_if(!found,
             "Cannot build simulated MPT tree: reserved memory is too small "
             "for %llu pages (%llu bytes)\n",
             static_cast<unsigned long long>(table_pages),
             static_cast<unsigned long long>(table_bytes));
    panic_if(!system->isMemAddr(selected) ||
             !system->isMemAddr(selected + table_bytes - 1),
             "Simulated MPT reserved range [%#lx, %#lx] is not backed by "
             "physical memory\n", selected, selected + table_bytes - 1);
    panic_if(selected & (PageBytes - 1),
             "Simulated MPT root %#lx is not page aligned\n", selected);
    panic_if((selected >> PageShift) & ~MptPpnMask,
             "Simulated MPT root PPN %#lx exceeds the MMPT field\n",
             selected >> PageShift);

    Addr next_table = selected + PageBytes;
    auto alloc_table = [&next_table]() {
        const Addr table = next_table;
        next_table += PageBytes;
        return table;
    };
    auto write_mpte = [this](Addr table, Addr index, uint64_t value) {
        system->physProxy.write<uint64_t>(
            table + index * MPT_MPTE_SIZE, value,
            system->getGuestByteOrder());
    };
    auto clear_table = [&write_mpte](Addr table) {
        for (unsigned i = 0; i < MptRootEntries; ++i) {
            write_mpte(table, i, 0);
        }
    };
    auto fill_leaf_table = [&write_mpte](Addr table) {
        for (unsigned i = 0; i < MptRootEntries; ++i) {
            write_mpte(table, i, MptAllowAllLeafMpte);
        }
    };

    clear_table(selected);
    for (auto &table : l2_tables) {
        table.second = alloc_table();
        clear_table(table.second);
    }
    for (auto &table : l1_tables) {
        table.second = alloc_table();
        clear_table(table.second);
    }
    for (auto &table : l0_tables) {
        table.second = alloc_table();
        fill_leaf_table(table.second);
    }
    assert(next_table == selected + table_bytes);

    for (const auto &table : l2_tables) {
        write_mpte(selected, table.first, makeInternalMpte(table.second));
    }
    for (const auto &table : l1_tables) {
        const Addr root_idx = table.first >> 9;
        const Addr l2_idx = table.first & IndexMask;
        write_mpte(l2_tables[root_idx], l2_idx,
                   makeInternalMpte(table.second));
    }
    for (const auto &table : l0_tables) {
        const Addr root_idx = table.first >> 18;
        const Addr l2_idx = (table.first >> 9) & IndexMask;
        const Addr l1_idx = table.first & IndexMask;
        write_mpte(l1_tables[l1_key(root_idx, l2_idx)], l1_idx,
                   makeInternalMpte(table.second));
    }

    DPRINTF(PageTableWalker,
            "Built per-core simulated L0-leaf MPT at %#lx: %llu pages\n",
            selected, static_cast<unsigned long long>(table_pages));
    return selected;
}

void
MptUnit::ensureSimulatedMptTree(ThreadContext *tc)
{
    if (mmpt.mode == 0 || mmpt.ppn != 0) {
        return;
    }
    if (!simulatedTreeBuilt) {
        simulatedRootPaddr = buildSimulatedMptTree();
        simulatedTreeBuilt = true;
    }
    MMPT updated = mmpt;
    updated.ppn = simulatedRootPaddr >> PageShift;
    tc->setMiscRegNoEffect(MISCREG_MMPT, updated);
    mmpt = updated;
}

bool
MptUnit::checkFunctional(Addr paddr, BaseMMU::Mode mode) const
{
    if (!enabled()) {
        return true;
    }
    Addr base = mmpt.ppn << PageShift;
    for (int level = 3; level >= 0; --level) {
        const size_t shift = getPageShiftForLevel(level) + 4;
        const size_t index = (paddr >> shift) & 0x1ff;
        const Addr mpte_paddr = base + index * MPT_MPTE_SIZE;
        const uint64_t raw = system->physProxy.read<uint64_t>(
            mpte_paddr, system->getGuestByteOrder());
        const MPTE52 mpte(raw);
        if (!mpte.isValid()) {
            return false;
        }
        if (mpte.isLeaf()) {
            const uint8_t pi =
                (paddr >> getPageShiftForLevel(level)) & 0xf;
            return mptPermAllowsAccess(
                mptEffectivePerm(mpte.perms(pi)), mode);
        }
        if (level == 0) {
            return false;
        }
        base = mpte.nextLevelPAddr();
    }
    return false;
}

} // namespace RiscvISA
} // namespace gem5
